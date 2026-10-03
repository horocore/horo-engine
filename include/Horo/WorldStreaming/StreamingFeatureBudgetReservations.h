#pragma once

/** @file StreamingFeatureBudgetReservations.h
 * @brief Authority-owned feature slices, operation peaks and cache realization under one global capacity. */

#include "Horo/WorldStreaming/SharedAssetResidency.h"

#include <array>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Keeps aggregate reservation revisions distinct from policy and composition revisions. */
        struct StreamingFeatureBudgetRevisionTag;
    }  // namespace Detail

    /** @brief Non-wrapping revision of one aggregate reservation authority. */
    using StreamingFeatureBudgetRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingFeatureBudgetRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Independent feature partitions of the global capacity; General receives the unassigned remainder. */
    enum class StreamingBudgetFeature : std::uint8_t {
        Terrain,
        Foliage,
        Navigation,
        Physics,
        General,
        Count
    };
    /** @brief Number of complete feature partitions, excluding the sentinel. */
    inline constexpr std::size_t StreamingBudgetFeatureCount = static_cast<std::size_t>(StreamingBudgetFeature::Count);

    /** @brief Complete known resource vector for one feature; zero is explicit, never inferred. */
    struct StreamingFeatureBudgetAmount final {
        StreamingBudgetFeature feature{};
        StreamingBudgetAmounts amounts;
    };

    /** @brief Immutable complete five-feature peak plan with independent resource axes. */
    class StreamingFeatureBudgetPlan final {
    public:
        /** @brief Canonicalizes a complete plan. @param amounts Exactly one vector per feature, in any order.
         * @return Owned plan or typed invalid/unsupported result without retaining caller storage. */
        [[nodiscard]] static Result<StreamingFeatureBudgetPlan> Create(std::span<const StreamingFeatureBudgetAmount> amounts);
        /** @brief Reads one complete feature vector. @param feature Supported feature. @return Vector or typed unsupported result. */
        [[nodiscard]] Result<StreamingBudgetAmounts> Amounts(StreamingBudgetFeature feature) const;

    private:
        using Axes = std::array<std::uint64_t, StreamingBudgetDimensionCount>;
        using Matrix = std::array<Axes, StreamingBudgetFeatureCount>;

        explicit StreamingFeatureBudgetPlan(const Matrix &values) noexcept : values_(values) {}

        Matrix values_{};
        friend class StreamingFeatureBudgetPolicy;
        friend class StreamingFeatureBudgetReservations;
    };

    /** @brief Immutable partition of aggregate hard capacity; slices are not additive allowances. */
    class StreamingFeatureBudgetPolicy final {
    public:
        /** @brief Deducts four complete feature slices from aggregate hard limits using checked arithmetic.
         * @param aggregate Validated global seven-axis policy; its revision remains the configuration identity.
         * @param slices Exactly Terrain, Foliage, Navigation and Physics; General is derived, never supplied.
         * @return Owned partition or typed invalid/unsupported/capacity result; oversubscription never underflows General. */
        [[nodiscard]] static Result<StreamingFeatureBudgetPolicy> Create(const StreamingBudgetPolicy &aggregate,
                                                                         std::span<const StreamingFeatureBudgetAmount> slices);
        /** @brief Reads the global hard capacity. @return Complete resource vector. */
        [[nodiscard]] StreamingBudgetAmounts GlobalCapacity() const;
        /** @brief Reads a feature hard slice. @param feature Supported feature. @return Complete slice or typed unsupported result. */
        [[nodiscard]] Result<StreamingBudgetAmounts> Capacity(StreamingBudgetFeature feature) const;
        /** @brief Reads the immutable policy identity. @return Aggregate policy revision. */
        [[nodiscard]] StreamingBudgetPolicyRevision Revision() const noexcept;

    private:
        StreamingFeatureBudgetPolicy(StreamingBudgetPolicyRevision revision, const StreamingFeatureBudgetPlan::Axes &global,
                                     const StreamingFeatureBudgetPlan &capacity) noexcept
            : revision_(revision), global_(global), capacity_(capacity) {}

        StreamingBudgetPolicyRevision revision_;
        StreamingFeatureBudgetPlan::Axes global_{};
        StreamingFeatureBudgetPlan capacity_;
        friend class StreamingFeatureBudgetReservations;
    };

    /** @brief Complete mutation fence; workers route commands through the current StreamingAuthorityRole context. */
    struct StreamingFeatureBudgetContext final {
        StreamingRuntimeOwnerToken owner;             /**< Exact mounted authority lifetime. */
        StreamingBudgetPolicyRevision policyRevision; /**< Immutable aggregate configuration identity. */
        StreamingFeatureBudgetRevision revision;      /**< Current command-publication revision. */
    };

    /** @brief Exact operation reservation whose resource amounts remain canonical inside the authority. */
    struct StreamingFeatureBudgetReservation final {
        StreamingRuntimeOwnerToken owner;        /**< Original mounted authority; old tokens never route to a new owner. */
        StreamingSchedulerReservation scheduler; /**< Exact canonical operation reservation, not a mutable state snapshot. */
        /** @brief Checks token representation. @return True when owner and scheduler identities are usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const StreamingFeatureBudgetReservation &) const noexcept = default;
    };

    /** @brief Exact cache handoff; resident cost and resolved peak are separate accounting projections. */
    struct StreamingSharedAssetRealization final {
        StreamingBudgetFeature feature;      /**< Feature owning the resolved operation peak. */
        SharedAssetKey key;                  /**< Exact cache allocation and immutable content revision. */
        StreamingBudgetAmounts residentCost; /**< Complete cache-reported physical charge. */
        StreamingBudgetAmounts peakPortion;  /**< Equal to cost for new allocations; zero or a cost subset for known reuse. */
        SharedAssetConsumer consumer;        /**< Exact accepted operation fence and optional provider. */
    };

    /** @brief Versioned host composition and mandatory metadata ceilings; cache/native resources remain externally owned. */
    struct StreamingFeatureBudgetConfig final {
        /** @brief Supported aggregate admission/materialization protocol. */
        static constexpr std::uint32_t CurrentContractVersion = 1;
        std::uint32_t contractVersion{CurrentContractVersion}; /**< Unknown versions fail before host composition. */
        WorldStreamingRuntimeCompositionConfig runtime;        /**< Owned scheduler and borrowed service lifetime configuration. */
        std::uint32_t sharedEntries{};                         /**< Positive cache charge ceiling within SharedAssetResidencyLimits. */
        std::uint32_t sharedLeases{};                          /**< Positive consumer ceiling within SharedAssetResidencyLimits. */
    };

    /** @brief One owning composition of canonical scheduler, feature/global reservations and cache accounting.
     * @details All mutations run on StreamingAuthorityRole. Child ledgers are exposed const-only; host services/cache instances
     * must outlive all retained operations and shared charges. Successful mutations use bounded preallocated metadata.
     * Operation completion releases only unrealized peaks. Realized cache charges remain in their first admitted feature
     * until exact cache retirement acknowledgement, even after cancellation, replacement or operation release.
     * Policy/partition replacement creates a new owner lifetime and drains the old authority; it never resets retained usage. */
    class StreamingFeatureBudgetReservations final {
    public:
        StreamingFeatureBudgetReservations(const StreamingFeatureBudgetReservations &) = delete;
        StreamingFeatureBudgetReservations &operator=(const StreamingFeatureBudgetReservations &) = delete;
        StreamingFeatureBudgetReservations(StreamingFeatureBudgetReservations &&) noexcept = default;
        StreamingFeatureBudgetReservations &operator=(StreamingFeatureBudgetReservations &&) = delete;
        /** @brief Creates a detached complete authority before accepting work.
         * @param config Versioned runtime composition and bounded cache metadata limits.
         * @param services Borrowed host bindings; must outlive operation and cache retirement.
         * @param policy Immutable global/feature partition.
         * @return Empty owned composition or typed failure without publishing partial host state. */
        [[nodiscard]] static Result<StreamingFeatureBudgetReservations> Create(const StreamingFeatureBudgetConfig &config,
                                                                               std::span<const StreamingRuntimeServiceBinding> services,
                                                                               const StreamingFeatureBudgetPolicy &policy);
        /** @brief Atomically admits one queued operation plus complete feature peaks.
         * @param context Current owner/configuration/revision fence. @param operation Exact queued operation.
         * @param peak Complete known maximum costs; explicit zero allows cached-only consumers.
         * @param capacityUnits Positive canonical scheduler concurrency charge.
         * @param currentAttempt Authority's current canonical cell-attempt fence; no second cell-state machine is stored here.
         * @return Exact token or typed failure; neither scheduler nor budget mutates on rejection. */
        [[nodiscard]] Result<StreamingFeatureBudgetReservation> TryAdmit(const StreamingFeatureBudgetContext &context,
                                                                         const StreamingCellOperation &operation,
                                                                         const StreamingFeatureBudgetPlan &peak,
                                                                         std::uint64_t capacityUnits, const StreamingFence &currentAttempt);
        /** @brief Reserves additional peak before allocation. @param context Current fence. @param reservation Exact operation.
         * @param additional Complete additional known costs. @return Success or typed failure without partial growth. */
        [[nodiscard]] Result<void> Grow(const StreamingFeatureBudgetContext &context, const StreamingFeatureBudgetReservation &reservation,
                                        const StreamingFeatureBudgetPlan &additional);
        /** @brief Routes a lifecycle command to the sole canonical operation owner.
         * @param context Current fence. @param reservation Exact operation. @param transition Requested lifecycle command.
         * @return Canonical successor or typed failure; interruptions retain all capacity through retirement. */
        [[nodiscard]] Result<StreamingCellOperation> Advance(const StreamingFeatureBudgetContext &context,
                                                             const StreamingFeatureBudgetReservation &reservation,
                                                             StreamingCellOperationTransition transition);
        /** @brief Releases unrealized peaks only after canonical terminal completion.
         * @param context Current fence. @param reservation Exact operation. @return Success or typed failure; cache charges remain. */
        [[nodiscard]] Result<void> Release(const StreamingFeatureBudgetContext &context,
                                           const StreamingFeatureBudgetReservation &reservation);
        /** @brief Atomically realizes a reserved peak or reuses an already charged cache allocation.
         * @param context Current fence. @param reservation Exact active load/activation operation.
         * @param realization Exact cache/consumer facts and reserved portion resolved by this handoff.
         * For a new allocation peakPortion must equal residentCost. For cache reuse it may be zero or a componentwise cost subset.
         * @return Exact consumer lease or typed failure with both ledgers unchanged.
         * @pre Cache owner retains the allocation; no scratch/upload allocation is released merely by this handoff. */
        [[nodiscard]] Result<SharedAssetLease> RealizeShared(const StreamingFeatureBudgetContext &context,
                                                             const StreamingFeatureBudgetReservation &reservation,
                                                             const StreamingSharedAssetRealization &realization);
        /** @brief Releases exact consumer retention after its dependents retire. @param context Current fence.
         * @param lease Exact accepted lease. @return Success or typed failure; allocation budget remains charged. */
        [[nodiscard]] Result<void> ReleaseShared(const StreamingFeatureBudgetContext &context, const SharedAssetLease &lease);
        /** @brief Closes new leases for an unleased cache allocation. @param context Current fence. @param key Exact allocation.
         * @param charge Exact charge incarnation. @return Retirement ticket or typed failure without releasing budget. */
        [[nodiscard]] Result<SharedAssetRetirement> BeginRetireShared(const StreamingFeatureBudgetContext &context,
                                                                      const SharedAssetKey &key, SharedAssetChargeId charge);
        /** @brief Returns realized capacity only after actual cache retirement. @param context Current fence.
         * @param retirement Exact accepted retirement ticket. @return Success or typed failure; stale tickets cannot free a successor. */
        [[nodiscard]] Result<void> AcknowledgeSharedRetired(const StreamingFeatureBudgetContext &context,
                                                            const SharedAssetRetirement &retirement);
        /** @brief Replaces borrowed bindings only when operations and cache charges have drained.
         * @param context Current fence. @param revision Strictly newer composition revision. @param services Complete bindings.
         * @return Success or typed failure without publishing partial replacement. */
        [[nodiscard]] Result<void> ReplaceServices(const StreamingFeatureBudgetContext &context,
                                                   StreamingRuntimeCompositionRevision revision,
                                                   std::span<const StreamingRuntimeServiceBinding> services);
        /** @brief Closes admission; exact operations, leases and cache retirement remain routable.
         * @param context Current fence. @return Success (idempotent) or typed failure. */
        [[nodiscard]] Result<void> BeginShutdown(const StreamingFeatureBudgetContext &context);
        /** @brief Gets the current mutation fence. @return Owner, immutable policy and ledger revision. */
        [[nodiscard]] StreamingFeatureBudgetContext Context() const noexcept;
        /** @brief Gets aggregate lifecycle including retained cache allocations. @return Accepting, Draining or Closed. */
        [[nodiscard]] StreamingSchedulerAdmissionState State() const noexcept;
        /** @brief Reads reserved plus realized usage. @param feature Slice. @param dimension Resource axis.
         * @return Exact amount or typed unsupported result. */
        [[nodiscard]] Result<std::uint64_t> Used(StreamingBudgetFeature feature, StreamingBudgetDimension dimension) const;
        /** @brief Reads aggregate reserved plus realized usage. @param dimension Resource axis.
         * @return Exact amount or typed unsupported result; shared bytes are counted once. */
        [[nodiscard]] Result<std::uint64_t> Used(StreamingBudgetDimension dimension) const;
        /** @brief Inspects host composition without mutable bypass. @return Owned canonical runtime composition. */
        [[nodiscard]] const WorldStreamingRuntimeComposition &Runtime() const noexcept;
        /** @brief Inspects cache accounting without mutable bypass. @return Owned shared ledger. */
        [[nodiscard]] const SharedAssetResidencyLedger &SharedAssets() const noexcept;

    private:
        using Matrix = StreamingFeatureBudgetPlan::Matrix;

        struct Entry {
            StreamingFeatureBudgetReservation reservation;
            Matrix remaining;
        };

        struct ChargeSlice {
            SharedAssetChargeId charge;
            StreamingBudgetFeature feature;
        };

        StreamingFeatureBudgetReservations(WorldStreamingRuntimeComposition runtime, SharedAssetResidencyLedger shared,
                                           const StreamingFeatureBudgetPolicy &policy) noexcept;
        [[nodiscard]] Result<void> Validate(const StreamingFeatureBudgetContext &context) const;
        [[nodiscard]] Result<std::size_t> Find(const StreamingFeatureBudgetReservation &reservation, bool requireActive = false) const;
        [[nodiscard]] bool Fits(const Matrix &additional) const noexcept;
        void Apply(const Matrix &amounts, bool release) noexcept;
        void Publish() noexcept;
        WorldStreamingRuntimeComposition runtime_;
        SharedAssetResidencyLedger shared_;
        StreamingFeatureBudgetPolicy policy_;
        StreamingFeatureBudgetRevision revision_{StreamingFeatureBudgetRevision::Create(1).Value()};
        Matrix used_{};
        std::vector<Entry> entries_;
        std::vector<ChargeSlice> chargeSlices_;
    };
}  // namespace Horo::WorldStreaming
