#include <iostream>
#include <string>
#include <string_view>

#include "save/SavePathPolicy.h"

namespace {

using DSRRandomizer::Save::PathDecisionKind;
using DSRRandomizer::Save::PathOperation;
using DSRRandomizer::Save::SavePathPolicy;
using DSRRandomizer::Save::SavePathPolicyConfiguration;

struct Case {
    const char* name;
    PathOperation operation;
    std::wstring_view path;
    PathDecisionKind expectedKind;
    std::wstring_view expectedRedirect;
};

int Fail(const Case& test, std::wstring_view actual) {
    std::wcerr << L"case failed: " << test.name << L"\npath: " << test.path
               << L"\nactual redirect: " << actual << L'\n';
    return 1;
}

int FailConfiguration(const char* name) {
    std::cerr << "configuration case failed: " << name << '\n';
    return 1;
}

}  // namespace

int main() {
    const std::wstring_view virtualSave =
        L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2";
    const std::wstring_view realSaveRoot =
        L"C:\\Users\\U\\Documents\\NBGI\\D\u00c4RK SOULS REMASTERED";
    const std::wstring_view dedicatedRmm =
        L"C:\\External\\DSR-Randomizer\\saves\\12345678901234567\\DRAKS0005.rmm";

    const SavePathPolicy policy(SavePathPolicyConfiguration{
        std::wstring(virtualSave),
        std::wstring(realSaveRoot),
        std::wstring(dedicatedRmm),
    });

    const Case cases[] = {
        {"open redirects exact logical save", PathOperation::Open, virtualSave,
         PathDecisionKind::Redirect, dedicatedRmm},
        {"read redirects mixed-case logical save", PathOperation::Read,
         L"c:\\r\u00e9solvedvirtual\\nbgi\\dark souls remastered\\12345678901234567\\draks0005.sl2",
         PathDecisionKind::Redirect, dedicatedRmm},
        {"write redirects slash-normalized logical save", PathOperation::Write,
         L"C:/R\u00c9solvedVirtual/NBGI/DARK SOULS REMASTERED/12345678901234567/DRAKS0005.sl2",
         PathDecisionKind::Redirect, dedicatedRmm},
        {"rename source redirects logical save", PathOperation::RenameSource, virtualSave,
         PathDecisionKind::Redirect, dedicatedRmm},
        {"rename destination redirects logical save", PathOperation::RenameDestination, virtualSave,
         PathDecisionKind::Redirect, dedicatedRmm},
        {"delete redirects logical save", PathOperation::Delete, virtualSave,
         PathDecisionKind::Redirect, dedicatedRmm},
        {"attributes redirect logical save", PathOperation::Attributes, virtualSave,
         PathDecisionKind::Redirect, dedicatedRmm},
        {"enumeration redirects logical save", PathOperation::Enumeration, virtualSave,
         PathDecisionKind::Redirect, dedicatedRmm},
        {"enumeration directory remains unrelated", PathOperation::Enumeration,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567",
         PathDecisionKind::Allow, L""},
        {"adapter-resolved reparse target redirects", PathOperation::Open,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2",
         PathDecisionKind::Redirect, dedicatedRmm},
        {"real save root itself is denied", PathOperation::Attributes, realSaveRoot,
         PathDecisionKind::Deny, L""},
        {"real save root child is denied", PathOperation::Read,
         L"C:\\Users\\U\\Documents\\NBGI\\D\u00c4RK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"real root check is Windows ordinal case and separator insensitive", PathOperation::Write,
         L"c:/users/u/documents/nbgi/d\u00e4rk souls remastered/12345678901234567/DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"real root prefix is not a matching segment", PathOperation::Open,
         L"C:\\Users\\U\\Documents\\NBGI\\DARK SOULS REMASTERED-archive\\unrelated.txt",
         PathDecisionKind::Allow, L""},
        {"overhaul suffix outside real root is denied", PathOperation::Open,
         L"C:\\External\\DRAKS0005.sl2.overhaul.sl2",
         PathDecisionKind::Deny, L""},
        {"overhaul suffix is denied before generic allow", PathOperation::Delete,
         L"C:\\External\\archive\\DRAKS0005.sl2.OVERHAUL.sl2.bak",
         PathDecisionKind::Deny, L""},
        {"wrong steam id normal save is denied", PathOperation::Open,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\99999999999999999\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"normal save backup suffix is denied", PathOperation::Open,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2.bak",
         PathDecisionKind::Deny, L""},
        {"normal save outside virtual profile is denied", PathOperation::Open,
         L"C:\\External\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"unrelated external path is allowed", PathOperation::Open,
         L"C:\\External\\unrelated.txt", PathDecisionKind::Allow, L""},
        {"traversal-bearing path is denied", PathOperation::Open,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\..\\12345678901234567\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"repeated separator path is denied", PathOperation::Open,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"terminal separator path is denied", PathOperation::Open,
         L"C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2\\",
         PathDecisionKind::Deny, L""},
        {"short-name path is denied until adapter resolves it", PathOperation::Open,
         L"C:\\RESOLV~1\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"device namespace path is denied", PathOperation::Open,
         L"\\\\?\\C:\\R\u00c9solvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
        {"unc path is denied", PathOperation::Open,
         L"\\\\server\\share\\DRAKS0005.sl2", PathDecisionKind::Deny, L""},
        {"trailing-dot path is denied", PathOperation::Open,
         L"C:\\ResolvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2.",
         PathDecisionKind::Deny, L""},
        {"trailing-space path is denied", PathOperation::Open,
         L"C:\\ResolvedVirtual\\NBGI\\DARK SOULS REMASTERED\\12345678901234567\\DRAKS0005.sl2 ",
         PathDecisionKind::Deny, L""},
        {"relative path is denied", PathOperation::Open, L"DRAKS0005.sl2",
         PathDecisionKind::Deny, L""},
    };

    for (const auto& test : cases) {
        const auto decision = policy.Evaluate(test.operation, test.path);
        if (decision.kind != test.expectedKind) {
            return Fail(test, decision.redirectTarget);
        }
        if (decision.redirectTarget != test.expectedRedirect) {
            return Fail(test, decision.redirectTarget);
        }
        if (test.expectedKind == PathDecisionKind::Redirect
            && decision.EffectivePath() != test.expectedRedirect) {
            return Fail(test, decision.EffectivePath());
        }
    }

    struct ConfigurationCase {
        const char* name;
        std::wstring_view dedicatedPath;
    };

    const ConfigurationCase invalidConfigurations[] = {
        {"wrong dedicated leaf", L"C:\\External\\DSR-Randomizer\\saves\\12345678901234567\\wrong.rmm"},
        {"dedicated path below real root", L"C:\\Users\\U\\Documents\\NBGI\\D\u00c4RK SOULS REMASTERED\\DRAKS0005.rmm"},
        {"forward slash dedicated path", L"C:/External/DSR-Randomizer/saves/12345678901234567/DRAKS0005.rmm"},
        {"repeated separator dedicated path", L"C:\\External\\DSR-Randomizer\\saves\\12345678901234567\\\\DRAKS0005.rmm"},
        {"terminal separator dedicated path", L"C:\\External\\DSR-Randomizer\\saves\\12345678901234567\\DRAKS0005.rmm\\"},
        {"relative dedicated path", L"DRAKS0005.rmm"},
    };

    for (const auto& test : invalidConfigurations) {
        const SavePathPolicy invalidPolicy(SavePathPolicyConfiguration{
            std::wstring(virtualSave),
            std::wstring(realSaveRoot),
            std::wstring(test.dedicatedPath),
        });
        if (invalidPolicy.Evaluate(PathOperation::Open, virtualSave).kind
            != PathDecisionKind::Deny) {
            return FailConfiguration(test.name);
        }
    }

    return 0;
}
