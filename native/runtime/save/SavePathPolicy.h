#pragma once

#include <string>
#include <string_view>

namespace DSRRandomizer::Save {

enum class PathOperation {
    Open,
    Read,
    Write,
    RenameSource,
    RenameDestination,
    Delete,
    Attributes,
    Enumeration,
};

enum class PathDecisionKind {
    Allow,
    Redirect,
    Deny,
};

struct PathDecision {
    PathDecisionKind kind;
    std::wstring redirectTarget;
    std::wstring effectivePath;

    [[nodiscard]] std::wstring_view EffectivePath() const noexcept;
};

struct SavePathPolicyConfiguration {
    std::wstring virtualLogicalSave;
    std::wstring realSaveRoot;
    std::wstring dedicatedRmm;
};

class SavePathPolicy {
public:
    explicit SavePathPolicy(SavePathPolicyConfiguration configuration);

    [[nodiscard]] PathDecision Evaluate(
        PathOperation operation,
        std::wstring_view canonicalPath) const;

private:
    [[nodiscard]] static std::wstring Normalize(
        std::wstring_view path,
        bool allowForwardSlashes);
    [[nodiscard]] static bool IsBelow(
        std::wstring_view path,
        std::wstring_view root) noexcept;
    [[nodiscard]] static bool IsOverhaulSave(std::wstring_view path) noexcept;
    [[nodiscard]] static bool IsNormalSaveTarget(std::wstring_view path) noexcept;

    std::wstring virtualLogicalSave_;
    std::wstring realSaveRoot_;
    std::wstring dedicatedRmm_;
    bool configurationIsValid_ = false;
};

}  // namespace DSRRandomizer::Save
