#include "Horo/Audio/AudioPlaybackRequest.h"

#include "Horo/Audio/AudioErrors.h"

#include <utility>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] Result<void> InvalidRequest() {
            return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        }

    }  // namespace

    /** @copydoc ValidateAudioPlaybackRequest */
    Result<void> ValidateAudioPlaybackRequest(const AudioPlaybackRequest &request) {
        if (!request.sceneContext.IsValid() || !request.sound.IsAssigned())
            return InvalidRequest();
        if (const Result<void> reference = ValidateAudioSoundReference(request.sound); reference.HasError())
            return reference;
        if (const Result<void> playback = ValidateAudioSoundPlaybackDefaults(request.playback); playback.HasError())
            return playback;
        if (const Result<void> lifecycle = ValidateAudioSceneLifecyclePolicy(request.sceneLifecycle); lifecycle.HasError())
            return lifecycle;
        return Result<void>::Success();
    }

    /** @copydoc MakeAudioPlaybackRequest */
    Result<AudioPlaybackRequest> MakeAudioPlaybackRequest(const AudioSceneContextHandle sceneContext, AudioSoundReference sound,
                                                          AudioPlaybackSettings playback, const AudioSceneLifecyclePolicy sceneLifecycle) {
        AudioPlaybackRequest request{.sceneContext = sceneContext,
                                     .sound = std::move(sound),
                                     .playback = std::move(playback),
                                     .sceneLifecycle = sceneLifecycle};
        if (const Result<void> validation = ValidateAudioPlaybackRequest(request); validation.HasError())
            return Result<AudioPlaybackRequest>::Failure(validation.ErrorValue());
        return Result<AudioPlaybackRequest>::Success(std::move(request));
    }
}  // namespace Horo::Audio
