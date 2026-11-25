#pragma once

/** @file Sdl3CaptureDefaults.h @brief Control-lane evidence for SDL recording default roles. */

#include "Horo/Audio/AudioFormat.h"

#include <array>
#include <optional>
#include <string>

namespace Horo::Audio::Backend::Sdl3Detail {
    /** @brief Complete, untruncated name and format used to correlate a recording route with SDL. */
    struct CaptureDefaultRoute final {
        std::string name;
        AudioProcessingFormat format;
        friend bool operator==(const CaptureDefaultRoute &, const CaptureDefaultRoute &) = default;
    };

#if defined(_WIN32)
    /** @brief Native role evidence. The endpoint ID is private and compares observations during discovery. */
    struct WindowsCaptureDefault final {
        std::wstring endpoint;
        CaptureDefaultRoute route;
        friend bool operator==(const WindowsCaptureDefault &, const WindowsCaptureDefault &) = default;
    };

    using WindowsCaptureDefaults = std::array<std::optional<WindowsCaptureDefault>, 3>;

    /** @brief Observes Console, Multimedia and Communications in order from the native recording inventory.
     * @return Role evidence only for unambiguous active endpoints with supported format and name evidence.
     * @details Audio control lane. Initializes no SDL devices and requests no capture permission.
     */
    [[nodiscard]] WindowsCaptureDefaults ObserveWindowsCaptureDefaults();
#endif
}  // namespace Horo::Audio::Backend::Sdl3Detail
