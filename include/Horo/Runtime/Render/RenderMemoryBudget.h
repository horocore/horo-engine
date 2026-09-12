#pragma once

/**
 * @file RenderMemoryBudget.h
 * @brief Backend-neutral renderer allocation admission, accounting, and fragmentation contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Horo::Render {
    /** @brief Backend-neutral compatibility class for one renderer backing pool. */
    enum class RenderMemoryClass : std::uint8_t {
        PersistentDevice,
        Upload,
        Readback,
        Transient,
    };

    /** @brief Declares whether a native requirement is exact or conservatively estimated. */
    enum class RenderMemoryCostProvenance : std::uint8_t {
        Exact,
        Estimated,
    };

    /** @brief Selects shared-block suballocation or one dedicated backing allocation. */
    enum class RenderMemoryAllocationClass : std::uint8_t {
        Suballocated,
        Dedicated,
    };

    /** @brief Backend-neutral compatibility identity for requirements that may share one backing pool. */
    struct RenderMemoryCompatibilityId {
        std::uint64_t value{0};

        /** @brief Reports whether the compatibility class is non-zero. @return True for a usable compatibility identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryCompatibilityId &) const noexcept = default;
    };

    /** @brief Identifies one admitted host/editor/world/service scope incarnation. */
    struct RenderMemoryScopeId {
        std::uint64_t owner{0};
        std::uint64_t incarnation{0};

        /** @brief Reports whether both scope identity fields are non-zero. @return True for a usable scope identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner != 0 && incarnation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryScopeId &) const noexcept = default;
    };

    /** @brief Opaque identity of one backend-neutral compatible memory pool. */
    struct RenderMemoryPoolId {
        RenderResourceOwnerId renderer;
        std::uint64_t value{0};

        /** @brief Reports whether the pool owner and value are non-zero. @return True for a usable pool identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryPoolId &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one outstanding memory reservation. */
    struct RenderMemoryReservationId {
        RenderResourceOwnerId renderer;
        std::uint64_t value{0};

        /** @brief Reports whether the reservation owner and value are non-zero. @return True for a usable reservation identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryReservationId &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one committed suballocation or dedicated allocation. */
    struct RenderMemoryAllocationId {
        RenderResourceOwnerId renderer;
        std::uint64_t value{0};

        /** @brief Reports whether the allocation owner and value are non-zero. @return True for a usable allocation identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMemoryAllocationId &) const noexcept = default;
    };

    /** @brief Native-free requirements returned by a selected backend before allocation. */
    struct RenderMemoryCostPlan {
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice};
        RenderMemoryAllocationClass allocationClass{RenderMemoryAllocationClass::Suballocated};
        RenderMemoryCostProvenance provenance{RenderMemoryCostProvenance::Exact};
        RenderMemoryCompatibilityId compatibility; /**< Opaque equality key for backing-pool compatibility. */
        std::size_t payloadBytes{0};               /**< Logical descriptor/content bytes. */
        std::size_t requiredBytes{0};              /**< Native requirement including internal padding. */
        std::size_t alignment{1};                  /**< Required power-of-two placement alignment. */

        /** @brief Reports whether all values are known, bounded, and internally consistent. @return True for an admissible cost plan. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            const bool memoryClassValid = static_cast<std::uint8_t>(memoryClass) <= static_cast<std::uint8_t>(RenderMemoryClass::Transient);
            const bool allocationClassValid =
                static_cast<std::uint8_t>(allocationClass) <= static_cast<std::uint8_t>(RenderMemoryAllocationClass::Dedicated);
            const bool provenanceValid =
                static_cast<std::uint8_t>(provenance) <= static_cast<std::uint8_t>(RenderMemoryCostProvenance::Estimated);
            return memoryClassValid && allocationClassValid && provenanceValid && compatibility.IsValid() && payloadBytes > 0 &&
                   requiredBytes >= payloadBytes && alignment > 0 && (alignment & (alignment - 1U)) == 0;
        }
    };

    /** @brief Finite host-composed bounds for one frontend memory ledger. */
    struct RenderMemoryBudgetConfig {
        std::size_t hardCapBytes{512U * 1024U * 1024U};     /**< Maximum charged backing for this ledger. */
        std::size_t defaultBlockBytes{16U * 1024U * 1024U}; /**< Growth size for ordinary suballocation pools. */
        std::size_t maximumBlockBytes{64U * 1024U * 1024U}; /**< Largest permitted shared backing block. */
        std::size_t maximumAlignment{64U * 1024U};          /**< Largest admitted placement alignment. */
        std::uint32_t maximumPools{64};
        std::uint32_t maximumBlocks{1024};
        std::uint32_t maximumReservations{1024};
        std::uint32_t maximumAllocations{4096};
        std::uint64_t revision{1}; /**< Initial host budget revision captured by new reservations. */

        /** @brief Reports whether every bound is finite, non-zero, and mutually consistent. @return True for a usable configuration. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return hardCapBytes > 0 && defaultBlockBytes > 0 && defaultBlockBytes <= maximumBlockBytes &&
                   maximumBlockBytes <= hardCapBytes && maximumAlignment > 0 && (maximumAlignment & (maximumAlignment - 1U)) == 0 &&
                   maximumPools > 0 && maximumBlocks > 0 && maximumReservations > 0 && maximumAllocations > 0 &&
                   maximumReservations <= maximumAllocations && revision > 0;
        }
    };

    /** @brief Immutable placement returned only after a reservation is committed. */
    struct RenderMemoryAllocation {
        RenderMemoryAllocationId id;
        RenderMemoryPoolId pool;
        RenderMemoryScopeId scope;
        ResourceOperationId attempt;
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice};
        RenderMemoryCompatibilityId compatibility;
        RenderMemoryCostProvenance provenance{RenderMemoryCostProvenance::Exact};
        std::uint64_t budgetRevision{0};
        std::size_t offsetBytes{0};
        std::size_t payloadBytes{0};
        std::size_t requiredBytes{0};
        std::size_t backingBytes{0}; /**< Non-additive whole backing-block capacity. */
        RenderMemoryAllocationClass allocationClass{RenderMemoryAllocationClass::Suballocated};
    };

    /** @brief Non-additive accounting and fragmentation snapshot for one compatible memory pool. */
    struct RenderMemoryPoolSnapshot {
        RenderMemoryPoolId pool;
        RenderMemoryScopeId scope;
        RenderMemoryClass memoryClass{RenderMemoryClass::PersistentDevice};
        RenderMemoryCompatibilityId compatibility;
        std::size_t reservedUnallocatedBytes{0}; /**< Whole blocks charged before native allocation succeeds. */
        std::size_t committedBackingBytes{0};    /**< Whole native backing charged exactly once. */
        std::size_t reservedPayloadBytes{0};     /**< Payload represented by unconsumed reservations. */
        std::size_t livePayloadBytes{0};         /**< Payload represented by live allocations. */
        std::size_t retiringPayloadBytes{0};     /**< Payload awaiting reader-retirement acknowledgement. */
        std::size_t reusableSlackBytes{0};       /**< Free bytes in committed shared blocks only. */
        std::uint32_t blockCount{0};
        std::uint32_t reservationCount{0};
        std::uint32_t allocationCount{0};
        std::uint32_t externalFragmentationBasisPoints{0}; /**< Zero to 10,000 within this compatible pool. */
    };

    /** @brief Non-additive accounting snapshot for one frontend memory envelope. */
    struct RenderMemoryBudgetSnapshot {
        std::uint64_t revision{0};
        std::size_t hardCapBytes{0};
        std::size_t reservedUnallocatedBytes{0}; /**< Whole blocks charged before native allocation succeeds. */
        std::size_t committedBackingBytes{0};    /**< Whole native backing charged exactly once. */
        std::size_t reservedPayloadBytes{0};     /**< Payload represented by unconsumed reservations. */
        std::size_t livePayloadBytes{0};         /**< Payload represented by live allocations. */
        std::size_t retiringPayloadBytes{0};     /**< Payload awaiting reader-retirement acknowledgement. */
        std::size_t reusableSlackBytes{0};       /**< Sum of free bytes in compatible shared pools. */
        std::size_t peakChargedBytes{0};         /**< Historical peak of committed plus reserved backing. */
        std::uint64_t failedReservationCount{0};
        std::uint32_t poolCount{0};
        std::uint32_t blockCount{0};
        std::uint32_t reservationCount{0};
        std::uint32_t allocationCount{0};
        std::uint32_t externalFragmentationBasisPoints{0}; /**< Worst compatible pool, zero to 10,000. */
        bool overBudget{false};
        bool acceptingReservations{false};
    };

    /**
     * @brief Owns bounded renderer reservation, suballocation, and whole-block accounting.
     *
     * The host constructs one instance per frontend/device allowance. All methods and
     * destruction run serially on its host-declared render-capable thread. The class
     * is not thread-safe. A reservation charges new backing before native allocation;
     * Commit consumes that same claim rather than adding a second charge. Empty block
     * capacity returns to the budget only through ReclaimEmptyBlocks.
     */
    class RenderMemoryBudget final {
    public:
        /**
         * @brief Creates an empty ledger with preallocated bounded metadata storage.
         * @param renderer Exact owning frontend identity.
         * @param config Finite host-composed memory and record limits.
         * @return Owned ledger or a typed configuration/identity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderMemoryBudget>> Create(RenderResourceOwnerId renderer,
                                                                                RenderMemoryBudgetConfig config);

        /** @brief Releases all CPU-side accounting state after the backend has completed its shutdown contract. */
        ~RenderMemoryBudget();

        RenderMemoryBudget(const RenderMemoryBudget &) = delete;
        RenderMemoryBudget &operator=(const RenderMemoryBudget &) = delete;
        RenderMemoryBudget(RenderMemoryBudget &&) = delete;
        RenderMemoryBudget &operator=(RenderMemoryBudget &&) = delete;

        /**
         * @brief Reserves compatible slack or charges a new whole backing block before allocation.
         * @param scope Admitted owner-scope incarnation charged by this claim.
         * @param attempt Exact resource attempt correlated with the claim.
         * @param plan Valid backend-provided native-free requirements.
         * @return Outstanding reservation identity or a typed denial without ledger mutation.
         */
        [[nodiscard]] Result<RenderMemoryReservationId> Reserve(RenderMemoryScopeId scope, ResourceOperationId attempt,
                                                                const RenderMemoryCostPlan &plan);

        /**
         * @brief Cancels one unconsumed claim and returns any newly charged pending block capacity.
         * @param reservation Exact outstanding reservation owned by this ledger.
         * @return Success or a typed malformed, foreign, stale, or consumed-reservation failure.
         */
        [[nodiscard]] Result<void> Cancel(RenderMemoryReservationId reservation);

        /**
         * @brief Consumes one claim after native allocation succeeds and returns its stable placement record.
         * @param reservation Exact outstanding reservation owned by this ledger.
         * @return Immutable allocation placement or a typed reservation failure.
         */
        [[nodiscard]] Result<RenderMemoryAllocation> Commit(RenderMemoryReservationId reservation);

        /**
         * @brief Marks a live allocation as retiring while keeping all backing charged.
         * @param allocation Exact live allocation owned by this ledger.
         * @return Success or a typed malformed, foreign, stale, or state-incompatible failure.
         */
        [[nodiscard]] Result<void> BeginRetire(RenderMemoryAllocationId allocation);

        /**
         * @brief Acknowledges that all readers retired and converts the allocation region to reusable slack.
         * @param allocation Exact retiring allocation whose readers have completed.
         * @return Success or a typed malformed, foreign, stale, or state-incompatible failure.
         */
        [[nodiscard]] Result<void> AcknowledgeRetirement(RenderMemoryAllocationId allocation);

        /**
         * @brief Destroys up to a bounded number of empty committed blocks.
         * @param maxBlocks Maximum eligible blocks to remove during this owner-thread safe point.
         * @return Total charged backing capacity returned to the host envelope.
         */
        [[nodiscard]] std::size_t ReclaimEmptyBlocks(std::size_t maxBlocks) noexcept;

        /**
         * @brief Lowers or raises the finite host cap without erasing already issued charges.
         * @param hardCapBytes New non-zero accounting cap; it may be below existing charges.
         * @param revision Strictly newer host configuration revision.
         * @return Success or a typed invalid-cap or stale-revision failure.
         */
        [[nodiscard]] Result<void> ReviseHardCap(std::size_t hardCapBytes, std::uint64_t revision);

        /** @brief Returns a coherent non-additive accounting and fragmentation snapshot. @return Immutable value snapshot. */
        [[nodiscard]] RenderMemoryBudgetSnapshot Snapshot() const noexcept;

        /**
         * @brief Returns accounting for one exact compatible pool without combining incompatible slack.
         * @param pool Live pool identity issued by this ledger.
         * @return Pool snapshot or a typed malformed, foreign, or stale-pool failure.
         */
        [[nodiscard]] Result<RenderMemoryPoolSnapshot> PoolSnapshot(RenderMemoryPoolId pool) const;

        /**
         * @brief Stops admission and clears all records after backend-owned backing is gone; idempotent.
         * @pre The backend has acknowledged destruction of every backing allocation represented by this ledger.
         */
        void Shutdown() noexcept;

    private:
        class Impl;
        explicit RenderMemoryBudget(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> implementation_;
    };
}  // namespace Horo::Render
