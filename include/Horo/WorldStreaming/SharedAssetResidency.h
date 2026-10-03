#pragma once

/**
 * @file SharedAssetResidency.h
 * @brief Bounded accounting and fenced consumer leases for cache-owned shared assets.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/WorldStreaming/StreamingBudgetModel.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct SharedAssetRevisionTag;
        struct SharedAssetLeaseIdTag;
        struct SharedAssetChargeIdTag;
    }  // namespace Detail

    /** @brief Immutable content revision of a cache-owned asset allocation. */
    using SharedAssetRevision = Foundation::Detail::NonZeroId64<Detail::SharedAssetRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Non-reused lease identity within one residency-ledger owner lifetime. */
    using SharedAssetLeaseId = Foundation::Detail::NonZeroId64<Detail::SharedAssetLeaseIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Non-reused charge incarnation within one residency-ledger owner lifetime. */
    using SharedAssetChargeId = Foundation::Detail::NonZeroId64<Detail::SharedAssetChargeIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Exact cache allocation identity; changed content is a separate charge. */
    struct SharedAssetKey final {
        Assets::AssetId asset;        /**< Stable asset identity, not a cache pointer. */
        SharedAssetRevision revision; /**< Exact resident content revision. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SharedAssetKey &) const noexcept = default;
    };

    /** @brief One cell or feature-provider consumer of a cache-owned allocation. */
    struct SharedAssetConsumer final {
        StreamingFence fence;                              /**< Exact cell attempt; old attempts may retire after replacement. */
        std::optional<StreamingRuntimeServiceId> provider; /**< Empty for a cell lease, set for a feature-provider lease. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SharedAssetConsumer &) const noexcept = default;
    };

    /** @brief Exact token whose release ends one consumer's retention, not the cache owner's allocation. */
    struct SharedAssetLease final {
        StreamingRuntimeOwnerId owner; /**< Ledger lifetime, distinct from the asset-cache owner. */
        SharedAssetLeaseId id;         /**< Monotonic lease identity. */
        SharedAssetChargeId charge;    /**< Exact cache-allocation charge incarnation. */
        SharedAssetKey key;            /**< Charged allocation. */
        SharedAssetConsumer consumer;  /**< Exact cell/provider claimant. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SharedAssetLease &) const noexcept = default;
    };

    /** @brief Exact cache-retirement ticket; a late acknowledgement cannot release a successor charge. */
    struct SharedAssetRetirement final {
        StreamingRuntimeOwnerId owner; /**< Exact ledger lifetime. */
        SharedAssetChargeId charge;    /**< Non-reused allocation-charge incarnation. */
        SharedAssetKey key;            /**< Exact logical asset and content revision. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SharedAssetRetirement &) const noexcept = default;
    };

    /** @brief Fixed admission ceilings; each axis is an authority slice, not cache ownership. */
    struct SharedAssetResidencyLimits final {
        static constexpr std::uint32_t MaximumEntries = 4096;
        static constexpr std::uint32_t MaximumLeases = 16384;
        std::uint32_t entries{};         /**< Positive maximum distinct charged allocations. */
        std::uint32_t leases{};          /**< Positive maximum simultaneous consumer leases. */
        StreamingBudgetAmounts capacity; /**< Complete independent resource limits, never collapsed to one scalar. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Admission lifecycle; cancellation and shutdown retain charges until exact retirement. */
    enum class SharedAssetResidencyState : std::uint8_t {
        Accepting,
        Draining,
        Closed
    };

    /** @brief Read-only projection of one canonical cache charge; this value owns no allocation. */
    struct SharedAssetCharge final {
        SharedAssetChargeId id;      /**< Exact non-reused allocation incarnation. */
        StreamingBudgetAmounts cost; /**< Cache-reported independent resource amounts. */
        bool retiring{};             /**< New leases are closed while the cache retires this allocation. */
    };

    /**
     * @brief Streaming-authority ledger for one mounted partition and exact host owner lifetime.
     * @details Asset Pipeline/cache retains allocation ownership and reports actual retirement. This ledger only
     *          accounts its charge once and fences consumer retention. Mutations run on StreamingAuthorityRole;
     *          provider/worker completions route there. No cache pointer, allocator, or native resource is owned here.
     *          The host must drain leases and retirement acknowledgements before destroying this ledger.
     */
    class SharedAssetResidencyLedger final {
    public:
        SharedAssetResidencyLedger(const SharedAssetResidencyLedger &) = delete;
        SharedAssetResidencyLedger &operator=(const SharedAssetResidencyLedger &) = delete;
        SharedAssetResidencyLedger(SharedAssetResidencyLedger &&other) noexcept;
        SharedAssetResidencyLedger &operator=(SharedAssetResidencyLedger &&) = delete;

        /**
         * @brief Preallocates bounded accounting storage for one partition incarnation.
         * @param owner Exact mounted partition, epoch, and unique host owner.
         * @param limits Mandatory entry, lease, and resource-axis ceilings.
         * @return Empty accepting ledger or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<SharedAssetResidencyLedger> Create(const StreamingRuntimeOwnerToken &owner,
                                                                       const SharedAssetResidencyLimits &limits);

        /**
         * @brief Atomically obtains one lease, charging a new asset revision exactly once.
         * @param key Exact cache allocation identity.
         * @param residentCost Nonzero complete cache-reported allocation charge; must agree across consumers.
         * @param consumer Exact cell or provider attempt within this mounted owner.
         * @param currentAttempt Authority's current admission fence for this cell; no second cell-state machine is kept here.
         * @return Unique lease, or typed invalid/stale/conflict/capacity/lifecycle failure without partial charge.
         * @pre The cache owner has retained the allocation and the authority's aggregate reservation admits its bytes.
         */
        [[nodiscard]] Result<SharedAssetLease> Acquire(const SharedAssetKey &key, const StreamingBudgetAmounts &residentCost,
                                                       const SharedAssetConsumer &consumer, const StreamingFence &currentAttempt);

        /**
         * @brief Releases one exact consumer after its readers, jobs and native dependents retire.
         * @param lease Token returned by Acquire.
         * @return Success or typed invalid/stale failure; the allocation remains charged until cache retirement.
         */
        [[nodiscard]] Result<void> Release(const SharedAssetLease &lease);

        /**
         * @brief Revokes new leases for an unleased allocation while the cache owner retires it.
         * @param key Exact charged allocation.
         * @param charge Charge incarnation obtained with the accepted lease.
         * @return Exact retirement ticket or typed stale/lifecycle failure without changing the charge.
         */
        [[nodiscard]] Result<SharedAssetRetirement> BeginRetire(const SharedAssetKey &key, SharedAssetChargeId charge);

        /**
         * @brief Releases the single charge only after the cache owner confirms actual allocation retirement.
         * @param retirement Exact ticket returned by BeginRetire.
         * @return Success or typed stale/lifecycle failure; outstanding leases can never be silently dropped.
         */
        [[nodiscard]] Result<void> AcknowledgeRetired(const SharedAssetRetirement &retirement);

        /**
         * @brief Fences one cancelled cell attempt against further leases without releasing accepted consumers.
         * @param fence Exact mounted attempt; its outstanding leases remain releasable after cancellation.
         * @return Success (also for an idempotent repeat) or typed invalid/stale/capacity failure.
         */
        [[nodiscard]] Result<void> CancelAttempt(const StreamingFence &fence);

        /** @brief Closes new admission and retains all leases/charges through cancellation or shutdown. */
        void BeginShutdown() noexcept;

        /** @brief Returns the mounted owner fence. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Returns current admission lifecycle. @return Accepting, Draining, or Closed. */
        [[nodiscard]] SharedAssetResidencyState State() const noexcept;
        /** @brief Returns distinct cache allocations still charged. @return Entry count. */
        [[nodiscard]] std::size_t ChargedCount() const noexcept;
        /** @brief Reads one independent charged resource axis, including unleased/retiring allocations.
         * @param dimension Supported axis. @return Charged amount or typed unsupported-axis failure. */
        [[nodiscard]] Result<std::uint64_t> Charged(StreamingBudgetDimension dimension) const;
        /** @brief Returns consumer leases not yet released. @return Retained lease count. */
        [[nodiscard]] std::size_t LeaseCount() const noexcept;

        /** @brief Inspects one canonical charge without creating a lease or changing ownership.
         * @param key Exact asset/content identity. @return Current charge, empty for absence, or typed invalid error. */
        [[nodiscard]] Result<std::optional<SharedAssetCharge>> Inspect(const SharedAssetKey &key) const;

    private:
        struct Entry final {
            SharedAssetKey key;
            SharedAssetChargeId id;
            StreamingBudgetAmounts cost;
            bool retiring{};
        };

        struct ChargeCandidate final {
            SharedAssetChargeId id;
            bool isNew{};
        };

        SharedAssetResidencyLedger(const StreamingRuntimeOwnerToken &owner, const SharedAssetResidencyLimits &limits) noexcept;
        [[nodiscard]] Result<ChargeCandidate> PrepareCharge(const SharedAssetKey &key, const StreamingBudgetAmounts &residentCost) const;
        StreamingRuntimeOwnerToken owner_;
        SharedAssetResidencyLimits limits_;
        SharedAssetResidencyState state_{SharedAssetResidencyState::Accepting};
        std::array<std::uint64_t, StreamingBudgetDimensionCount> charged_{};
        std::uint64_t nextLeaseValue_{1};
        std::uint64_t nextChargeValue_{1};
        std::vector<Entry> entries_;
        std::vector<SharedAssetLease> leases_;
        std::vector<StreamingFence> cancelledAttempts_;
    };
}  // namespace Horo::WorldStreaming
