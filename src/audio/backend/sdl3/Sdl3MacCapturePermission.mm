/**
 * @file Sdl3MacCapturePermission.mm
 * @brief Read-only observation of macOS microphone authorization.
 */

#include "Sdl3AudioBackendImpl.h"

#import <AVFoundation/AVFoundation.h>

namespace Horo::Audio::Backend::Sdl3Detail {
    /** @copydoc ReadMacCapturePermission */
    AudioCapturePermission ReadMacCapturePermission() noexcept {
        using enum AudioCapturePermission;
        if (@available(macOS 10.14, *)) {
            switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
                case AVAuthorizationStatusNotDetermined:
                    return Required;
                case AVAuthorizationStatusAuthorized:
                    return Granted;
                case AVAuthorizationStatusRestricted:
                case AVAuthorizationStatusDenied:
                    return Denied;
            }
        }
        return Unknown;
    }
}  // namespace Horo::Audio::Backend::Sdl3Detail
