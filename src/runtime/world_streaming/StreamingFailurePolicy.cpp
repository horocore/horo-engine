#include "Horo/WorldStreaming/StreamingFailurePolicy.h"

#include "Horo/WorldStreaming/StreamingCellState.h"

#include <limits>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Returns a typed failure without modifying policy or history. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Validates current safe-point evidence against an inert policy publication. */
        Result<void> ValidateContext(const StreamingFailurePolicy &policy, const StreamingFailureContext &context) {
            if (context.lifecycle >= StreamingFailureLifecycle::Count)
                return Failure<void>(WorldStreamingErrors::FailurePolicyUnsupported);
            if (!context.policy.IsValid() || !context.policyRevision.IsValid() || !context.partition.IsValid() ||
                !context.epoch.IsValid() || !context.contentRevision.IsValid() || !context.providerRevision.IsValid())
                return Failure<void>(WorldStreamingErrors::FailurePolicyInvalid);
            if (context.policy != policy.Facts().id || context.policyRevision != policy.Facts().revision)
                return Failure<void>(WorldStreamingErrors::FailurePolicyStale);
            if (context.lifecycle != StreamingFailureLifecycle::Active)
                return Failure<void>(WorldStreamingErrors::FailurePolicyLifecycleUnavailable);
            return Result<void>::Success();
        }

        /** @brief Checks retained authority, mounted incarnation and non-decreasing service time. */
        bool Matches(const StreamingFailureSnapshot &snapshot, const StreamingFailureContext &context) noexcept {
            return snapshot.policy == context.policy && snapshot.policyRevision == context.policyRevision &&
                   snapshot.operation.fence.partition == context.partition && snapshot.operation.fence.epoch == context.epoch &&
                   snapshot.observedAtServiceMilliseconds <= context.serviceTimeMilliseconds;
        }

        /** @brief Checks whether an explicitly newer producer publication authorizes a fresh attempt series. */
        bool RevisionChanged(const StreamingFailureSnapshot &snapshot, const StreamingFailureContext &context) noexcept {
            return snapshot.contentRevision != context.contentRevision || snapshot.providerRevision != context.providerRevision;
        }

        /** @brief Only I/O and provider transients consume automatic retry allowance. */
        bool IsTransient(const StreamingFailureCause cause) noexcept {
            return cause == StreamingFailureCause::TransientIo || cause == StreamingFailureCause::TransientProvider;
        }

        /** @brief Load completion must match the issued operation; its activation stage shares the exact attempt fence. */
        bool MatchesIssuedOperation(const StreamingFailureSnapshot &snapshot, const StreamingCellOperation &operation) noexcept {
            return snapshot.operation.fence == operation.Handle().fence &&
                   (operation.Kind() == StreamingCellOperationKind::Activate || snapshot.operation == operation.Handle());
        }

        /** @brief Computes capped exponential delay with bounded iterations and no overflowing multiplication. */
        std::uint64_t Cooldown(const StreamingFailurePolicyRequest &policy, const std::uint32_t issued) noexcept {
            std::uint64_t delay = policy.initialCooldownMilliseconds;
            for (std::uint32_t index = 0; index < issued; ++index)
                delay = delay >= policy.maximumCooldownMilliseconds - delay ? policy.maximumCooldownMilliseconds : delay * 2;
            return delay;
        }

        /** @brief Stores the next bounded deadline or quarantine sentinel without overflow. */
        Result<void> SetCooldown(const StreamingFailurePolicyRequest &policy, const StreamingFailureContext &context,
                                 StreamingFailureSnapshot &snapshot) {
            snapshot.nextRetryAtServiceMilliseconds = 0;
            if (!IsTransient(snapshot.cause) || snapshot.automaticRetriesIssued >= policy.automaticRetries)
                return Result<void>::Success();
            const auto delay = Cooldown(policy, snapshot.automaticRetriesIssued);
            if (delay > std::numeric_limits<std::uint64_t>::max() - context.serviceTimeMilliseconds)
                return Failure<void>(WorldStreamingErrors::FailurePolicyTimeExhausted);
            snapshot.nextRetryAtServiceMilliseconds = context.serviceTimeMilliseconds + delay;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc StreamingFailurePolicy::Create */
    Result<StreamingFailurePolicy> StreamingFailurePolicy::Create(const StreamingFailurePolicyRequest &request) {
        if (request.contractVersion != StreamingFailurePolicyRequest::CurrentContractVersion)
            return Failure<StreamingFailurePolicy>(WorldStreamingErrors::FailurePolicyUnsupported);
        if (!request.id.IsValid() || !request.revision.IsValid() ||
            request.automaticRetries > StreamingFailurePolicyRequest::MaximumAutomaticRetries || request.initialCooldownMilliseconds == 0 ||
            request.initialCooldownMilliseconds > request.maximumCooldownMilliseconds ||
            request.maximumCooldownMilliseconds > StreamingFailurePolicyRequest::MaximumCooldownMilliseconds ||
            request.maximumTrackedCells == 0 || request.maximumTrackedCells > StreamingFailurePolicyRequest::MaximumTrackedCells)
            return Failure<StreamingFailurePolicy>(WorldStreamingErrors::FailurePolicyInvalid);
        return Result<StreamingFailurePolicy>::Success(StreamingFailurePolicy{request});
    }

    StreamingFailurePolicy::StreamingFailurePolicy(const StreamingFailurePolicyRequest &request) noexcept : request_(request) {}

    /** @copydoc StreamingFailurePolicy::Facts */
    const StreamingFailurePolicyRequest &StreamingFailurePolicy::Facts() const noexcept {
        return request_;
    }

    StreamingFailureRecord::StreamingFailureRecord(const StreamingFailureSnapshot &snapshot) noexcept : snapshot_(snapshot) {}

    /** @copydoc StreamingFailureRecord::Snapshot */
    const StreamingFailureSnapshot &StreamingFailureRecord::Snapshot() const noexcept {
        return snapshot_;
    }

    /** @copydoc StreamingFailureRecord::RecordFailure */
    Result<StreamingFailureRecord> StreamingFailureRecord::RecordFailure(const StreamingFailurePolicy &policy,
                                                                         const StreamingFailureContext &context,
                                                                         const StreamingCellOperation &operation,
                                                                         const StreamingFailureCause cause,
                                                                         const std::optional<StreamingFailureRecord> &previous) {
        const auto valid = ValidateContext(policy, context);
        if (!valid.HasValue())
            return Result<StreamingFailureRecord>::Failure(valid.ErrorValue());
        if (cause >= StreamingFailureCause::Count)
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyUnsupported);
        if (operation.Handle().fence.partition != context.partition || operation.Handle().fence.epoch != context.epoch)
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyStale);
        if (!operation.IsTerminal() || operation.Outcome() != StreamingCellOperationOutcome::Failed ||
            operation.Kind() == StreamingCellOperationKind::Retire)
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyTransitionInvalid);
        if (!previous && context.trackedCells >= policy.Facts().maximumTrackedCells)
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyCapacityExceeded);
        if (previous && (!Matches(previous->snapshot_, context) || !previous->snapshot_.retryIssued ||
                         !MatchesIssuedOperation(previous->snapshot_, operation) || RevisionChanged(previous->snapshot_, context)))
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyStale);

        StreamingFailureSnapshot next{context.policy,          context.policyRevision,   operation.Handle(),
                                      context.contentRevision, context.providerRevision, cause};
        if (previous) {
            next.automaticRetriesIssued = previous->snapshot_.automaticRetriesIssued;
            next.attemptCount = previous->snapshot_.attemptCount;
        }
        next.observedAtServiceMilliseconds = context.serviceTimeMilliseconds;
        const auto cooldown = SetCooldown(policy.Facts(), context, next);
        if (!cooldown.HasValue())
            return Result<StreamingFailureRecord>::Failure(cooldown.ErrorValue());
        return Result<StreamingFailureRecord>::Success(StreamingFailureRecord{next});
    }

    /** @copydoc StreamingFailureRecord::EvaluateRetry */
    Result<StreamingRetryDisposition> StreamingFailureRecord::EvaluateRetry(const StreamingFailurePolicy &policy,
                                                                            const StreamingFailureContext &context,
                                                                            const StreamingRetryAuthorization authorization) const {
        const auto valid = ValidateContext(policy, context);
        if (!valid.HasValue())
            return Result<StreamingRetryDisposition>::Failure(valid.ErrorValue());
        if (authorization >= StreamingRetryAuthorization::Count)
            return Failure<StreamingRetryDisposition>(WorldStreamingErrors::FailurePolicyUnsupported);
        if (!Matches(snapshot_, context) || context.contentRevision.Value() < snapshot_.contentRevision.Value() ||
            context.providerRevision.Value() < snapshot_.providerRevision.Value())
            return Failure<StreamingRetryDisposition>(WorldStreamingErrors::FailurePolicyStale);
        if (snapshot_.retryIssued)
            return Result<StreamingRetryDisposition>::Success(StreamingRetryDisposition::AlreadyIssued);
        if (authorization == StreamingRetryAuthorization::Authorized || RevisionChanged(snapshot_, context))
            return Result<StreamingRetryDisposition>::Success(StreamingRetryDisposition::Eligible);
        if (snapshot_.nextRetryAtServiceMilliseconds == 0)
            return Result<StreamingRetryDisposition>::Success(StreamingRetryDisposition::Quarantined);
        return Result<StreamingRetryDisposition>::Success(context.serviceTimeMilliseconds < snapshot_.nextRetryAtServiceMilliseconds
                                                              ? StreamingRetryDisposition::CoolingDown
                                                              : StreamingRetryDisposition::Eligible);
    }

    /** @copydoc StreamingFailureRecord::IssueRetry */
    Result<StreamingFailureRecord> StreamingFailureRecord::IssueRetry(const StreamingFailurePolicy &policy,
                                                                      const StreamingFailureContext &context,
                                                                      const StreamingCellOperation &retry,
                                                                      const StreamingRetryAuthorization authorization) const {
        const auto eligibility = EvaluateRetry(policy, context, authorization);
        if (!eligibility.HasValue())
            return Result<StreamingFailureRecord>::Failure(eligibility.ErrorValue());
        if (eligibility.Value() != StreamingRetryDisposition::Eligible || retry.State() != StreamingCellOperationState::Queued ||
            retry.Kind() != StreamingCellOperationKind::Load)
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyTransitionInvalid);
        const auto &fresh = retry.Handle();
        const auto &old = snapshot_.operation;
        if (fresh.operation == old.operation || fresh.fence.partition != old.fence.partition || fresh.fence.epoch != old.fence.epoch ||
            fresh.fence.cell != old.fence.cell || fresh.fence.generation.Value() <= old.fence.generation.Value())
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyStale);
        auto next = snapshot_;
        const bool reset = authorization == StreamingRetryAuthorization::Authorized || RevisionChanged(snapshot_, context);
        next.automaticRetriesIssued = reset ? 0 : next.automaticRetriesIssued + 1;
        next.attemptCount = reset ? 1 : next.attemptCount + 1;
        next.contentRevision = context.contentRevision;
        next.providerRevision = context.providerRevision;
        next.operation = fresh;
        next.observedAtServiceMilliseconds = context.serviceTimeMilliseconds;
        next.nextRetryAtServiceMilliseconds = 0;
        next.retryIssued = true;
        return Result<StreamingFailureRecord>::Success(StreamingFailureRecord{next});
    }

    /** @copydoc StreamingFailureRecord::ReconcileInterruption */
    Result<StreamingFailureRecord> StreamingFailureRecord::ReconcileInterruption(const StreamingFailurePolicy &policy,
                                                                                 const StreamingFailureContext &context,
                                                                                 const StreamingCellOperation &operation) const {
        const auto valid = ValidateContext(policy, context);
        if (!valid.HasValue())
            return Result<StreamingFailureRecord>::Failure(valid.ErrorValue());
        if (!Matches(snapshot_, context) || !MatchesIssuedOperation(snapshot_, operation) || RevisionChanged(snapshot_, context))
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyStale);
        const auto outcome = operation.Outcome();
        if (!snapshot_.retryIssued || !operation.IsTerminal() ||
            (outcome != StreamingCellOperationOutcome::Cancelled && outcome != StreamingCellOperationOutcome::Replaced &&
             outcome != StreamingCellOperationOutcome::Shutdown))
            return Failure<StreamingFailureRecord>(WorldStreamingErrors::FailurePolicyTransitionInvalid);
        auto next = snapshot_;
        next.retryIssued = false;
        next.observedAtServiceMilliseconds = context.serviceTimeMilliseconds;
        const auto cooldown = SetCooldown(policy.Facts(), context, next);
        if (!cooldown.HasValue())
            return Result<StreamingFailureRecord>::Failure(cooldown.ErrorValue());
        return Result<StreamingFailureRecord>::Success(StreamingFailureRecord{next});
    }

    /** @copydoc StreamingFailureRecord::ValidateSuccess */
    Result<void> StreamingFailureRecord::ValidateSuccess(const StreamingFailurePolicy &policy, const StreamingFailureContext &context,
                                                         const StreamingCellStateRecord &active) const {
        const auto valid = ValidateContext(policy, context);
        if (!valid.HasValue())
            return valid;
        if (!active.IsValid())
            return Failure<void>(WorldStreamingErrors::FailurePolicyInvalid);
        if (!Matches(snapshot_, context) || active.operation.fence != snapshot_.operation.fence || RevisionChanged(snapshot_, context))
            return Failure<void>(WorldStreamingErrors::FailurePolicyStale);
        if (!snapshot_.retryIssued || active.state != StreamingCellState::Active)
            return Failure<void>(WorldStreamingErrors::FailurePolicyTransitionInvalid);
        return Result<void>::Success();
    }

}  // namespace Horo::WorldStreaming
