#pragma once

/** @file EditorMicrophoneMonitor.h @brief Editor host ownership of one explicitly authorized native listening session. */
#include "Horo/Audio/AudioDeviceDiscovery.h"
#include "Horo/Foundation/Result.h"

#include <memory>
#include <span>

namespace Horo {
    class JobSystem;
}

namespace Horo::Editor {
    enum class EditorMicrophoneMonitorPhase : std::uint8_t { Ready, Opening, Listening, Closing, Closed };

    /** @brief Owns a single use SDL capture and output route on the editor control lane. */
    class EditorMicrophoneMonitor final {
    public:
        /** @brief Enumerates both endpoints on one fresh runtime without opening either device. */
        [[nodiscard]] static Result<std::unique_ptr<EditorMicrophoneMonitor>> Discover(Audio::AudioRuntimeId owner);
        ~EditorMicrophoneMonitor();

        /** @brief Returns the exact input inventory from this session's backend. */
        [[nodiscard]] std::span<const Audio::AudioDiscoveredCaptureDevice> Inputs() const noexcept;
        /** @brief Returns the output inventory, including any system default entry. */
        [[nodiscard]] std::span<const Audio::AudioDiscoveredDevice> Outputs() const noexcept;
        /** @brief Prepares playback for an explicit input and independently confirmed private output.
         * @param input Input selected from this discovery. @param output Explicit output from this discovery.
         * @param privateListening Current user's confirmation for the selected output. @param jobs Host lifetime for the live PCM service.
         * @return An admitted output startup or a typed error. No recording is opened until Pump sees an active output.
         */
        [[nodiscard]] Result<void> Start(Audio::AudioCaptureDeviceId input, Audio::AudioDeviceId output, bool privateListening,
                                         std::shared_ptr<JobSystem> jobs);
        /** @brief Advances startup, closure or a single PCM handoff.
         * @param authorized Current foreground and native permission evidence for this exact session.
         * @return A successful control step or the reason capture was rejected or interrupted.
         */
        [[nodiscard]] Result<void> Pump(bool authorized);
        /** @brief Immediately withdraws capture and schedules output detachment. */
        void Stop() noexcept;
        /** @brief Returns the current host lifecycle. */
        [[nodiscard]] EditorMicrophoneMonitorPhase Phase() const noexcept;

    private:
        struct State;
        explicit EditorMicrophoneMonitor(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
