#include "save/SavePathPolicy.h"

#include <utility>

namespace DSRRandomizer::Save {
namespace {

bool IsAsciiLetter(const wchar_t value) noexcept {
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

wchar_t ToLowerAscii(const wchar_t value) noexcept {
    if (value >= L'A' && value <= L'Z') {
        return static_cast<wchar_t>(value - L'A' + L'a');
    }
    return value;
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

}  // namespace

std::wstring_view PathDecision::EffectivePath() const noexcept {
    return effectivePath;
}

SavePathPolicy::SavePathPolicy(SavePathPolicyConfiguration configuration)
    : virtualLogicalSave_(Normalize(configuration.virtualLogicalSave)),
      realSaveRoot_(Normalize(configuration.realSaveRoot)),
      dedicatedRmm_(std::move(configuration.dedicatedRmm)) {
    configurationIsValid_ = !virtualLogicalSave_.empty()
        && !realSaveRoot_.empty()
        && !Normalize(dedicatedRmm_).empty();
}

PathDecision SavePathPolicy::Evaluate(
    const PathOperation operation,
    const std::wstring_view canonicalPath) const {
    static_cast<void>(operation);

    const auto normalizedPath = Normalize(canonicalPath);
    if (!configurationIsValid_ || normalizedPath.empty()) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    if (IsBelow(normalizedPath, realSaveRoot_)) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    if (IsOverhaulSave(normalizedPath)) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    if (normalizedPath == virtualLogicalSave_) {
        return {PathDecisionKind::Redirect, dedicatedRmm_, dedicatedRmm_};
    }

    if (IsNormalSaveTarget(normalizedPath)) {
        return {PathDecisionKind::Deny, L"", L""};
    }

    return {PathDecisionKind::Allow, L"", std::wstring(normalizedPath)};
}

std::wstring SavePathPolicy::Normalize(const std::wstring_view path) {
    if (path.size() < 3 || !IsAsciiLetter(path[0]) || path[1] != L':'
        || !IsPathSeparator(path[2])) {
        return {};
    }

    std::wstring normalized;
    normalized.reserve(path.size());
    normalized.push_back(ToLowerAscii(path[0]));
    normalized.append(L":\\");

    std::size_t segmentStart = 3;
    while (segmentStart < path.size()) {
        while (segmentStart < path.size() && IsPathSeparator(path[segmentStart])) {
            ++segmentStart;
        }
        if (segmentStart == path.size()) {
            break;
        }

        auto segmentEnd = segmentStart;
        while (segmentEnd < path.size() && !IsPathSeparator(path[segmentEnd])) {
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
            normalized.push_back(ToLowerAscii(character));
        }
        segmentStart = segmentEnd;
    }

    return normalized;
}

bool SavePathPolicy::IsBelow(
    const std::wstring_view path,
    const std::wstring_view root) noexcept {
    return path == root
        || (path.size() > root.size() && path.starts_with(root)
            && path[root.size()] == L'\\');
}

bool SavePathPolicy::IsOverhaulSave(const std::wstring_view path) noexcept {
    return path.find(L".overhaul.sl2") != std::wstring_view::npos;
}

bool SavePathPolicy::IsNormalSaveTarget(const std::wstring_view path) noexcept {
    constexpr std::wstring_view normalSaveName = L"draks0005.sl2";
    const auto position = path.rfind(normalSaveName);
    if (position == std::wstring_view::npos || position == 0
        || path[position - 1] != L'\\') {
        return false;
    }

    const auto afterName = position + normalSaveName.size();
    return afterName == path.size() || path[afterName] == L'.';
}

}  // namespace DSRRandomizer::Save
