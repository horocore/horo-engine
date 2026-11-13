#pragma once
/** @file EditorColorOptions.h @brief Explicit bounded command-line color settings; no path discovery or renderer fallback. */
#include "EditorLightingOptions.h"
#include "EditorViewportColorTypes.h"

#include <charconv>
#include <cmath>
#include <filesystem>

namespace Horo::Editor::Detail {
    /** @brief Inert unique startup color selection with validation preserved for the host. */
    struct EditorColorOptions final {
        std::optional<std::filesystem::path> root;
        std::optional<std::filesystem::path> lookRoot;
        Render::ExposureSettings exposure;
        bool valid{true};
        bool settingsRequested{};
    };

    /** @brief Parses one complete finite float setting. @param value Exact unique option. @param result Selected value.
     * @return False for malformed/foreign syntax without changing renderer selection. */
    inline bool ReadColorEv(const LightingOptionValue &value, float &result) noexcept {
        if (value.state == LightingOptionState::Absent)
            return true;
        if (value.state != LightingOptionState::Value)
            return false;
        const auto parsed = std::from_chars(value.value.data(), value.value.data() + value.value.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == value.value.data() + value.value.size() && std::isfinite(result);
    }

    /** @brief Admits only the explicit manual/automatic mode names. @param mode Unique option. @param options Selected settings. */
    inline void ApplyColorExposureMode(const LightingOptionValue &mode, EditorColorOptions &options) {
        if (mode.state == LightingOptionState::Value) {
            if (mode.value == "automatic")
                options.exposure.mode = Render::ExposureMode::AutomaticHistogram;
            else if (mode.value != "manual")
                options.valid = false;
        } else if (mode.state == LightingOptionState::Invalid)
            options.valid = false;
    }

    /** @brief Reads only declared color options. @param arguments Actual process arguments. @return Explicit validated selection. */
    inline EditorColorOptions ParseEditorColorArguments(std::span<char *const> arguments) {
        EditorColorOptions options;
        const auto root = ReadLightingOption(arguments, "--color-pipeline-root");
        if (root.state == LightingOptionState::Value)
            options.root = std::filesystem::path{root.value};
        options.valid = root.state != LightingOptionState::Invalid;
        const auto look = ReadLightingOption(arguments, "--color-look-root");
        if (look.state == LightingOptionState::Value)
            options.lookRoot = std::filesystem::path{look.value};
        options.valid = look.state != LightingOptionState::Invalid && options.valid;
        const auto mode = ReadLightingOption(arguments, "--exposure-mode");
        options.settingsRequested = mode.state != LightingOptionState::Absent ||
                                    ReadLightingOption(arguments, "--exposure-ev").state != LightingOptionState::Absent ||
                                    ReadLightingOption(arguments, "--exposure-compensation").state != LightingOptionState::Absent;
        ApplyColorExposureMode(mode, options);
        options.valid = ReadColorEv(ReadLightingOption(arguments, "--exposure-ev"), options.exposure.manualEv) && options.valid;
        options.valid =
            ReadColorEv(ReadLightingOption(arguments, "--exposure-compensation"), options.exposure.compensationEv) && options.valid;
        options.valid = Render::ValidateExposureSettings(options.exposure).HasValue() && options.valid;
        return options;
    }
}  // namespace Horo::Editor::Detail
