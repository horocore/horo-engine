#pragma once

/**
 * @file AudioPlaybackRequest.h
 * @brief Backend-neutral transient audio playback intent bound to a scene context.
 */

#include "Horo/Audio/AudioSoundReference.h"

namespace Horo::Audio {
    /**
     * @brief One transient request to start a sound in an admitted scene context.
     *
     * The request owns only semantic identities and value policies. It contains
     * no prepared clip, provider, native object, or live voice handle. The Audio
     * control owner resolves those resources and may reject the request when the
     * referenced scene context has closed.
     */
    struct AudioPlaybackRequest final {
        AudioSceneContextHandle sceneContext; /**< Generation-checked context associated with the request. */
        AudioSoundReference sound;            /**< Persistent sound identity to resolve before voice admission. */
        AudioPlaybackSettings playback;       /**< Gain, routing, admission, loop, and spatial policy. */
        AudioSceneLifecyclePolicy sceneLifecycle{AudioSceneLifecyclePolicy::StopOnUnload}; /**< Teardown policy. */

        /**
         * @brief Reports whether this request needs a spatial-provider implementation.
         * @return False for a two-dimensional request, which routes directly to its bus.
         */
        [[nodiscard]] constexpr bool RequiresSpatialProvider() const noexcept {
            return playback.spatialMode != AudioSpatialMode::TwoD;
        }

        /** @brief Reports whether this request is bound to a usable scene-context generation. */
        [[nodiscard]] constexpr bool IsSceneBound() const noexcept {
            return sceneContext.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const AudioPlaybackRequest &) const noexcept = default;
    };

    /** @brief Compatibility name for transient source playback intent. */
    using AudioSourcePlaybackRequest = AudioPlaybackRequest;

    /** @brief Compatibility name for gameplay-facing transient playback intent. */
    using AudioPlaybackIntent = AudioPlaybackRequest;

    /**
     * @brief Validates a transient playback request without resolving assets, buses, providers, or voices.
     * @param request Candidate request copied from a producer-owned snapshot.
     * @return Success, PlaybackRequestInvalid for missing request identity, or the underlying typed validation error.
     */
    [[nodiscard]] Result<void> ValidateAudioPlaybackRequest(const AudioPlaybackRequest &request);

    /**
     * @brief Builds a scene-bound transient request from persistent sound and playback values.
     * @param sceneContext Exact active context generation that owns the request.
     * @param sound Persistent sound reference; it must be assigned.
     * @param playback Backend-neutral playback values.
     * @param sceneLifecycle Teardown behavior for the request's context.
     * @return Valid request, PlaybackRequestInvalid for missing request identity, or the underlying typed validation error.
     */
    [[nodiscard]] Result<AudioPlaybackRequest> MakeAudioPlaybackRequest(
        AudioSceneContextHandle sceneContext, AudioSoundReference sound, AudioPlaybackSettings playback,
        AudioSceneLifecyclePolicy sceneLifecycle = AudioSceneLifecyclePolicy::StopOnUnload);
}  // namespace Horo::Audio
