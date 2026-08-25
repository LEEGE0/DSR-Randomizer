#include "modules/DeferredModuleGate.h"

#include <Windows.h>
#include <bcrypt.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <utility>

#include "steam/SteamPolicy.h"

namespace DSRRandomizer::Modules {
namespace {

using LoadLibraryAFunction = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFunction = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFunction = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFunction = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetProcAddressFunction = FARPROC(WINAPI*)(HMODULE, LPCSTR);

struct UniqueHandle final {
    HANDLE value = INVALID_HANDLE_VALUE;

    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : value(handle) {}
    ~UniqueHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) {
                CloseHandle(value);
            }
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};

struct ProtectedExport {
    std::string name;
    std::size_t slot = Steam::kSteamFactorySlotCapacity;
    void* target = nullptr;
};

struct ModuleRecord {
    std::wstring canonicalPath;
    std::wstring baseName;
    UniqueHandle expectedFile;
    std::array<std::uint8_t, 32> expectedSha256{};
    bool allowDeferred = false;
    std::vector<std::string> declaredInterfaces;
    std::vector<ProtectedExport> protectedExports;
    std::mutex admissionMutex;
    HMODULE admittedModule = nullptr;
    HMODULE pinnedModule = nullptr;
    bool admitted = false;

    ~ModuleRecord() {
        if (pinnedModule != nullptr) {
            FreeLibrary(pinnedModule);
        }
    }
};

struct Trampolines {
    LoadLibraryAFunction loadLibraryA = nullptr;
    LoadLibraryWFunction loadLibraryW = nullptr;
    LoadLibraryExAFunction loadLibraryExA = nullptr;
    LoadLibraryExWFunction loadLibraryExW = nullptr;
    GetProcAddressFunction getProcAddress = nullptr;
};

struct GateContext {
    std::vector<std::unique_ptr<ModuleRecord>> modules;
    Steam::FatalReporter fatalReporter = nullptr;
    Trampolines trampolines;
    std::atomic<bool> denyOnly{false};
    std::mutex slotMutex;
    std::array<bool, Steam::kSteamFactorySlotCapacity> slots{};
};

struct HookEntry {
    void* target = nullptr;
    bool created = false;
    bool factory = false;
    std::size_t factorySlot = Steam::kSteamFactorySlotCapacity;
};

struct Lifecycle {
    std::shared_ptr<GateContext> context;
    std::vector<HookEntry> hooks;
    bool initialized = false;
    bool ownsInitialization = false;
    bool mayBeEnabled = false;
};

std::atomic<std::shared_ptr<GateContext>> activeContext;
std::atomic<bool> gateInstalled{false};
Lifecycle lifecycle;
std::mutex installMutex;
std::mutex minHookMutex;
std::mutex hooksMutex;
std::shared_mutex callbackGate;
thread_local std::uint32_t internalBypassDepth = 0;

class InternalBypass final {
public:
    InternalBypass() noexcept { ++internalBypassDepth; }
    ~InternalBypass() { --internalBypassDepth; }
    InternalBypass(const InternalBypass&) = delete;
    InternalBypass& operator=(const InternalBypass&) = delete;
};

bool EqualsPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(INT_MAX)
        || right.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

std::wstring BaseName(const std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    return Lowercase(std::wstring(
        slash == std::wstring_view::npos ? path : path.substr(slash + 1)));
}

bool ReadModulePath(const HMODULE module, std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(
        module,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

bool ReadCanonicalPath(const HANDLE file, std::wstring& path) {
    const auto required = GetFinalPathNameByHandleW(file, nullptr, 0, 0);
    if (required == 0) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
    const auto length = GetFinalPathNameByHandleW(
        file,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        0);
    if (length == 0 || length >= buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

UniqueHandle OpenPinnedReadOnly(const std::wstring& path) noexcept {
    return UniqueHandle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
}

bool IsCanonicalAbsolutePath(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':'
        || (path[2] != L'\\' && path[2] != L'/')
        || path.find(L'~') != std::wstring::npos) {
        return false;
    }
    std::vector<wchar_t> buffer(32768);
    const auto length = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    return length != 0 && length < buffer.size()
        && EqualsPath(path, std::wstring_view(buffer.data(), length));
}

bool HashFileHandle(
    const HANDLE file,
    std::array<std::uint8_t, 32>& digest) noexcept {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD bytes = 0;
    std::vector<std::uint8_t> object;
    bool ok = BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0) >= 0
        && BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &bytes,
            0) >= 0;
    try {
        if (ok) {
            object.resize(objectLength);
            ok = BCryptCreateHash(
                algorithm,
                &hash,
                object.data(),
                static_cast<ULONG>(object.size()),
                nullptr,
                0,
                0) >= 0;
        }
        std::array<std::uint8_t, 4096> buffer{};
        while (ok) {
            DWORD read = 0;
            if (!ReadFile(
                    file,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }
            ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
        }
        if (ok) {
            ok = BCryptFinishHash(
                hash,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0) >= 0;
        }
    }
    catch (...) {
        ok = false;
    }
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return ok;
}

bool Fatal(
    const std::shared_ptr<GateContext>& context,
    const char* const code) noexcept {
    if (context != nullptr && context->fatalReporter != nullptr) {
        context->denyOnly.store(true, std::memory_order_release);
        context->fatalReporter(code);
    }
    return false;
}

std::size_t AllocateFactorySlot(GateContext& context) noexcept {
    std::scoped_lock lock(context.slotMutex);
    const auto available = std::find(
        context.slots.begin(),
        context.slots.end(),
        false);
    if (available == context.slots.end()) {
        return Steam::kSteamFactorySlotCapacity;
    }
    *available = true;
    return static_cast<std::size_t>(available - context.slots.begin());
}

void ReleaseFactorySlot(GateContext& context, const std::size_t slot) noexcept {
    std::scoped_lock lock(context.slotMutex);
    if (slot < context.slots.size()) {
        context.slots[slot] = false;
    }
}

void RollBackFactoryHooks(
    GateContext& context,
    std::vector<HookEntry>& created) noexcept {
    std::scoped_lock hookLock(minHookMutex);
    for (auto& hook : created) {
        if (hook.target != nullptr) {
            static_cast<void>(MH_DisableHook(hook.target));
        }
    }
    Steam::SteamFactoryCallbackBlock callbackBlock;
    for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator) {
        if (iterator->target != nullptr) {
            static_cast<void>(MH_RemoveHook(iterator->target));
        }
        if (iterator->factorySlot < Steam::kSteamFactorySlotCapacity) {
            Steam::UnregisterSteamFactorySlot(iterator->factorySlot);
            ReleaseFactorySlot(context, iterator->factorySlot);
        }
    }
    created.clear();
}

bool ProtectFactoryExports(
    const std::shared_ptr<GateContext>& context,
    ModuleRecord& record,
    const HMODULE module) noexcept {
    std::vector<HookEntry> created;
    try {
        std::scoped_lock hookLock(minHookMutex);
        for (auto& exportEntry : record.protectedExports) {
            const auto slot = AllocateFactorySlot(*context);
            if (slot >= Steam::kSteamFactorySlotCapacity
                || Steam::RegisterSteamFactorySlot(
                    slot,
                    record.declaredInterfaces,
                    context->fatalReporter)
                    != Steam::SteamFactorySlotStatus::Success) {
                if (slot < Steam::kSteamFactorySlotCapacity) {
                    ReleaseFactorySlot(*context, slot);
                }
                break;
            }

            void* const target = reinterpret_cast<void*>(
                context->trampolines.getProcAddress(module, exportEntry.name.c_str()));
            void* const detour = Steam::SteamFactoryDetourAddress(slot);
            void* original = nullptr;
            if (target == nullptr || detour == nullptr
                || MH_CreateHook(target, detour, &original) != MH_OK
                || !Steam::SetSteamFactoryOriginal(
                    slot,
                    reinterpret_cast<Steam::Synthetic::FactoryFunction>(original))
                || MH_QueueEnableHook(target) != MH_OK) {
                if (target != nullptr) {
                    static_cast<void>(MH_RemoveHook(target));
                }
                Steam::UnregisterSteamFactorySlot(slot);
                ReleaseFactorySlot(*context, slot);
                break;
            }
            created.push_back({target, true, true, slot});
            exportEntry.slot = slot;
            exportEntry.target = target;
        }

        if (created.size() != record.protectedExports.size()
            || MH_ApplyQueued() != MH_OK) {
            // Rollback occurs after releasing the MinHook serialization lock.
        }
        else {
            std::scoped_lock lifecycleLock(hooksMutex);
            lifecycle.mayBeEnabled = true;
            lifecycle.hooks.insert(
                lifecycle.hooks.end(),
                created.begin(),
                created.end());
            return true;
        }
    }
    catch (...) {
    }
    RollBackFactoryHooks(*context, created);
    return Fatal(context, "STEAM_HOOK_FAILED");
}

struct AdmissionResult {
    bool target = false;
    bool admitted = false;
    ModuleRecord* record = nullptr;
};

AdmissionResult AdmitModuleCore(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module) {
    if (context == nullptr || module == nullptr) {
        return {};
    }
    const auto alreadyAdmitted = std::find_if(
        context->modules.begin(),
        context->modules.end(),
        [module](const std::unique_ptr<ModuleRecord>& record) {
            std::scoped_lock admissionLock(record->admissionMutex);
            return record->admitted && record->admittedModule == module;
        });
    if (alreadyAdmitted != context->modules.end()) {
        return {true, true, alreadyAdmitted->get()};
    }
    std::wstring modulePath;
    if (!ReadModulePath(module, modulePath)) {
        return {};
    }
    const auto baseName = BaseName(modulePath);
    const auto found = std::find_if(
        context->modules.begin(),
        context->modules.end(),
        [&baseName](const std::unique_ptr<ModuleRecord>& record) {
            return record->baseName == baseName;
        });
    if (found == context->modules.end()) {
        return {};
    }
    ModuleRecord& record = **found;
    std::scoped_lock admissionLock(record.admissionMutex);
    if (record.admitted) {
        if (record.admittedModule == module) {
            return {true, true, &record};
        }
        return {true, Fatal(context, "STEAM_MODULE_PATH_MISMATCH"), &record};
    }
    if (context->denyOnly.load(std::memory_order_acquire)) {
        return {true, Fatal(context, "STEAM_GATE_UNAVAILABLE"), &record};
    }

    InternalBypass bypass;
    auto actualFile = OpenPinnedReadOnly(modulePath);
    std::wstring canonicalActual;
    if (actualFile.value == INVALID_HANDLE_VALUE
        || !ReadCanonicalPath(actualFile.value, canonicalActual)
        || !EqualsPath(canonicalActual, record.canonicalPath)) {
        return {true, Fatal(context, "STEAM_MODULE_PATH_MISMATCH"), &record};
    }
    std::array<std::uint8_t, 32> actualHash{};
    if (!HashFileHandle(actualFile.value, actualHash)
        || !std::equal(
            actualHash.begin(),
            actualHash.end(),
            record.expectedSha256.begin())) {
        return {true, Fatal(context, "STEAM_MODULE_HASH_MISMATCH"), &record};
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module),
            &pinned)
        || pinned != module) {
        if (pinned != nullptr) {
            FreeLibrary(pinned);
        }
        return {true, Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE"), &record};
    }
    record.pinnedModule = pinned;
    if (!ProtectFactoryExports(context, record, module)) {
        return {true, false, &record};
    }
    record.admittedModule = module;
    record.admitted = true;
    return {true, true, &record};
}

AdmissionResult AdmitModule(
    const std::shared_ptr<GateContext>& context,
    const HMODULE module) noexcept {
    try {
        return AdmitModuleCore(context, module);
    }
    catch (...) {
        return {true, Fatal(context, "STEAM_GATE_UNAVAILABLE"), nullptr};
    }
}

bool RequestedTarget(
    const std::shared_ptr<GateContext>& context,
    const std::wstring_view requested) noexcept {
    try {
        if (context == nullptr || requested.empty()) {
            return false;
        }
        const auto baseName = BaseName(requested);
        return std::any_of(
            context->modules.begin(),
            context->modules.end(),
            [&baseName](const std::unique_ptr<ModuleRecord>& record) {
                return record->baseName == baseName;
            });
    }
    catch (...) {
        static_cast<void>(Fatal(context, "STEAM_GATE_UNAVAILABLE"));
        return true;
    }
}

bool RequestedTarget(
    const std::shared_ptr<GateContext>& context,
    const char* const requested) noexcept {
    try {
        if (requested == nullptr || *requested == '\0') {
            return false;
        }
        const auto required = MultiByteToWideChar(
            CP_ACP,
            MB_ERR_INVALID_CHARS,
            requested,
            -1,
            nullptr,
            0);
        if (required <= 1) {
            return false;
        }
        std::wstring wide(static_cast<std::size_t>(required), L'\0');
        if (MultiByteToWideChar(
                CP_ACP,
                MB_ERR_INVALID_CHARS,
                requested,
                -1,
                wide.data(),
                required) != required) {
            return false;
        }
        wide.resize(static_cast<std::size_t>(required - 1));
        return RequestedTarget(context, wide);
    }
    catch (...) {
        static_cast<void>(Fatal(context, "STEAM_GATE_UNAVAILABLE"));
        return true;
    }
}

bool AdmitAfterLoad(
    const std::shared_ptr<GateContext>& context,
    const HMODULE returnedModule,
    const bool requestedTarget) noexcept {
    const auto returnedAdmission = AdmitModule(context, returnedModule);
    if (returnedAdmission.target && !returnedAdmission.admitted) {
        return false;
    }
    if (!returnedAdmission.target && requestedTarget) {
        static_cast<void>(Fatal(context, "STEAM_MODULE_IDENTITY_UNAVAILABLE"));
        return false;
    }

    // A LoadLibrary call may map a protected module as a dependency while
    // returning the outer module. Protect every newly present expectation
    // before releasing that outer handle to its caller.
    for (const auto& record : context->modules) {
        bool admitted = false;
        {
            std::scoped_lock admissionLock(record->admissionMutex);
            admitted = record->admitted;
        }
        if (admitted) {
            continue;
        }
        const HMODULE loaded = GetModuleHandleW(record->baseName.c_str());
        if (loaded != nullptr) {
            const auto dependencyAdmission = AdmitModule(context, loaded);
            if (!dependencyAdmission.target || !dependencyAdmission.admitted) {
                return false;
            }
        }
    }
    return true;
}

HMODULE WINAPI HookLoadLibraryA(const LPCSTR fileName) {
    std::shared_lock callbackLock(callbackGate);
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->trampolines.loadLibraryA == nullptr) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const HMODULE module = context->trampolines.loadLibraryA(fileName);
    if (internalBypassDepth != 0 || module == nullptr) {
        return module;
    }
    return AdmitAfterLoad(context, module, RequestedTarget(context, fileName))
        ? module
        : nullptr;
}

HMODULE WINAPI HookLoadLibraryW(const LPCWSTR fileName) {
    std::shared_lock callbackLock(callbackGate);
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->trampolines.loadLibraryW == nullptr) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const HMODULE module = context->trampolines.loadLibraryW(fileName);
    if (internalBypassDepth != 0 || module == nullptr) {
        return module;
    }
    return AdmitAfterLoad(context, module, RequestedTarget(context, fileName))
        ? module
        : nullptr;
}

HMODULE WINAPI HookLoadLibraryExA(
    const LPCSTR fileName,
    const HANDLE file,
    const DWORD flags) {
    std::shared_lock callbackLock(callbackGate);
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->trampolines.loadLibraryExA == nullptr) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const HMODULE module = context->trampolines.loadLibraryExA(fileName, file, flags);
    if (internalBypassDepth != 0 || module == nullptr) {
        return module;
    }
    return AdmitAfterLoad(context, module, RequestedTarget(context, fileName))
        ? module
        : nullptr;
}

HMODULE WINAPI HookLoadLibraryExW(
    const LPCWSTR fileName,
    const HANDLE file,
    const DWORD flags) {
    std::shared_lock callbackLock(callbackGate);
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->trampolines.loadLibraryExW == nullptr) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const HMODULE module = context->trampolines.loadLibraryExW(fileName, file, flags);
    if (internalBypassDepth != 0 || module == nullptr) {
        return module;
    }
    return AdmitAfterLoad(context, module, RequestedTarget(context, fileName))
        ? module
        : nullptr;
}

FARPROC WINAPI HookGetProcAddress(
    const HMODULE module,
    const LPCSTR procedureName) {
    std::shared_lock callbackLock(callbackGate);
    const auto context = activeContext.load(std::memory_order_acquire);
    if (context == nullptr || context->trampolines.getProcAddress == nullptr) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    if (internalBypassDepth != 0) {
        return context->trampolines.getProcAddress(module, procedureName);
    }
    const auto admission = AdmitModule(context, module);
    if (!admission.target) {
        return context->trampolines.getProcAddress(module, procedureName);
    }
    if (!admission.admitted || admission.record == nullptr) {
        return nullptr;
    }
    if (reinterpret_cast<std::uintptr_t>(procedureName) <= 0xffffU) {
        static_cast<void>(Fatal(context, "STEAM_SYMBOL_UNSUPPORTED"));
        return nullptr;
    }
    const std::string_view requested(procedureName);
    const auto found = std::find_if(
        admission.record->protectedExports.begin(),
        admission.record->protectedExports.end(),
        [requested](const ProtectedExport& exportEntry) {
            return exportEntry.name == requested;
        });
    if (found != admission.record->protectedExports.end()) {
        return reinterpret_cast<FARPROC>(
            Steam::SteamFactoryDetourAddress(found->slot));
    }
    return context->trampolines.getProcAddress(module, procedureName);
}

struct LoaderHookDefinition {
    const char* procedure;
    void* detour;
    void** original;
};

std::array<LoaderHookDefinition, 5> LoaderHooks(GateContext& context) noexcept {
    return {{
        {"LoadLibraryA", reinterpret_cast<void*>(&HookLoadLibraryA),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryA)},
        {"LoadLibraryW", reinterpret_cast<void*>(&HookLoadLibraryW),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryW)},
        {"LoadLibraryExA", reinterpret_cast<void*>(&HookLoadLibraryExA),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryExA)},
        {"LoadLibraryExW", reinterpret_cast<void*>(&HookLoadLibraryExW),
         reinterpret_cast<void**>(&context.trampolines.loadLibraryExW)},
        {"GetProcAddress", reinterpret_cast<void*>(&HookGetProcAddress),
         reinterpret_cast<void**>(&context.trampolines.getProcAddress)},
    }};
}

bool BuildContext(
    const DeferredModuleGateConfiguration& configuration,
    std::shared_ptr<GateContext>& context) {
    if (configuration.modules.empty()
        || configuration.modules.size() > Steam::kSteamFactorySlotCapacity
        || configuration.fatalReporter == nullptr) {
        return false;
    }
    auto candidate = std::make_shared<GateContext>();
    candidate->fatalReporter = configuration.fatalReporter;
    const Steam::SteamPolicy policy;
    std::size_t exportCount = 0;
    for (const auto& source : configuration.modules) {
        if (!IsCanonicalAbsolutePath(source.expectedPath)
            || source.declaredInterfaces.empty()
            || source.protectedFactoryExports.empty()) {
            return false;
        }
        exportCount += source.protectedFactoryExports.size();
        if (exportCount > Steam::kSteamFactorySlotCapacity) {
            return false;
        }
        auto record = std::make_unique<ModuleRecord>();
        record->expectedFile = OpenPinnedReadOnly(source.expectedPath);
        if (record->expectedFile.value == INVALID_HANDLE_VALUE
            || !ReadCanonicalPath(
                record->expectedFile.value,
                record->canonicalPath)) {
            return false;
        }
        const std::wstring_view finalPath(record->canonicalPath);
        if (!finalPath.starts_with(L"\\\\?\\")
            || !EqualsPath(source.expectedPath, finalPath.substr(4))) {
            return false;
        }
        record->baseName = BaseName(record->canonicalPath);
        record->expectedSha256 = source.expectedSha256;
        record->allowDeferred = source.allowDeferred;
        record->declaredInterfaces = source.declaredInterfaces;
        for (const auto& version : record->declaredInterfaces) {
            const auto decision = policy.EvaluateInterface(version);
            if (version.empty()
                || decision == Steam::InterfaceDecision::UnknownProtectedFatal
                || decision == Steam::InterfaceDecision::Unrecognized) {
                return false;
            }
        }
        if (std::any_of(
                record->declaredInterfaces.begin(),
                record->declaredInterfaces.end(),
                [&record](const std::string& version) {
                    return std::count(
                        record->declaredInterfaces.begin(),
                        record->declaredInterfaces.end(),
                        version) != 1;
                })) {
            return false;
        }
        for (const auto& name : source.protectedFactoryExports) {
            if (name.empty()
                || std::any_of(
                    name.begin(),
                    name.end(),
                    [](const char character) {
                        return std::isspace(
                            static_cast<unsigned char>(character)) != 0;
                    })) {
                return false;
            }
            record->protectedExports.push_back({name});
        }
        if (std::any_of(
                record->protectedExports.begin(),
                record->protectedExports.end(),
                [&record](const ProtectedExport& exportEntry) {
                    return std::count_if(
                        record->protectedExports.begin(),
                        record->protectedExports.end(),
                        [&exportEntry](const ProtectedExport& candidate) {
                            return candidate.name == exportEntry.name;
                        }) != 1;
                })) {
            return false;
        }
        const auto duplicate = std::find_if(
            candidate->modules.begin(),
            candidate->modules.end(),
            [&record](const std::unique_ptr<ModuleRecord>& existing) {
                return existing->baseName == record->baseName
                    || EqualsPath(existing->canonicalPath, record->canonicalPath);
            });
        if (duplicate != candidate->modules.end()) {
            return false;
        }
        candidate->modules.push_back(std::move(record));
    }
    context = std::move(candidate);
    return true;
}

DeferredModuleGateCleanupStatus CleanupLocked() noexcept {
    gateInstalled.store(false, std::memory_order_release);
    if (lifecycle.context != nullptr) {
        lifecycle.context->denyOnly.store(true, std::memory_order_release);
    }
    auto disableKnownHooks = []() noexcept {
        bool disabled = true;
        std::scoped_lock hookLock(minHookMutex, hooksMutex);
        for (auto& hook : lifecycle.hooks) {
            if (!hook.created) {
                continue;
            }
            const auto status = MH_DisableHook(hook.target);
            disabled = (status == MH_OK || status == MH_ERROR_DISABLED
                    || status == MH_ERROR_NOT_CREATED)
                && disabled;
        }
        return disabled;
    };
    if (!disableKnownHooks()) {
        return DeferredModuleGateCleanupStatus::Incomplete;
    }

    std::unique_lock callbackLock(callbackGate);
    // A loader callback that began before the first disable may have admitted a
    // factory while cleanup waited. With loader/symbol callbacks now quiesced,
    // disable the complete final set before blocking factory callbacks.
    if (!disableKnownHooks()) {
        return DeferredModuleGateCleanupStatus::Incomplete;
    }
    lifecycle.mayBeEnabled = false;
    Steam::SteamFactoryCallbackBlock factoryCallbackBlock;
    bool removed = true;
    {
        std::scoped_lock hookLock(minHookMutex, hooksMutex);
        for (auto iterator = lifecycle.hooks.rbegin();
             iterator != lifecycle.hooks.rend();
             ++iterator) {
            if (!iterator->created) {
                continue;
            }
            const auto status = MH_RemoveHook(iterator->target);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
                iterator->created = false;
                if (iterator->factorySlot < Steam::kSteamFactorySlotCapacity) {
                    Steam::UnregisterSteamFactorySlot(iterator->factorySlot);
                    if (lifecycle.context != nullptr) {
                        ReleaseFactorySlot(
                            *lifecycle.context,
                            iterator->factorySlot);
                    }
                }
            }
            else {
                removed = false;
            }
        }
    }
    if (!removed) {
        return DeferredModuleGateCleanupStatus::Incomplete;
    }

    if (lifecycle.initialized && lifecycle.ownsInitialization) {
        std::scoped_lock hookLock(minHookMutex);
        const auto status = MH_Uninitialize();
        if (status != MH_OK && status != MH_ERROR_NOT_INITIALIZED) {
            return DeferredModuleGateCleanupStatus::Incomplete;
        }
    }
    activeContext.store({}, std::memory_order_release);
    lifecycle = {};
    return DeferredModuleGateCleanupStatus::Success;
}

}  // namespace

DeferredModuleGateInstallStatus InstallDeferredModuleGate(
    const DeferredModuleGateConfiguration& configuration) noexcept {
    std::scoped_lock installLock(installMutex);
    if (lifecycle.context != nullptr
        || activeContext.load(std::memory_order_acquire) != nullptr) {
        return DeferredModuleGateInstallStatus::HookInstallFailed;
    }
    try {
        std::shared_ptr<GateContext> context;
        if (!BuildContext(configuration, context)) {
            return DeferredModuleGateInstallStatus::InvalidConfiguration;
        }
        bool loaderHooksReady = true;
        {
            std::scoped_lock hookLock(minHookMutex);
            const auto initialize = MH_Initialize();
            lifecycle.ownsInitialization = initialize == MH_OK;
            if (!lifecycle.ownsInitialization
                && initialize != MH_ERROR_ALREADY_INITIALIZED) {
                return DeferredModuleGateInstallStatus::HookInstallFailed;
            }
            lifecycle.initialized = true;
            lifecycle.context = context;

            const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            if (kernel == nullptr) {
                loaderHooksReady = false;
            }
            if (loaderHooksReady) {
                const auto definitions = LoaderHooks(*context);
                lifecycle.hooks.reserve(
                    definitions.size() + Steam::kSteamFactorySlotCapacity);
                for (const auto& definition : definitions) {
                    void* const target = reinterpret_cast<void*>(
                        GetProcAddress(kernel, definition.procedure));
                    if (target == nullptr
                        || MH_CreateHook(
                            target,
                            definition.detour,
                            definition.original) != MH_OK) {
                        loaderHooksReady = false;
                        break;
                    }
                    lifecycle.hooks.push_back({target, true, false});
                }
            }
            if (loaderHooksReady) {
                for (const auto& hook : lifecycle.hooks) {
                    if (MH_QueueEnableHook(hook.target) != MH_OK) {
                        loaderHooksReady = false;
                        break;
                    }
                }
            }
            if (loaderHooksReady) {
                lifecycle.mayBeEnabled = true;
                activeContext.store(context, std::memory_order_release);
                loaderHooksReady = MH_ApplyQueued() == MH_OK;
            }
        }
        if (!loaderHooksReady) {
            static_cast<void>(CleanupLocked());
            return DeferredModuleGateInstallStatus::HookInstallFailed;
        }

        for (const auto& record : context->modules) {
            const HMODULE loaded = GetModuleHandleW(record->baseName.c_str());
            if (loaded != nullptr) {
                const auto admission = AdmitModule(context, loaded);
                if (!admission.target || !admission.admitted) {
                    static_cast<void>(CleanupLocked());
                    return DeferredModuleGateInstallStatus::AdmissionFailed;
                }
            }
            else if (!record->allowDeferred) {
                static_cast<void>(CleanupLocked());
                return DeferredModuleGateInstallStatus::AdmissionFailed;
            }
        }
        gateInstalled.store(true, std::memory_order_release);
        return DeferredModuleGateInstallStatus::Success;
    }
    catch (...) {
        static_cast<void>(CleanupLocked());
        return DeferredModuleGateInstallStatus::HookInstallFailed;
    }
}

DeferredModuleGateCleanupStatus UninstallDeferredModuleGate() noexcept {
    std::scoped_lock installLock(installMutex);
    return CleanupLocked();
}

bool DeferredModuleGateIsInstalled() noexcept {
    return gateInstalled.load(std::memory_order_acquire);
}

}  // namespace DSRRandomizer::Modules
