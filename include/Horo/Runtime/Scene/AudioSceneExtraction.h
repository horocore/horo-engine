#pragma once

/** @file AudioSceneExtraction.h
 * @brief Single-owner scene-to-audio extraction with bounded motion history and explicit listener policy.
 */
#include "Horo/Audio/AudioSpatialModel.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Runtime {
    /** @brief Typed extraction failures; rejected captures never advance history. */
    namespace AudioSceneErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Closed;
    }  // namespace AudioSceneErrors

    /** @brief Owner-thread motion override; entity references never enter the published audio frame. */
    struct AudioSceneMotionInput final {
        EntityRef entity; /**< Exact live audio-bearing entity; parent motion changes use affected-object or global discontinuity. */
        std::optional<Math::Vec3> velocity;    /**< Explicit world velocity; absent derives from successive world positions. */
        std::uint64_t discontinuityRevision{}; /**< Change on teleport, tracking loss, reparenting or authority replacement. */
    };

    /** @brief Complete bounded extraction intent for one control update. */
    struct AudioSceneExtractionInput final {
        Audio::AudioSceneContextHandle context;
        std::uint64_t sequence{}; /**< Strictly increasing in this extractor, including across scene-context replacement. */
        float elapsedSeconds{};   /**< Finite positive interval; never inferred from callback or wall time. */
        Audio::AudioListenerPolicy policy{Audio::AudioListenerPolicy::Primary};
        std::span<const AudioSceneMotionInput> motion;
        bool discontinuous{}; /**< Global discontinuity suppresses all derived and explicit velocity for this frame. */
    };

    /** @brief Exclusive control-owner adapter; bounded steady-state work, no locks or callback-side ECS access.
     * @details Captures at most 256 sources, 16 authored listeners, 4096 scene slots and 64 ancestors per object.
     * History and output are owned values. Capture failure leaves history unchanged. The host supplies a fresh
     * Audio scene-context generation for every new Scene runtime and transfers completed frames as const values.
     * Shutdown closes capture and releases history; no references to Scene survive a call.
     */
    class AudioSceneExtractor final {
    public:
        /** @brief Begin an empty exclusively owned extraction timeline. */
        AudioSceneExtractor() = default;
        AudioSceneExtractor(const AudioSceneExtractor &) = delete;
        AudioSceneExtractor &operator=(const AudioSceneExtractor &) = delete;
        /** @brief Extract hierarchy-aware spatial state and deterministic listener selection.
         * @param scene Current borrowed runtime scene; must outlive only this call on its owner thread.
         * @param input Exact context, sequence, interval, policy and optional bounded motion overrides.
         * @return Complete owned frame, or typed stale-view/context, malformed, capacity or closed rejection.
         */
        [[nodiscard]] Result<Audio::AudioSpatialFrame> Capture(RuntimeSceneView scene, const AudioSceneExtractionInput &input);
        /** @brief Permanently close this owner and discard motion history after host detachment. */
        void Shutdown() noexcept;

    private:
        Audio::AudioSpatialFrame history_;
        SceneRuntimeId scene_;
        bool closed_{};
    };
}  // namespace Horo::Runtime
