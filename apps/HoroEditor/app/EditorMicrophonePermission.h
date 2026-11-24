#pragma once

/** @file EditorMicrophonePermission.h @brief Main-thread macOS microphone permission composition for Editor Settings. */
#include "Horo/Editor/EditorGuiContext.h"

#include <chrono>
#include <memory>

namespace Horo::Editor {
    /** @brief Projects native permission into settings while coalescing repeated draw-time observations. */
    class EditorMicrophonePermission final : public IEditorMicrophonePermission {
    public:
        EditorMicrophonePermission();
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

    private:
        struct CaptureCheck;
        std::unique_ptr<CaptureCheck> check_;
        std::chrono::steady_clock::time_point nextObservation_{};
        EditorMicrophonePermissionStatus status_{EditorMicrophonePermissionStatus::Unavailable};
    };
}  // namespace Horo::Editor
