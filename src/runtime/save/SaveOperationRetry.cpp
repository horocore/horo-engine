#include "SaveOperationArbiterState.h"

#include <limits>
#include <utility>

namespace Horo::Runtime {
    using namespace SaveOperationArbiterDetail;

    namespace {
        /** @brief Checks finite bounds independently of mutable operation evidence. */
        [[nodiscard]] bool IsRetryPolicyValid(const SaveArbiterRetryPolicy &policy) noexcept {
            return policy.maximumRetries <= MaximumSaveStorageAutomaticRetries && policy.initialBackoffMilliseconds != 0 &&
                   policy.maximumBackoffMilliseconds >= policy.initialBackoffMilliseconds && policy.maximumElapsedMilliseconds != 0;
        }

        /** @brief Checks the immutable archive and storage CAS/version evidence. */
        [[nodiscard]] bool IsPublicationEvidenceValid(const SaveArbiterRetryPreconditions &facts) noexcept {
            return facts.catalogRevision != 0 && facts.compatibilityRevision != 0 && facts.publicationGeneration.IsValid() &&
                   (!facts.expectedGeneration ||
                    (facts.expectedGeneration->IsValid() && *facts.expectedGeneration != facts.publicationGeneration));
        }

        /** @brief Validates exact binding and publication generations before any worker is admitted. */
        [[nodiscard]] bool IsRetryPreconditionsValid(const SaveArbiterRetryPreconditions &facts,
                                                     const SaveArbiterAddress &address) noexcept {
            return facts.access.expected == address.nameSpace && facts.access.expectedRevision != 0 &&
                   facts.bindingState == SaveNamespaceBindingState::Available && facts.runtime.IsValid() && facts.authorized &&
                   facts.slot.IsValid() && facts.slot == address.slot && IsPublicationEvidenceValid(facts);
        }

        /** @brief Checks the operation's monotonic supplied clock without changing its admitted baseline. */
        [[nodiscard]] bool IsRetryClockValid(const State &state, const State::Record &record, const std::uint64_t clock) noexcept {
            return (!state.retryClock || clock >= *state.retryClock) &&
                   (!record.request.retry || clock >= record.request.retry->admittedAtMilliseconds);
        }

        /** @brief Accepts failure evidence only in the original Save phase; uncertain publication requires its entered gate. */
        [[nodiscard]] bool IsStorageFailureStateValid(const State::Record &record, const SaveStorageFailureInput &failure) noexcept {
            return record.request.operation.kind == SaveOperationKind::Save &&
                   (record.state == SaveArbiterState::Encoding || record.state == SaveArbiterState::Committing) &&
                   (failure.commitOutcome == SaveOperationCommitOutcome::NotCommitted ||
                    (failure.commitOutcome == SaveOperationCommitOutcome::Unknown && record.state == SaveArbiterState::Committing));
        }

        /** @brief Finds only an active Save whose typed failure and supplied clock preserve the original operation contract. */
        [[nodiscard]] RecordIterator FindStorageFailureRecord(State &state, const OperationId operation,
                                                              const SaveStorageFailureInput &failure, const std::uint64_t clock) {
            const auto found = FindActiveRecord(state, operation);
            if (found == state.records.end() || !IsStorageFailureStateValid(*found, failure) || !IsRetryClockValid(state, *found, clock))
                return state.records.end();
            return found;
        }

        /** @brief Prevents a conflicting transient label from overriding an already classified portable cause. */
        [[nodiscard]] bool ConflictsWithTransientCategory(const SaveStorageFailureInput &failure) noexcept {
            return failure.category == SaveStorageFailureCategory::TransientIo &&
                   failure.nativeCause.domain.Value() == SaveErrors::StorageTransientIo.domain.Value() &&
                   failure.nativeCause.code.Value() != SaveErrors::StorageTransientIo.code.Value();
        }

        /** @brief Authorizes backoff only before the original commit gate with its admitted finite capability. */
        [[nodiscard]] bool CanDefer(const State::Record &record, const SaveStorageFailureDecision &decision) noexcept {
            return record.request.retry && record.state == SaveArbiterState::Encoding && decision.CanRetryAutomatically();
        }

        /** @brief Respects due time and existing active or queued work without preemption. */
        [[nodiscard]] bool ReadyToResume(const State &state, const State::Record &record, const std::uint64_t clock,
                                         const std::size_t queued) noexcept {
            return !state.closed && clock >= record.retry.eligibleAtMilliseconds && !state.active && queued == 0;
        }

        /** @brief Publishes one original failure without introducing another operation or resetting its terminal receipt. */
        [[nodiscard]] Result<bool> FinishRetryFailure(State &state, State::Record &record, Error error,
                                                      const SaveOperationCommitOutcome outcome,
                                                      const std::chrono::steady_clock::time_point now) {
            const auto finished = FinishTerminalTransition(state, record, record.controller.Fail(std::move(error), outcome, now));
            if (finished.HasError())
                return Result<bool>::Failure(finished.ErrorValue());
            return Result<bool>::Success(false);
        }

        /** @brief Calculates bounded exponential delay without wrapping the host clock or elapsed budget. */
        [[nodiscard]] std::optional<std::uint64_t> RetryEligibleAt(const State::Record &record, const std::uint64_t clock) noexcept {
            const auto &descriptor = *record.request.retry;
            const auto &policy = descriptor.policy;
            const auto elapsed = clock - descriptor.admittedAtMilliseconds;
            std::uint64_t backoff = policy.initialBackoffMilliseconds;
            for (std::uint8_t index = 0; index < record.retry.completedRetries; ++index)
                backoff = backoff > policy.maximumBackoffMilliseconds / 2 ? policy.maximumBackoffMilliseconds : backoff * 2;
            if (elapsed >= policy.maximumElapsedMilliseconds || backoff > policy.maximumElapsedMilliseconds - elapsed ||
                backoff > std::numeric_limits<std::uint64_t>::max() - clock)
                return std::nullopt;
            return clock + backoff;
        }

    }  // namespace

    namespace SaveOperationArbiterDetail {
        /** @copydoc IsRetryValid */
        [[nodiscard]] bool IsRetryValid(const SaveArbiterRequest &request) noexcept {
            if (!request.retry)
                return true;
            return request.operation.kind == SaveOperationKind::Save && request.address && IsRetryPolicyValid(request.retry->policy) &&
                   IsRetryPreconditionsValid(request.retry->preconditions, *request.address);
        }

    }  // namespace SaveOperationArbiterDetail

    /** @copydoc SaveOperationArbiter::DeferStorageRetry */
    Result<bool> SaveOperationArbiter::DeferStorageRetry(const OperationId operation, SaveStorageFailureInput failure,
                                                         const std::uint64_t monotonicMilliseconds,
                                                         const std::chrono::steady_clock::time_point now) {
        const auto found = FindStorageFailureRecord(*state_, operation, failure, monotonicMilliseconds);
        if (found == state_->records.end())
            return Result<bool>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        const auto outcome = failure.commitOutcome;
        if (ConflictsWithTransientCategory(failure)) {
            const auto finished = FinishRetryFailure(*state_, *found, std::move(failure.nativeCause), outcome, now);
            if (finished.HasValue())
                state_->retryClock = monotonicMilliseconds;
            return finished;
        }
        failure.completedAutomaticRetries = found->retry.completedRetries;
        failure.maximumAutomaticRetries = found->request.retry ? found->request.retry->policy.maximumRetries : 0;
        const auto decision = MakeSaveStorageFailureDecision(std::move(failure));
        if (decision.HasError())
            return Result<bool>::Failure(decision.ErrorValue());
        state_->retryClock = monotonicMilliseconds;
        static_cast<void>(found->controller.ObserveCancellation(now));
        if (SynchronizeTerminal(*state_, *found))
            return Result<bool>::Success(false);
        found->retry.lastError = decision.Value().ErrorValue();
        if (!CanDefer(*found, decision.Value()))
            return FinishRetryFailure(*state_, *found, decision.Value().ErrorValue(), outcome, now);
        const auto eligible = RetryEligibleAt(*found, monotonicMilliseconds);
        if (!eligible)
            return FinishRetryFailure(*state_, *found, decision.Value().ErrorValue(), outcome, now);
        found->retry.eligibleAtMilliseconds = *eligible;
        found->state = SaveArbiterState::WaitingForRetry;
        ++found->revision;
        state_->active.reset();
        return Result<bool>::Success(true);
    }

    /** @copydoc SaveOperationArbiter::ResumeStorageRetry */
    Result<std::optional<SaveArbiterSnapshot>> SaveOperationArbiter::ResumeStorageRetry(const OperationId operation,
                                                                                        const SaveArbiterRetryPreconditions &current,
                                                                                        const std::uint64_t monotonicMilliseconds,
                                                                                        const std::chrono::steady_clock::time_point now) {
        using Return = Result<std::optional<SaveArbiterSnapshot>>;
        const auto found = Find(*state_, operation);
        if (found == state_->records.end() || !found->request.retry || !IsRetryClockValid(*state_, *found, monotonicMilliseconds))
            return Return::Failure(MakeError(SaveErrors::ArbiterInvalid));
        if (IsTerminal(*found))
            return Return::Success({});
        if (found->state != SaveArbiterState::WaitingForRetry)
            return Return::Failure(MakeError(SaveErrors::ArbiterInvalid));
        state_->retryClock = monotonicMilliseconds;
        static_cast<void>(found->controller.ObserveCancellation(now));
        if (SynchronizeTerminal(*state_, *found))
            return Return::Success({});
        const auto &descriptor = *found->request.retry;
        const auto terminate = [&](Error error) {
            static_cast<void>(found->controller.Fail(std::move(error), SaveOperationCommitOutcome::NotCommitted, now));
            static_cast<void>(SynchronizeTerminal(*state_, *found));
        };
        if (current != descriptor.preconditions) {
            terminate(MakeError(SaveErrors::GenerationStale));
            return Return::Success({});
        }
        if (monotonicMilliseconds - descriptor.admittedAtMilliseconds >= descriptor.policy.maximumElapsedMilliseconds) {
            terminate(*found->retry.lastError);
            return Return::Success({});
        }
        if (!ReadyToResume(*state_, *found, monotonicMilliseconds, QueuedCount()))
            return Return::Success({});
        ++found->retry.completedRetries;
        found->state = SaveArbiterState::Encoding;
        ++found->revision;
        state_->active = operation;
        return Return::Success(CopySnapshot(*found));
    }

}  // namespace Horo::Runtime
