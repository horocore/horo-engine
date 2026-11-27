#pragma once

/** @file EditorMicrophoneMonitor.h @brief Editor host ownership of one explicitly authorized native listening session. */
#include "Horo/Audio/AudioDeviceDiscovery.h"
#include "Horo/Editor/EditorMicrophoneMonitorSettings.h"
#include "Horo/Foundation/Result.h"

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

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
        /** @brief Checks whether the discovered output can be pinned for private listening. */
        [[nodiscard]] bool CanSelectOutput(Audio::AudioDeviceId output) const noexcept;
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

    /** @brief Queues Settings actions and owns only one native listening session on the editor control lane. */
    class EditorMicrophoneMonitorControls final : public IEditorMicrophoneMonitor {
    public:
        /** @brief Borrows the shared microphone availability check for the lifetime of these controls. */
        explicit EditorMicrophoneMonitorControls(std::function<bool()> inputAvailable);
        /** @copydoc IEditorMicrophoneMonitor::CanConfigure */
        [[nodiscard]] bool CanConfigure() const noexcept override;
        /** @copydoc IEditorMicrophoneMonitor::Scan */
        void Scan() noexcept override;
        /** @copydoc IEditorMicrophoneMonitor::InputOptions */
        [[nodiscard]] std::span<const EditorMicrophoneMonitorOption> InputOptions() const noexcept override;
        /** @copydoc IEditorMicrophoneMonitor::OutputOptions */
        [[nodiscard]] std::span<const EditorMicrophoneMonitorOption> OutputOptions() const noexcept override;
        /** @copydoc IEditorMicrophoneMonitor::StartListening */
        void StartListening(std::size_t input, std::size_t output, bool privateListening) noexcept override;
        /** @copydoc IEditorMicrophoneMonitor::StopListening */
        void StopListening() noexcept override;
        /** @copydoc IEditorMicrophoneMonitor::MonitorStatus */
        [[nodiscard]] EditorMicrophoneMonitorStatus MonitorStatus() const noexcept override;
        /** @brief Holds the shared input through command admission and asynchronous detachment. */
        [[nodiscard]] bool OccupiesInput() const noexcept;
        /** @brief Consumes queued commands with current foreground and native authorization evidence. */
        void Advance(bool foreground, bool authorized) noexcept;

    private:
        struct Selection final {
            Audio::AudioCaptureDeviceId input;
            Audio::AudioDeviceId output;
        };
        void Discover();
        void StartSelected();
        void Pump();
        void AdvanceForeground();
        void Withdraw(EditorMicrophoneMonitorStatus result) noexcept;
        void DriveRetirement();

        std::function<bool()> inputAvailable_;
        std::unique_ptr<EditorMicrophoneMonitor> session_;
        std::vector<EditorMicrophoneMonitorOption> inputs_;
        std::vector<EditorMicrophoneMonitorOption> outputs_;
        std::optional<Selection> selection_;
        EditorMicrophoneMonitorStatus status_{EditorMicrophoneMonitorStatus::Idle};
        EditorMicrophoneMonitorStatus retirementResult_{EditorMicrophoneMonitorStatus::Idle};
        std::uint64_t nextRuntime_{std::numeric_limits<std::uint64_t>::max() - 1};
    };
}  // namespace Horo::Editor
