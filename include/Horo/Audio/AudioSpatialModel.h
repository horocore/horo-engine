#pragma once

/** @file AudioSpatialModel.h
 * @brief Bounded immutable handoff of scene spatial audio without scene or backend references.
 */
#include "Horo/Audio/AudioSoundReference.h"
#include "Horo/Math/SceneMath.h"

#include <array>
#include <cstddef>
#include <span>

namespace Horo::Audio {
    inline constexpr std::size_t MaximumSpatialAudioSources = 256;
    inline constexpr std::size_t MaximumSpatialAudioListeners = 16;
    inline constexpr std::uint32_t MaximumSpatialAudioViews = 16;

    /** @brief Process-local spatial identity scoped by an Audio-owned scene context, never serialized. */
    struct AudioSpatialIdentity final {
        AudioSceneContextHandle context;
        std::uint32_t slot{};
        std::uint32_t generation{};

        /** @brief Check complete generation-qualified identity shape. @return True for nonzero identity dimensions. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return context.IsValid() && slot != 0 && generation != 0;
        }

        auto operator<=>(const AudioSpatialIdentity &) const noexcept = default;
    };

    /** @brief World pose: proper rotation independent of transform scale, in right-handed Y-up scene space. */
    struct AudioSpatialPose final {
        Math::Vec3 position;
        Math::Quaternion orientation;
        bool operator==(const AudioSpatialPose &) const noexcept = default;
    };

    /** @brief Copied previous/current world poses and metres-per-second velocity; discontinuities suppress Doppler. */
    struct AudioSpatialMotion final {
        AudioSpatialPose previous;
        AudioSpatialPose current;
        Math::Vec3 velocity;
        std::uint64_t discontinuityRevision{};
        bool discontinuous{true};
    };

    /** @brief Extracted source values, containing no borrowed component, entity, provider or native reference. */
    struct AudioSpatialSource final {
        AudioSpatialIdentity identity;
        AudioSpatialMotion motion;
        AudioSoundReference sound;
        AudioSoundPlaybackDefaults playback;
    };

    /** @brief Extracted authored listener and selection metadata, with no ECS access from the callback. */
    struct AudioSpatialListener final {
        AudioSpatialIdentity identity;
        AudioSpatialMotion motion;
        std::uint32_t view{}; /**< Zero is the global view; 1..16 are explicitly assigned split views. */
        std::int32_t priority{};
        float weight{1.0F}; /**< Positive authored mixing weight, normalized only after selection. */
    };

    /** @brief Closed explicit listener policy; no camera discovery or silent listener fallback. */
    enum class AudioListenerPolicy : std::uint8_t {
        Primary,    /**< Highest priority, then lowest complete identity; one listener for the whole mix. */
        PerView,    /**< One highest-priority listener per exact authored view, ordered by view. */
        WeightedAll /**< Every enabled listener, ordered by identity with normalized authored weights. */
    };

    /** @brief Owns one complete bounded publication. Publish as const; consumers retain only their own value copy.
     * @details Extraction and transfer occur at the host's quiescent ownership boundary. The callback never
     * accesses Scene, and no method implies concurrent publication or audio-backend activation.
     */
    struct AudioSpatialFrame final {
        AudioSceneContextHandle context;
        std::uint64_t sequence{};
        AudioListenerPolicy policy{AudioListenerPolicy::Primary};
        std::array<AudioSpatialSource, MaximumSpatialAudioSources> sources;
        std::array<AudioSpatialListener, MaximumSpatialAudioListeners> listeners;
        std::size_t sourceCount{};
        std::size_t listenerCount{};

        /** @brief Access the initialized source prefix. @return Owned values valid for this frame's lifetime. */
        [[nodiscard]] std::span<const AudioSpatialSource> Sources() const noexcept {
            return {sources.data(), sourceCount};
        }

        /** @brief Access the selected listener prefix. @return Owned values valid for this frame's lifetime. */
        [[nodiscard]] std::span<const AudioSpatialListener> Listeners() const noexcept {
            return {listeners.data(), listenerCount};
        }
    };
}  // namespace Horo::Audio
