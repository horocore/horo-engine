#pragma once

/** @file EditorAudioPreviewOptions.h @brief Explicit unique preview-backend selection at the process boundary. */

#include "Horo/Audio/AudioDeviceDiscovery.h"

#include <optional>
#include <span>
#include <string_view>

namespace Horo::Editor::Detail {
    /** @brief Read one exact desktop peer name. @param value Process argument value. @return Typed selection or no known peer. */
    inline std::optional<Audio::AudioBackendKind> ParseAudioPreviewBackend(const std::string_view value) noexcept {
        using enum Audio::AudioBackendKind;
        if (value == "sdl3")
            return SDL3Audio;
        if (value == "wasapi")
            return WASAPI;
        if (value == "coreaudio")
            return CoreAudio;
        if (value == "pipewire")
            return PipeWire;
        if (value == "null")
            return NullAudio;
        return std::nullopt;
    }

    /** @brief Preserve a typed selection through renderer/project process restart.
     * @param backend Exact selected peer. @return Stable argument value, or empty for an unknown enum.
     */
    inline std::string_view AudioPreviewBackendName(const Audio::AudioBackendKind backend) noexcept {
        using enum Audio::AudioBackendKind;
        switch (backend) {
            case SDL3Audio:
                return "sdl3";
            case WASAPI:
                return "wasapi";
            case CoreAudio:
                return "coreaudio";
            case PipeWire:
                return "pipewire";
            case NullAudio:
                return "null";
        }
        return {};
    }

    /** @brief Read a unique preview-only selection without probing, native activation or fallback.
     * @param arguments Full process argv, including its executable entry when present.
     * @return Existing SDL3 default when absent, explicit peer when valid, or no selection for malformed/duplicate input.
     */
    inline std::optional<Audio::AudioBackendKind> ParseEditorAudioPreviewArguments(const std::span<char *const> arguments) noexcept {
        constexpr std::string_view name = "--audio-preview-backend";
        constexpr std::string_view joined = "--audio-preview-backend=";
        std::optional<Audio::AudioBackendKind> selected{Audio::AudioBackendKind::SDL3Audio};
        bool seen{};
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument{arguments[index]};
            std::string_view value;
            if (argument == name) {
                if (++index == arguments.size())
                    return std::nullopt;
                value = arguments[index];
            } else if (argument.starts_with(joined))
                value = argument.substr(joined.size());
            else
                continue;
            if (seen)
                return std::nullopt;
            selected = ParseAudioPreviewBackend(value);
            if (!selected)
                return std::nullopt;
            seen = true;
        }
        return selected;
    }
}  // namespace Horo::Editor::Detail
