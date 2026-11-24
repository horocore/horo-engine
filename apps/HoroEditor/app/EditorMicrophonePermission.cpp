#include "EditorMicrophonePermission.h"

#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

namespace Horo::Editor {
    namespace {
        /** @brief Projects exact OS facts without interpreting permission as a live microphone session. */
        EditorMicrophonePermissionStatus Project(const Audio::Backend::Sdl3MacCapturePermission native) noexcept {
            using enum EditorMicrophonePermissionStatus;
            if (native.pending)
                return Pending;
            switch (native.permission) {
                case Audio::AudioCapturePermission::Required:
                    return Required;
                case Audio::AudioCapturePermission::Granted:
                    return Granted;
                case Audio::AudioCapturePermission::Denied:
                    return Denied;
                default:
                    return Unavailable;
            }
        }
    }  // namespace

    /** @copydoc EditorMicrophonePermission::Status */
    EditorMicrophonePermissionStatus EditorMicrophonePermission::Status() {
        if (const auto now = std::chrono::steady_clock::now(); now >= nextObservation_) {
            status_ = Project(Audio::Backend::ObserveSdl3MacCapturePermission());
            nextObservation_ = now + std::chrono::milliseconds(250);
        }
        return status_;
    }

    /** @copydoc EditorMicrophonePermission::Request */
    void EditorMicrophonePermission::Request() {
        if (Status() != EditorMicrophonePermissionStatus::Required)
            return;
        const auto requested = Audio::Backend::RequestSdl3MacCapturePermission();
        nextObservation_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        status_ = requested.HasValue() ? Project(Audio::Backend::ObserveSdl3MacCapturePermission())
                                       : EditorMicrophonePermissionStatus::Unavailable;
    }
}  // namespace Horo::Editor
