#pragma once

/** @file EditorMicrophoneMonitorSettings.h
 * @brief Backend-neutral Settings commands and state for local microphone listening.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace Horo::Editor {
    /** @brief Owner-lane state displayed by the optional local listening controls. */
    enum class EditorMicrophoneMonitorStatus : std::uint8_t {
        Idle,
        Scanning,
        Ready,
        Starting,
        Listening,
        Stopping,
        PermissionDenied,
        Failed
    };

    /** @brief Selectable device label and its position in the host's current discovery. */
    struct EditorMicrophoneMonitorOption final {
        std::size_t discoveryIndex{};
        std::string label;
    };

    /** @brief Queues local listening commands. Device and capture operations run outside drawing. */
    class IEditorMicrophoneMonitor {
    public:
        virtual ~IEditorMicrophoneMonitor() = default;
        /** @brief Returns whether the shared microphone is available for device selection. */
        [[nodiscard]] virtual bool CanConfigure() const noexcept = 0;
        /** @brief Requests a fresh inventory without opening any device. */
        virtual void Scan() noexcept = 0;
        /** @brief Borrows the current input options until the next control update. */
        [[nodiscard]] virtual std::span<const EditorMicrophoneMonitorOption> InputOptions() const noexcept = 0;
        /** @brief Borrows the current non-migrating output options until the next control update. */
        [[nodiscard]] virtual std::span<const EditorMicrophoneMonitorOption> OutputOptions() const noexcept = 0;
        /** @brief Queues an explicitly authorized input and output from the current option lists.
         * @param input Index in the current input choices. @param output Index in the current output choices.
         * @param privateListening Explicit user confirmation for the selected output.
         */
        virtual void StartListening(std::size_t input, std::size_t output, bool privateListening) noexcept = 0;
        /** @brief Withdraws the active or queued listening intent. */
        virtual void StopListening() noexcept = 0;
        /** @brief Returns the last control update without polling any device. */
        [[nodiscard]] virtual EditorMicrophoneMonitorStatus MonitorStatus() const noexcept = 0;
    };
}  // namespace Horo::Editor
