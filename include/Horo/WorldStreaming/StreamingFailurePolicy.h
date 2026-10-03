#pragma once

/** @file StreamingFailurePolicy.h
 * @brief Bounded per-cell failure history, retry cooldown and quarantine contract.
 */

#include "Horo/WorldStreaming/StreamingCellOperation.h"

#include <cstdint>
#include <optional>

namespace Horo::WorldStreaming {
    struct StreamingCellStateRecord;

    namespace Detail {
        struct StreamingFailurePolicyIdTag;
        struct StreamingFailurePolicyRevisionTag;
        struct StreamingContentRevisionTag;
        struct StreamingProviderRevisionTag;
    }  // namespace Detail

    /** @brief Stable identity of the authority's retry policy. */
    using StreamingFailurePolicyId =
        Foundation::Detail::NonZeroId64<Detail::StreamingFailurePolicyIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact policy publication revision. */
    using StreamingFailurePolicyRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingFailurePolicyRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact content publication used by an attempt. */
    using StreamingContentRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingContentRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact required-provider publication used by an attempt. */
    using StreamingProviderRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingProviderRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Producer-classified cause; generic error recoverability never grants retries. */
    enum class StreamingFailureCause : std::uint8_t {
        TransientIo,
        TransientProvider,
        Integrity,
        Schema,
        MissingRequiredProvider,
        PermanentlyOversized,
        Count
    };
    /** @brief Authority lifecycle admission gate. */
    enum class StreamingFailureLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count
    };
    /** @brief Explicit host authorization; observing demand is never authorization. */
    enum class StreamingRetryAuthorization : std::uint8_t {
        None,
        Authorized,
        Count
    };
    /** @brief Pure decision; eligibility still requires separate fresh scheduler/budget admission. */
    enum class StreamingRetryDisposition : std::uint8_t {
        CoolingDown,
        Quarantined,
        Eligible,
        AlreadyIssued
    };

    /** @brief Bounded immutable policy construction facts. */
    struct StreamingFailurePolicyRequest final {
        static constexpr std::uint32_t CurrentContractVersion = 1;
        static constexpr std::uint32_t MaximumAutomaticRetries = 16;
        static constexpr std::uint64_t MaximumCooldownMilliseconds = 600'000;
        static constexpr std::uint32_t MaximumTrackedCells = 1'048'576;
        std::uint32_t contractVersion{CurrentContractVersion}; /**< Supported in-memory version. */
        StreamingFailurePolicyId id{};                         /**< Stable authority identity. */
        StreamingFailurePolicyRevision revision{};             /**< Exact policy revision. */
        std::uint32_t automaticRetries{3};                /**< Retries after the initial failed attempt; zero disables automatic retry. */
        std::uint64_t initialCooldownMilliseconds{2'000}; /**< Positive unscaled initial delay. */
        std::uint64_t maximumCooldownMilliseconds{8'000}; /**< Positive exponential-backoff cap. */
        std::uint32_t maximumTrackedCells{1'024};         /**< Failure tombstone ceiling; demand loss never releases a record. */
    };

    /** @brief Inert validated policy; owns no clock, queue, resources or registration. */
    class StreamingFailurePolicy final {
    public:
        /** @brief Validates policy limits. @param request Complete policy facts. @return Policy or typed invalid/unsupported failure. */
        [[nodiscard]] static Result<StreamingFailurePolicy> Create(const StreamingFailurePolicyRequest &request);
        /** @brief Returns immutable policy facts. @return Borrowed facts owned by this value. */
        [[nodiscard]] const StreamingFailurePolicyRequest &Facts() const noexcept;

    private:
        explicit StreamingFailurePolicy(const StreamingFailurePolicyRequest &request) noexcept;
        StreamingFailurePolicyRequest request_;
    };

    /** @brief Exact owner facts supplied at an authority safe point. */
    struct StreamingFailureContext final {
        StreamingFailurePolicyId policy{};               /**< Current authority identity. */
        StreamingFailurePolicyRevision policyRevision{}; /**< Exact policy publication. */
        WorldPartitionId partition{};                    /**< Mounted partition. */
        PartitionEpoch epoch{};                          /**< Mounted incarnation. */
        StreamingContentRevision contentRevision{};      /**< Current content publication. */
        StreamingProviderRevision providerRevision{};    /**< Current required-provider publication. */
        std::uint64_t serviceTimeMilliseconds{};         /**< Unscaled monotonic time. */
        std::uint32_t trackedCells{};                    /**< Authority-owned failure records, including quarantine. */
        StreamingFailureLifecycle lifecycle{StreamingFailureLifecycle::Closed}; /**< Admission lifecycle. */
    };

    /** @brief Immutable diagnostic facts; only StreamingFailureRecord constructs transitions. */
    struct StreamingFailureSnapshot final {
        StreamingFailurePolicyId policy{};               /**< Policy identity. */
        StreamingFailurePolicyRevision policyRevision{}; /**< Policy publication. */
        StreamingCellOperationHandle operation{};        /**< Failed attempt, or exact issued retry. */
        StreamingContentRevision contentRevision{};      /**< Attempt content publication. */
        StreamingProviderRevision providerRevision{};    /**< Attempt provider publication. */
        StreamingFailureCause cause{};                   /**< Last failure cause, retained during requeue. */
        std::uint32_t automaticRetriesIssued{};          /**< Consumed automatic retry allowance. */
        std::uint32_t attemptCount{1};                   /**< Attempts in this revision/authorization series. */
        std::uint64_t observedAtServiceMilliseconds{};   /**< Last state transition time. */
        std::uint64_t nextRetryAtServiceMilliseconds{};  /**< Deadline, or zero for quarantine/issued work. */
        bool retryIssued{};                              /**< True until the exact issued attempt fails; duplicate requeue is forbidden. */
    };

    /**
     * @brief Authority-owned immutable failure tombstone and single-use retry transition.
     * @details StreamingAuthorityRole retains one record per cell across demand loss, cancellation and policy replacement.
     *          Pure calls allocate no storage and mutate no input. Partition teardown discards history only after retirement.
     *          Exact Active residency authorizes releasing the tombstone; cancellation alone never resets history.
     */
    class StreamingFailureRecord final {
    public:
        /**
         * @brief Records a canonical failed terminal after cleanup; duplicate or unrelated completions are stale.
         * @param policy Current policy. @param context Exact owner/time/revision/capacity facts.
         * @param operation Canonical failed terminal, including retirement acknowledgement for admitted work.
         * @param cause Producer's typed failure classification. @param previous Prior cell tombstone, if any.
         * @return New immutable record or typed invalid, stale, capacity, lifecycle or transition failure.
         */
        [[nodiscard]] static Result<StreamingFailureRecord> RecordFailure(const StreamingFailurePolicy &policy,
                                                                          const StreamingFailureContext &context,
                                                                          const StreamingCellOperation &operation,
                                                                          StreamingFailureCause cause,
                                                                          const std::optional<StreamingFailureRecord> &previous);
        /** @brief Returns diagnostic facts. @return Borrowed immutable snapshot. */
        [[nodiscard]] const StreamingFailureSnapshot &Snapshot() const noexcept;
        /**
         * @brief Observes eligibility without consuming allowance or performing admission.
         * @param policy Current policy. @param context Current owner facts.
         * @param authorization Explicit host authorization to reset the attempt series.
         * @return Cooldown, quarantine, eligibility or already-issued decision; otherwise typed failure.
         */
        [[nodiscard]] Result<StreamingRetryDisposition> EvaluateRetry(
            const StreamingFailurePolicy &policy, const StreamingFailureContext &context,
            StreamingRetryAuthorization authorization = StreamingRetryAuthorization::None) const;
        /**
         * @brief Consumes one eligibility decision into an exact async requeue; never sleeps or reserves resources.
         * @param policy Current policy. @param context Current owner facts.
         * @param retry Fresh queued Load operation with a greater cell generation and distinct operation identity.
         * @param authorization Explicit host authorization; revision changes also start a fresh series.
         * @return Successor tombstone or typed failure without changing this record.
         * @pre Authority applies the successor and requeues atomically; ordinary scheduler/budget admission follows before work starts.
         */
        [[nodiscard]] Result<StreamingFailureRecord> IssueRetry(
            const StreamingFailurePolicy &policy, const StreamingFailureContext &context, const StreamingCellOperation &retry,
            StreamingRetryAuthorization authorization = StreamingRetryAuthorization::None) const;
        /**
         * @brief Reconciles an exact cancelled/replaced/shutdown retry terminal after cleanup, retaining its consumed allowance.
         * @param policy Current policy. @param context Current active authority facts after cancellation resumes.
         * @param operation Exact interrupted terminal of the issued retry.
         * @return Successor cooldown/quarantine record, or typed stale/lifecycle/transition/time failure.
         * @details Closed authorities retain history while draining; reconciliation never grants new allowance.
         */
        [[nodiscard]] Result<StreamingFailureRecord> ReconcileInterruption(const StreamingFailurePolicy &policy,
                                                                           const StreamingFailureContext &context,
                                                                           const StreamingCellOperation &operation) const;
        /**
         * @brief Validates release after the exact issued generation reaches Active residency; the authority may then erase this history.
         * @param policy Current policy. @param context Current owner facts.
         * @param active Canonical Active residency record for the exact issued generation.
         * @return Success authorizing history release, or typed stale/lifecycle/transition failure.
         */
        [[nodiscard]] Result<void> ValidateSuccess(const StreamingFailurePolicy &policy, const StreamingFailureContext &context,
                                                   const StreamingCellStateRecord &active) const;

    private:
        explicit StreamingFailureRecord(const StreamingFailureSnapshot &snapshot) noexcept;
        StreamingFailureSnapshot snapshot_;
    };
}  // namespace Horo::WorldStreaming
