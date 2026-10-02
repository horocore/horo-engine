#include "Horo/Audio/AudioVoiceControls.h"

namespace Horo::Audio {
    /** @copydoc ValidateAudioVoiceControlRequest */
    bool ValidateAudioVoiceControlRequest(const AudioVoiceControlRequest &request) noexcept {
        using enum AudioVoiceControl;
        return request.voice.IsValid() && request.control <= Cancel && (request.control == Seek || request.seekFrame == 0) &&
               (request.control == SetLoop || (!request.loop.enabled && request.loop.begin == 0 && request.loop.end == 0)) &&
               (request.control == SetPlaybackSpeed || request.playbackSpeed == 1.0);
    }
}  // namespace Horo::Audio
