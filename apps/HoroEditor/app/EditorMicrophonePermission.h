#pragma once

/** @file EditorMicrophonePermission.h @brief Main-thread macOS microphone permission composition for Editor Settings. */
#include "Horo/Editor/EditorGuiContext.h"

#include <chrono>

namespace Horo::Editor {
    /** @brief Projects native permission into settings while coalescing repeated draw-time observations. */
    class EditorMicrophonePermission final : public IEditorMicrophonePermission {
    public:
        /** @copydoc IEditorMicrophonePermission::Status */
        [[nodiscard]] EditorMicrophonePermissionStatus Status() override;
        /** @copydoc IEditorMicrophonePermission::Request */
        void Request() override;

    private:
        std::chrono::steady_clock::time_point nextObservation_{};
        EditorMicrophonePermissionStatus status_{EditorMicrophonePermissionStatus::Unavailable};
    };
}  // namespace Horo::Editor
