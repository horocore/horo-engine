#pragma once
/** @file EditorLightingOptions.h @brief Explicit startup lighting recipe, fallback and coverage controls. */
#include "EditorViewportLighting.h"

#include <optional>
#include <span>
#include <string_view>

namespace Horo::Editor::Detail {
    enum class LightingOptionState {
        Absent,
        Value,
        Invalid
    };

    struct LightingOptionValue final {
        LightingOptionState state{LightingOptionState::Absent};
        std::string_view value;
    };

    /** @brief Reads one unique named startup option, including the separate and equals syntaxes. */
    inline LightingOptionValue ReadLightingOption(const std::span<char *const> arguments, const std::string_view name) noexcept {
        LightingOptionValue result;
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument{arguments[index]};
            std::string_view value;
            if (argument == name) {
                if (++index == arguments.size())
                    return {LightingOptionState::Invalid, {}};
                value = arguments[index];
            } else if (argument.starts_with(name) && argument.size() > name.size() && argument[name.size()] == '=')
                value = argument.substr(name.size() + 1);
            else
                continue;
            if (result.state != LightingOptionState::Absent || value.empty())
                return {LightingOptionState::Invalid, {}};
            result = {LightingOptionState::Value, value};
        }
        return result;
    }

    inline bool ApplyLightingMode(EditorViewportLightingProfile &profile, const LightingOptionValue &option) noexcept {
        if (option.state == LightingOptionState::Absent)
            return true;
        if (option.state == LightingOptionState::Invalid)
            return false;
        if (option.value == "forward")
            profile.requested = Render::LightPreparationMode::BaselineForwardCpu;
        else if (option.value == "clustered")
            profile.requested = Render::LightPreparationMode::ClusteredForwardNative;
        else
            return false;
        return true;
    }

    inline bool ApplyLightingFallback(EditorViewportLightingProfile &profile, const LightingOptionValue &option) noexcept {
        if (option.state == LightingOptionState::Absent)
            return true;
        if (option.state == LightingOptionState::Invalid)
            return false;
        if (option.value == "forward")
            profile.fallback = Render::LightPreparationFallback::AllowBaselineForwardCpu;
        else if (option.value == "forbid")
            profile.fallback = Render::LightPreparationFallback::Forbidden;
        else
            return false;
        return true;
    }

    inline bool ApplyLightingCoverage(EditorViewportLightingProfile &profile, const LightingOptionValue &option) noexcept {
        if (option.state == LightingOptionState::Absent)
            return true;
        if (option.state == LightingOptionState::Invalid)
            return false;
        if (option.value == "complete")
            profile.budget.requireCompleteCoverage = true;
        else if (option.value == "allow-omissions")
            profile.budget.requireCompleteCoverage = false;
        else
            return false;
        return true;
    }

    /** @brief Resolves only explicit authored choices; malformed or duplicate inputs cannot silently choose another recipe. */
    inline std::optional<EditorViewportLightingProfile> ParseEditorLightingArguments(const std::span<char *const> arguments) noexcept {
        EditorViewportLightingProfile profile;
        if (!ApplyLightingMode(profile, ReadLightingOption(arguments, "--lighting")) ||
            !ApplyLightingFallback(profile, ReadLightingOption(arguments, "--lighting-fallback")) ||
            !ApplyLightingCoverage(profile, ReadLightingOption(arguments, "--lighting-coverage")))
            return std::nullopt;
        return profile;
    }
}  // namespace Horo::Editor::Detail
