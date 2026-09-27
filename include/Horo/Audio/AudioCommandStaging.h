#pragma once

/** @file AudioCommandStaging.h
 * @brief Bounded multi-producer ingress, control-owned scene admission and ordered callback publication.
 */
#include "Horo/Audio/AudioCommandBuffer.h"

#include <memory>
#include <optional>

namespace Horo::Audio {
    /** @brief Fixed-size ingress/control outcomes; failed admission retains work with its caller. */
    enum class AudioCommandStagingStatus : std::uint8_t {
        Ok,
        Coalesced,
        Busy,
        OrdinaryFull,
        CriticalRetry,
        InvalidCommand,
        InvalidScene,
        Closed,
        SequenceExhausted,
        AlreadyStaged,
        OutputFull,
        Inactive,
        ProtocolError
    };

    /** @brief Accepted sequence and optional replaced parameter-snapshot sequence; zero means no new admission. */
    struct AudioCommandAdmission {
        AudioCommandStagingStatus status{AudioCommandStagingStatus::Inactive};
        std::uint64_t sequence{};
        std::uint64_t replacedSequence{};
    };

    /** @brief Bounded control pump facts, not callback execution acknowledgements. */
    struct AudioCommandPumpResult {
        AudioCommandStagingStatus status{AudioCommandStagingStatus::Inactive};
        std::uint32_t published{};
    };

    /** @brief Control-safe-point occupancy facts, excluding rejected caller-owned commands. */
    struct AudioCommandStagingStats final {
        std::uint32_t ingressDepth{};  /**< Accepted records awaiting control publication. */
        std::uint32_t callbackDepth{}; /**< Published records awaiting callback consumption. */
    };

    /** @brief Independent bounded reservations, prepared before starting any producer or callback. */
    struct AudioCommandStagingDescriptor {
        AudioCommandBufferDescriptor callback;
        AudioMemoryPoolId ingressIdentity; /**< Distinct from callback.storageIdentity in the same runtime. */
        std::uint32_t ingressSlots{};      /**< Power of two in [2, MaximumAudioCommandSlots]. */
        std::uint32_t criticalSlots{};     /**< Positive and smaller than ingressSlots. */
        std::uint32_t sceneSlots{};        /**< Upper bound on one-based context slots, at most MaximumAudioHandleSlots. */
        std::size_t ingressBudgetBytes{};  /**< Includes ingress record/pool metadata and scene gates; excludes owner/mutex overhead. */
    };

    /**
     * @brief One runtime epoch's multi-producer ingress and single-control-to-callback transport.
     * Submit may run on producer threads. All other mutating methods except TryConsume belong to one control owner.
     * TryConsume/IsDrained belong to one callback consumer and never acquire the ingress mutex.
     * Hosts must stop/join producers and detach/join the callback before move/destruction; no method performs that join.
     * Before destruction, accepted work must also be drained or explicitly reconciled by the control runtime.
     */
    class AudioCommandStaging final {
    public:
        /** @brief Allocate all bounded storage off-callback. @param descriptor Explicit reservations and epoch.
         * @return Prepared staging owner or typed invalid/budget/allocation failure, with no partial publication.
         */
        [[nodiscard]] static Result<AudioCommandStaging> Create(const AudioCommandStagingDescriptor &descriptor);
        /** @brief Destroy only after every participant is quiescent, outside the callback. */
        ~AudioCommandStaging();
        /** @brief Transfer quiescent ownership. @param other Source, left inert. */
        AudioCommandStaging(AudioCommandStaging &&other) noexcept;
        /** @brief Replace quiescent storage off-callback. @param other Source. @return This owner. */
        AudioCommandStaging &operator=(AudioCommandStaging &&other) noexcept;
        AudioCommandStaging(const AudioCommandStaging &) = delete;
        AudioCommandStaging &operator=(const AudioCommandStaging &) = delete;

        /** @brief Control-only registration of an admitted scene/host context generation.
         * @param scene Valid context within the configured slot bound. Reuse requires a greater generation and acknowledged retirement.
         * @return Ok or typed Busy/InvalidScene/Closed/Inactive, without changing a rejected generation.
         */
        [[nodiscard]] AudioCommandStagingStatus RegisterScene(AudioSceneContextHandle scene) noexcept;
        /** @brief Normalize and try-admit one non-barrier producer intent, never blocking on the mutex.
         * @param command Exact runtime/epoch and registered open scene. Stop/release may finish a closing, not-yet-barriered scene.
         * @return Ok/Coalesced with ordered sequence, or explicit rejection; Busy/full/retry never accepts or retains the input.
         * Only adjacent unpublished parameter snapshots may coalesce. Voice/clip/pool liveness and retained leases remain control facts.
         * Reset and scene-unload payloads are control-only and rejected here. Required rejected work stays caller-owned for
         * retry/reconciliation.
         */
        [[nodiscard]] AudioCommandAdmission Submit(const AudioCommand &command) noexcept;
        /** @brief Close ordinary scene admission and enqueue an ordered critical unload barrier.
         * @param scene Exact registered context. On CriticalRetry, admission remains closed and control must retry the barrier.
         * @return Accepted sequence, idempotent AlreadyStaged sequence, or rejection. Busy makes no state change.
         */
        [[nodiscard]] AudioCommandAdmission StageSceneUnload(AudioSceneContextHandle scene) noexcept;
        /** @brief Control-only retirement after a verified callback-execution acknowledgement.
         * @param scene Exact closing generation. @param sequence Its published unload barrier sequence.
         * @return Ok or InvalidScene/Busy/Inactive. Publication is checked but cannot prove execution on its own.
         * @pre Control has verified the matching execution acknowledgement and resource-lifetime boundary, not merely consumed a record.
         */
        [[nodiscard]] AudioCommandStagingStatus AcknowledgeScene(AudioSceneContextHandle scene, std::uint64_t sequence) noexcept;
        /** @brief Close ordinary epoch admission and enqueue its final critical reset barrier.
         * @return Accepted/idempotent sequence or retry. CriticalRetry leaves ordinary admission closed; stop/release may still enter
         * before reset. Once queued, all ingress closes. A new runtime epoch requires a new owner after quiescence, never in-place reuse.
         */
        [[nodiscard]] AudioCommandAdmission StageReset() noexcept;
        /** @brief Publish at most maximumRecords from the ingress FIFO, stopping at its first blocked record.
         * @param maximumRecords Bounded work count, clamped to current queue depth; zero publishes nothing but may finish an empty close.
         * @return Published count and status. OutputFull/ProtocolError preserve the blocked record; no later work bypasses it.
         */
        [[nodiscard]] AudioCommandPumpResult Pump(std::uint32_t maximumRecords) noexcept;
        /** @brief Close all producer admission without dropping accepted records; Pump completes ordered draining.
         * @return Ok/Busy/Inactive. Host must stage required stop/release/barrier work before closing.
         */
        [[nodiscard]] AudioCommandStagingStatus Close() noexcept;
        /** @brief Callback-only copy of at most one published record, without locks/allocation/free.
         * @param record Output unchanged on false. @return True for one owned command record.
         */
        [[nodiscard]] bool TryConsume(AudioCommandRecord &record) noexcept;
        /** @brief Callback-only closed-and-empty check. @return Drain state, not proof of execution or device detachment. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Tries to sample actual ingress and callback occupancies without blocking producers.
         * @return Bounded facts, or no sample when ingress is contended or inert.
         */
        [[nodiscard]] std::optional<AudioCommandStagingStats> Stats() const noexcept;

    private:
        struct State;
        /** @brief Publish fully prepared state. */
        explicit AudioCommandStaging(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
