#pragma once

/**
 * @file EffectInstancePool.h
 * @brief Prepared scene-scoped effect playback slots with allocation-free admission and retirement.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Vfx/VfxIdentity.h"
#include "Horo/Vfx/VfxQualityPolicy.h"

#include <cstdint>
#include <memory>

namespace Horo::Vfx {
    namespace Detail {
        struct EffectInstancePoolState;
    }

    /** @brief Per-effect compiled demand used to size one scene-owned pool before playback. */
    struct EffectPoolDescriptor final {
        VfxIdentityScope scene{};            /**< Exact scene incarnation; never shared across scenes. */
        std::uint32_t maximumInstances{};    /**< Authored per-effect concurrency ceiling. */
        std::uint32_t emittersPerInstance{}; /**< Cooked emitter slots charged per instance. */
        std::uint64_t bytesPerInstance{};    /**< Prepared payload charge, excluding pool control records. */
    };

    /** @brief Explicit scene-ledger slice reserved for this effect by host composition. */
    struct EffectPoolBudget final {
        std::uint32_t maximumInstances{};    /**< Instance-slot reservation. */
        std::uint32_t maximumEmitterSlots{}; /**< Emitter-slot reservation. */
        std::uint64_t maximumBytes{};        /**< Total pool control, delay, and instance payload reservation. */
        std::uint32_t requiredReserve{};     /**< Slots inaccessible to cosmetic admissions. */
        std::uint32_t maximumDelayed{};      /**< Preallocated cosmetic request records. */
    };

    /** @brief Pool-local overload modes; other ADR-128 modes require an outer cooked binding planner. */
    enum class EffectPoolOverBudgetPolicy : std::uint8_t {
        RejectNewest,
        DelayBounded,
        Count
    };

    /** @brief Immutable overload policy captured at preparation, never changed by telemetry. */
    struct EffectPoolPolicy final {
        EffectPoolOverBudgetPolicy overBudget{EffectPoolOverBudgetPolicy::RejectNewest};
        std::uint64_t maximumDelayTicks{}; /**< Positive expiry bound only for DelayBounded. */
    };

    /** @brief Data-derived materialization and ledger charges. */
    struct EffectPoolPlan final {
        std::uint32_t capacity{};
        std::uint32_t requiredReserve{};
        std::uint32_t delayedCapacity{};
        std::uint64_t reservedEmitterSlots{};
        std::uint64_t reservedBytes{};
    };

    /** @brief Frozen owner and ordering evidence for one playback attempt. */
    struct EffectPlaybackRequest final {
        VfxIdentityScope owner{};                                       /**< Owner identity validated by the scene boundary. */
        std::uint64_t ownerGeneration{};                                /**< Exact owner incarnation. */
        std::uint64_t tick{};                                           /**< Monotonic scene admission tick. */
        VfxRequirementClass requirement{VfxRequirementClass::Cosmetic}; /**< Required requests never delay or degrade. */
    };

    /** @brief Allocation-free playback and lifecycle result; no error string is built on the hot path. */
    enum class EffectPoolStatus : std::uint8_t {
        Admitted,
        Delayed,
        Rejected,
        DelayQueueFull,
        DelayExpired,
        Cancelled,
        Waiting,
        Empty,
        InvalidRequest,
        InvalidHandle,
        StaleHandle,
        RetentionPending,
        GenerationExhausted,
        ShutDown,
        ThreadViolation
    };

    /** @brief Exact admission or delayed-drain outcome. */
    struct EffectPoolOutcome final {
        EffectPoolStatus status{EffectPoolStatus::Empty};
        EffectSystemId instance{};     /**< Populated only for Admitted. */
        std::uint64_t delayedTicket{}; /**< Populated for Delayed, DelayExpired, Cancelled, or drained Admitted. */
    };

    /** @brief Allocation-free owner cancellation report. */
    struct EffectPoolCancelOutcome final {
        EffectPoolStatus status{EffectPoolStatus::Empty};
        std::uint32_t stopped{};
        std::uint32_t cancelledDelayed{};
    };

    /** @brief Owner-visible immutable slot facts; a handle is not a raw instance pointer. */
    struct EffectInstanceSnapshot final {
        EffectSystemId instance{};
        VfxIdentityScope owner{};
        std::uint64_t ownerGeneration{};
        std::uint64_t admittedTick{};
        VfxRequirementClass requirement{};
        std::uint32_t retainedReaders{};
        bool retiring{};
    };

    /** @brief Allocation-free capacity, overload, and retirement counters. */
    struct EffectPoolStatistics final {
        EffectPoolPlan plan{};
        std::uint32_t active{};
        std::uint32_t retiring{};
        std::uint32_t permanentlyRetired{};
        std::uint32_t delayed{};
        std::uint32_t peakActive{};
        std::uint64_t rejected{};
        std::uint64_t expired{};
        std::uint64_t cancelled{};
    };

    /**
     * @brief One scene-owner-thread pool for one compiled effect's logical playback slots.
     *
     * Prepare is the only allocating operation. No slot is reusable after Stop until every
     * registered reader/job/snapshot/GPU retention is acknowledged and CompleteRetirement
     * succeeds. The owning VfxWorld must outlive those dependents. Required capacity is
     * partitioned from cosmetic work; sleeping instances remain Active and charged.
     * The host ledger must reserve Statistics().plan.reservedBytes, including the declared
     * per-instance payload, plus any other resource charges not included there before
     * activation. This class owns logical slots, not renderer resources.
     */
    class EffectInstancePool final {
    public:
        EffectInstancePool(const EffectInstancePool &) = delete;
        EffectInstancePool &operator=(const EffectInstancePool &) = delete;
        /** @brief Transfers prepared pool ownership without changing owner-thread affinity. @param other Source pool. */
        EffectInstancePool(EffectInstancePool &&other) noexcept;
        /** @brief Replaces this pool after dependent work is quiescent. @param other Source pool. @return This pool. */
        EffectInstancePool &operator=(EffectInstancePool &&other) noexcept;
        /** @brief Releases prepared storage after dependents quiesce. */
        ~EffectInstancePool();

        /** @brief Validates and preallocates the finite scene-ledger slice.
         * @param descriptor Cooked per-effect demand and scene incarnation.
         * @param budget Host-reserved scene-ledger slice.
         * @param policy Immutable overload mode and delay bound.
         * @return Pool or typed invalid/over-budget/allocation failure.
         */
        [[nodiscard]] static Result<EffectInstancePool> Prepare(const EffectPoolDescriptor &descriptor, const EffectPoolBudget &budget,
                                                                const EffectPoolPolicy &policy);

        /** @brief Attempts playback without allocation or growth. @param request Frozen owner/order facts. @return Admission or overload
         * status. */
        [[nodiscard]] EffectPoolOutcome Play(const EffectPlaybackRequest &request) noexcept;
        /** @brief Processes at most the oldest delayed record at a tick boundary. @param tick Monotonic scene tick. @return One outcome. */
        [[nodiscard]] EffectPoolOutcome PumpDelayed(std::uint64_t tick) noexcept;
        /** @brief Restarts a quiescent live slot with a new generation. @param instance Exact live identity. @param tick New start tick.
         * @return New identity or a typed status; registered retained readers prohibit restart. */
        [[nodiscard]] EffectPoolOutcome Restart(EffectSystemId instance, std::uint64_t tick) noexcept;
        /** @brief Marks one live slot Retiring without returning its capacity. @param instance Exact identity. @return Transition status.
         */
        [[nodiscard]] EffectPoolStatus Stop(EffectSystemId instance) noexcept;
        /** @brief Records one dependent reader, job, snapshot, or GPU lease before handoff. @param instance Exact identity.
         * @return Status; the caller must acknowledge exactly once. */
        [[nodiscard]] EffectPoolStatus Retain(EffectSystemId instance) noexcept;
        /** @brief Acknowledges one retained dependent, even after Stop. @param instance Exact identity. @return Status. */
        [[nodiscard]] EffectPoolStatus Acknowledge(EffectSystemId instance) noexcept;
        /** @brief Reclaims a stopped slot only after all dependents retire. @param instance Exact identity. @return Status. */
        [[nodiscard]] EffectPoolStatus CompleteRetirement(EffectSystemId instance) noexcept;
        /** @brief Cancels queued requests and stops live instances for one exact owner incarnation.
         * @param owner Owner identity.
         * @param ownerGeneration Exact owner incarnation.
         * @return Typed counts or status.
         */
        [[nodiscard]] EffectPoolCancelOutcome CancelOwner(VfxIdentityScope owner, std::uint64_t ownerGeneration) noexcept;
        /** @brief Closes admission and stops all live slots, retaining dependent charges until acknowledgements. @return Status. */
        [[nodiscard]] EffectPoolStatus Shutdown() noexcept;
        /** @brief Returns a value snapshot of one live or retiring slot. @param instance Exact identity. @param output Filled on success.
         * @return Status; no pointer escapes the pool. */
        [[nodiscard]] EffectPoolStatus Inspect(EffectSystemId instance, EffectInstanceSnapshot &output) const noexcept;
        /** @brief Reads finite plan and counters without allocation. @return Value statistics. */
        [[nodiscard]] EffectPoolStatistics Statistics() const noexcept;
        /** @brief Reports whether all logical slots and dependents have retired. @return True when destruction is safe. */
        [[nodiscard]] bool Quiescent() const noexcept;

    private:
        explicit EffectInstancePool(std::unique_ptr<Detail::EffectInstancePoolState> state) noexcept;
        std::unique_ptr<Detail::EffectInstancePoolState> state_;
    };
}  // namespace Horo::Vfx
