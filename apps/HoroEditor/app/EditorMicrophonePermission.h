#pragma once

/** @file EditorMicrophonePermission.h @brief Main-thread macOS microphone permission composition for Editor Settings. */
#include "Horo/Audio/Internal/AudioCaptureBuffer.h"
#include "Horo/Editor/EditorGuiContext.h"

#include <chrono>
#include <memory>
#include <stop_token>

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::Assets {
    class AssetRegistry;
}

namespace Horo::Editor {
    class ProjectMutationCoordinator;
    class ImmersiveAuthoringContext;
    class ImmersivePlanningContext;

    /** @brief Identifies the editor's foreground speech capture deadline in a typed Audio result. */
    extern const ErrorCodeDescriptor EditorSpeechCaptureDurationLimit;

    /** @brief One foreground speech generation. Audio retains the native input and can revoke this token. */
    struct EditorSpeechCaptureLease final {
        Audio::Backend::AudioCaptureBufferDescriptor source;
        std::stop_token revoked;
    };

    /** @brief Projects native permission into settings while coalescing repeated draw-time observations. */
    class EditorMicrophonePermission final : public IEditorMicrophonePermission, public IEditorMicrophoneRecorder {
    public:
        EditorMicrophonePermission(ProjectMutationCoordinator &mutations, DurableFileSystem &files, Assets::AssetRegistry &registry);
        ~EditorMicrophonePermission() override;
        /** @copydoc IEditorMicrophonePermission::Status */
        [[nodiscard]] EditorMicrophonePermissionStatus Status() override;
        /** @copydoc IEditorMicrophonePermission::Request */
        void Request() override;
        /** @copydoc IEditorMicrophonePermission::ScanInputs */
        void ScanInputs() noexcept override;
        /** @copydoc IEditorMicrophonePermission::Inputs */
        [[nodiscard]] std::span<const std::string> Inputs() const noexcept override;
        /** @copydoc IEditorMicrophonePermission::StartTest */
        void StartTest(std::size_t input) noexcept override;
        /** @copydoc IEditorMicrophonePermission::StopTest */
        void StopTest() noexcept override;
        /** @copydoc IEditorMicrophonePermission::AdvanceTest */
        void AdvanceTest(bool foreground) noexcept override;
        /** @copydoc IEditorMicrophonePermission::TestStatus */
        [[nodiscard]] EditorMicrophoneTestStatus TestStatus() const noexcept override;
        /** @copydoc IEditorMicrophonePermission::TestPeakLevel */
        [[nodiscard]] float TestPeakLevel() const noexcept override;
        void SetProject(const std::filesystem::path &root) override;
        [[nodiscard]] bool CanRecord() const noexcept override;
        void StartRecording(std::size_t input, std::string_view wavePath) override;
        void SaveRecording() noexcept override;
        void CancelRecording() noexcept override;
        [[nodiscard]] EditorMicrophoneRecordingStatus RecordingStatus() const noexcept override;
        [[nodiscard]] float RecordingPeakLevel() const noexcept override;
        [[nodiscard]] std::string_view SavedRecordingPath() const noexcept override;
        [[nodiscard]] std::optional<Assets::AssetId> TakeSavedRecording() noexcept override;
        /** @brief Returns the local listening controls sharing this owner's input admission. */
        [[nodiscard]] IEditorMicrophoneMonitor &MonitorControls() noexcept;

        /** @brief Starts a selected input for speech after an explicit foreground action on Audio control.
         * @param input Stable ID selected from the input snapshot. @param foreground Current immersive focus.
         * @param owner Current editor context owner. @param context Admitted immersive planning snapshot.
         * @return Exact capture descriptor and token to pass to the transcription worker, or typed failure.
         */
        [[nodiscard]] Result<EditorSpeechCaptureLease> BeginSpeechCapture(Audio::AudioCaptureDeviceId input, bool foreground,
                                                                          const ImmersiveAuthoringContext &owner,
                                                                          const ImmersivePlanningContext &context);
        /** @brief Refreshes the selectable input list from an explicit foreground speech control. Does not open a device. */
        [[nodiscard]] bool RefreshSpeechInputs(bool foreground);
        /** @brief Resolves one displayed input to the exact stable endpoint in the current list. */
        [[nodiscard]] std::optional<Audio::AudioCaptureDeviceId> SpeechInputAt(std::size_t index) const noexcept;
        /** @brief Copies at most one admitted block. The caller clears scratch after consumption.
         * @param generation Exact lease generation. @param foreground Current immersive focus.
         * @param block Metadata written on success. @param pcm Bounded caller-owned interleaved scratch.
         * @return Copy outcome, or typed interruption that leaves no deliverable PCM.
         */
        [[nodiscard]] Result<Audio::Backend::AudioCaptureRead> ReadSpeechCapture(std::uint64_t generation, bool foreground,
                                                                                 Audio::Backend::AudioCapturedBlock &block,
                                                                                 std::span<Audio::AudioSample> pcm);
        /** @brief Detaches the microphone and hands off only the uninterrupted tail.
         * @param generation Exact lease generation. @param foreground Current immersive focus.
         * @return Detached tail to consume on the worker only while the lease token is current, or typed failure.
         */
        [[nodiscard]] Result<Audio::Backend::AudioCaptureBuffer> SealSpeechCapture(std::uint64_t generation, bool foreground);
        /** @brief Rechecks foreground focus, native consent and context for a live or detached speech lease.
         * @param generation Exact lease generation. @param foreground Current immersive focus.
         * @return Success while the lease is current, or an error that withdraws its evidence.
         */
        [[nodiscard]] Result<void> ReconcileSpeechLease(std::uint64_t generation, bool foreground);
        /** @brief Checks whether the exact live or detached lease still permits speech presentation on the editor lane. */
        [[nodiscard]] bool CanPresentSpeech(std::uint64_t generation) const noexcept;
        /** @brief Reads the live lease's capture clock without querying native permissions or touching PCM.
         * @param generation Exact active lease generation, on the editor owner lane.
         * @return Current timestamp in the same SDL clock as microphone PCM, or empty after detachment or expiry.
         */
        [[nodiscard]] std::optional<Audio::AudioMonotonicTimestamp> ReadSpeechClock(std::uint64_t generation) const noexcept;
        /** @brief Releases the exact lease and invalidates any pending transcription evidence. */
        void CancelSpeechCapture(std::uint64_t generation) noexcept;

    private:
        [[nodiscard]] Result<void> CheckSpeechCapture(std::uint64_t generation, bool foreground);
        struct CaptureCheck;
        std::unique_ptr<CaptureCheck> check_;
        std::chrono::steady_clock::time_point nextObservation_{};
        EditorMicrophonePermissionStatus status_{EditorMicrophonePermissionStatus::Unavailable};
    };
}  // namespace Horo::Editor
