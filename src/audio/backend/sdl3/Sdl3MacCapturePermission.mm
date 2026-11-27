/**
 * @file Sdl3MacCapturePermission.mm
 * @brief Observation and explicit macOS microphone authorization requests.
 */

#include "Sdl3AudioBackendImpl.h"

#import <AVFoundation/AVFoundation.h>
#import <Security/SecTask.h>
#include <atomic>

namespace Horo::Audio::Backend::Sdl3Detail {
    namespace {
        // TCC permission is process-scoped; the completion never accesses a backend or editor object.
        std::atomic<bool> requestPending{};

        /** @brief Checks the app-wide privacy explanation before AVFoundation can display its alert. */
        bool HasUsageDescription() noexcept {
            @autoreleasepool {
                id value = [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"];
                if (![value isKindOfClass:NSString.class])
                    return false;
                NSString *message = (NSString *)value;
                return [message stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length != 0;
            }
        }

        /** @brief Reads the entitlement of the running signature; a source plist is not evidence. */
        bool HasAudioInputEntitlement() noexcept {
            const auto task = SecTaskCreateFromSelf(nullptr);
            if (!task)
                return false;
            const auto value = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.device.audio-input"), nullptr);
            CFRelease(task);
            const bool enabled = value == kCFBooleanTrue;
            if (value)
                CFRelease(value);
            return enabled;
        }
    }  // namespace

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

    /** @copydoc MacCapturePackageReady */
    bool MacCapturePackageReady() noexcept {
        return HasUsageDescription() && HasAudioInputEntitlement();
    }

    /** @copydoc RequestMacCapturePermission */
    Result<Sdl3CapturePermissionRequest> RequestMacCapturePermission() {
        using Output = Result<Sdl3CapturePermissionRequest>;
        using enum Sdl3CapturePermissionRequest;
        if (!NSThread.isMainThread)
            return Output::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (!MacCapturePackageReady())
            return Output::Failure(
                MakeError(AudioErrors::CapabilityUnavailable, "The macOS app lacks microphone usage metadata or signing entitlement."));
        const auto permission = ReadMacCapturePermission();
        if (permission == AudioCapturePermission::Unknown)
            return Output::Failure(MakeError(AudioErrors::OperationUnsupported));
        if (permission != AudioCapturePermission::Required)
            return Output::Success(DecisionAvailable);
        if (requestPending.exchange(true))
            return Output::Success(AlreadyPending);
        if (@available(macOS 10.14, *)) {
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL) {
                                       requestPending.store(false);
                                     }];
        }
        return Output::Success(Requested);
    }
}  // namespace Horo::Audio::Backend::Sdl3Detail

namespace Horo::Audio::Backend {
    /** @copydoc ObserveSdl3MacCapturePermission */
    Sdl3MacCapturePermission ObserveSdl3MacCapturePermission() noexcept {
        if (!Sdl3Detail::MacCapturePackageReady())
            return {};
        return {Sdl3Detail::ReadMacCapturePermission(), Sdl3Detail::requestPending.load()};
    }

    /** @copydoc RequestSdl3MacCapturePermission */
    Result<Sdl3CapturePermissionRequest> RequestSdl3MacCapturePermission() {
        return Sdl3Detail::RequestMacCapturePermission();
    }
}  // namespace Horo::Audio::Backend
