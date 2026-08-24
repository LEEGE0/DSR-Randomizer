#include "save/SavePathPolicy.h"

#include <Windows.h>

#include <limits>

namespace DSRRandomizer::Save {
namespace {

bool IsAsciiLetter(const wchar_t value) noexcept {
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

bool EqualsOrdinalIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

bool StartsWithOrdinalIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size()
        && EqualsOrdinalIgnoreCase(value.substr(0, prefix.size()), prefix);
}

bool SegmentIsAmbiguous(const std::wstring_view segment) noexcept {
    if (segment == L"." || segment == L".." || segment.empty()
        || segment.back() == L'.' || segment.back() == L' ') {
        return true;
    }

    for (const auto character : segment) {
        if (character < L' ' || character == L':' || character == L'~'
            || character == L'?' || character == L'*' || character == L'"'
            || character == L'<' || character == L'>' || character == L'|') {
            return true;
        }
    }
    return false;
}

bool IsPathSeparator(const wchar_t value) noexcept {
    return value == L'\\' || value == L'/';
}

bool IsExactFileName(
    const std::wstring_view path,
    const std::wstring_view expectedName) noexcept {
    const auto separator = path.find_last_of(L'\\');
    return separator != std::wstring_view::npos
        && EqualsOrdinalIgnoreCase(path.substr(separator + 1), expectedName);
}

}  // namespace

std::wstring_view PathDecision::EffectivePath() const noexcept {
    return effectivePath;
}

SavePathPolicy::SavePathPolicy(SavePathPolicyConfiguration configuration)
    : virtualLogicalSave_(Normalize(configuration.virtualLogicalSave, false)),
      realSaveRoot_(Normalize(configuration.realSaveRoot, false)),
      dedicatedRmm_(Normalize(configuration.dedicatedRmm, false)) {
    configurationIsValid_ = !virtualLogicalSave_.empty()
        && !realSaveRoot_.empty()
        && !dedicatedRmm_.empty()
        && IsExactFileName(virtualLogicalSave_, L"DRAKS0005.sl2")
        && IsExactFileName(dedicatedRmm_, L"DRAKS0005.rmm")
        && !IsBelow(dedicatedRmm_, realSaveRoot_);
}

PathDecision SavePathPolicy::Evaluate(
    const PathOperation operation,
    const std::wstring_view canonicalPath) const {
    static_cast<void>(operation);

    const auto normalizedPath = Normalize(canonicalPath, true);
    if (!configurationIsValid_ || normalizedPath.empty()) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    if (IsBelow(normalizedPath, realSaveRoot_)) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    if (IsOverhaulSave(normalizedPath)) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    if (EqualsOrdinalIgnoreCase(normalizedPath, virtualLogicalSave_)) {
        return {PathDecisionKind::Redirect, dedicatedRmm_, dedicatedRmm_};
    }

    if (IsNormalSaveTarget(normalizedPath)) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    return {PathDecisionKind::Allow, L"", std::wstring(normalizedPath)};
}

std::wstring SavePathPolicy::Normalize(
    const std::wstring_view path,
    const bool allowForwardSlashes) {
    if (path.size() < 3 || !IsAsciiLetter(path[0]) || path[1] != L':'
        || !IsPathSeparator(path[2])
        || (!allowForwardSlashes && path[2] != L'\\')) {
        return {};
    }

    std::wstring normalized;
    normalized.reserve(path.size());
    normalized.push_back(path[0]);
    normalized.append(L":\\");

    if (path.size() == 3) {
        return normalized;
    }
    if (IsPathSeparator(path.back())) {
        return {};
    }

    std::size_t segmentStart = 3;
    while (segmentStart < path.size()) {
        if (!allowForwardSlashes && path[segmentStart] == L'/') {
            return {};
        }
        auto segmentEnd = segmentStart;
        while (segmentEnd < path.size() && !IsPathSeparator(path[segmentEnd])) {
            if (!allowForwardSlashes && path[segmentEnd] == L'/') {
                return {};
            }
            ++segmentEnd;
        }
        const auto segment = path.substr(segmentStart, segmentEnd - segmentStart);
        if (SegmentIsAmbiguous(segment)) {
            return {};
        }

        if (normalized.size() > 3) {
            normalized.push_back(L'\\');
        }
        for (const auto character : segment) {
            normalized.push_back(character);
        }
        if (segmentEnd == path.size()) {
            break;
        }
        if (!allowForwardSlashes && path[segmentEnd] == L'/') {
            return {};
        }
        if (segmentEnd + 1 == path.size() || IsPathSeparator(path[segmentEnd + 1])) {
            return {};
        }
        segmentStart = segmentEnd + 1;
    }

    return normalized;
}

bool SavePathPolicy::IsBelow(
    const std::wstring_view path,
    const std::wstring_view root) noexcept {
    return EqualsOrdinalIgnoreCase(path, root)
        || (path.size() > root.size()
            && StartsWithOrdinalIgnoreCase(path, root)
            && (root.back() == L'\\' || path[root.size()] == L'\\'));
}

bool SavePathPolicy::IsOverhaulSave(const std::wstring_view path) noexcept {
    constexpr std::wstring_view overhaulSuffix = L".overhaul.sl2";
    if (path.size() < overhaulSuffix.size()) {
        return false;
    }

    for (std::size_t position = 0;
         position + overhaulSuffix.size() <= path.size();
         ++position) {
        if (EqualsOrdinalIgnoreCase(
                path.substr(position, overhaulSuffix.size()), overhaulSuffix)) {
            return true;
        }
    }
    return false;
}

bool SavePathPolicy::IsNormalSaveTarget(const std::wstring_view path) noexcept {
    constexpr std::wstring_view normalSaveName = L"draks0005.sl2";
    const auto separator = path.find_last_of(L'\\');
    if (separator == std::wstring_view::npos) {
        return false;
    }

    const auto leaf = path.substr(separator + 1);
    return EqualsOrdinalIgnoreCase(leaf, normalSaveName)
        || (leaf.size() > normalSaveName.size()
            && leaf[normalSaveName.size()] == L'.'
            && StartsWithOrdinalIgnoreCase(leaf, normalSaveName));
}

}  // namespace DSRRandomizer::Save
