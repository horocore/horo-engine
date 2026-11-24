#pragma once

/** @file EditorMicrophonePermission.h @brief Main-thread macOS microphone permission composition for Editor Settings. */
#include "Horo/Editor/EditorGuiContext.h"

#include <chrono>
#include <memory>

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::Assets {
    class AssetRegistry;
}

namespace Horo::Editor {
    class ProjectMutationCoordinator;

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

    private:
        struct CaptureCheck;
        std::unique_ptr<CaptureCheck> check_;
        std::chrono::steady_clock::time_point nextObservation_{};
        EditorMicrophonePermissionStatus status_{EditorMicrophonePermissionStatus::Unavailable};
    };
}  // namespace Horo::Editor
