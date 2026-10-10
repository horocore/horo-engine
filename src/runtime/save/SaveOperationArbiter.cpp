#include "Horo/Runtime/Save/SaveOperationArbiter.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"
#include "SaveOperationArbiterState.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    using namespace SaveOperationArbiterDetail;

    namespace {
        [[nodiscard]] Error ArbiterError(const ErrorCodeDescriptor &descriptor, const char *message) {
            return MakeError(descriptor, message);
        }

        [[nodiscard]] bool IsKnown(const SaveArbiterPriority value) noexcept {
            return value >= SaveArbiterPriority::Background && value <= SaveArbiterPriority::UserBlocking;
        }

        [[nodiscard]] bool IsKnown(const SaveArbiterConflictPolicy value) noexcept {
            return value >= SaveArbiterConflictPolicy::Reject && value <= SaveArbiterConflictPolicy::ReplaceQueuedEquivalent;
        }

        [[nodiscard]] bool IsKnown(const SavePolicyMode value) noexcept {
            return value >= SavePolicyMode::Manual && value < SavePolicyMode::Count;
        }

        [[nodiscard]] bool IsKnown(const SaveOperationKind value) noexcept {
            return value >= SaveOperationKind::Save && value <= SaveOperationKind::Delete;
        }

        [[nodiscard]] bool RequiresAddress(const SaveOperationKind kind) noexcept {
            return kind != SaveOperationKind::RefreshCatalog;
        }

        [[nodiscard]] bool IsAddressValid(const std::optional<SaveArbiterAddress> &address) noexcept {
            return address.has_value() && address->nameSpace.IsValid() && address->slot.IsValid();
        }

        [[nodiscard]] bool IsRequestValid(const SaveArbiterRequest &request) noexcept {
            if (request.operation.operation == 0 || request.operation.maximumCompletionCallbacks == 0 ||
                request.operation.maximumCompletionCallbacks > MaximumSaveOperationCompletionCallbacks || !IsRetryValid(request) ||
                !IsKnown(request.operation.kind) || !IsKnown(request.mode) || !IsKnown(request.priority) || !IsKnown(request.conflict))
                return false;
            return RequiresAddress(request.operation.kind) ? IsAddressValid(request.address) : !request.address.has_value();
        }

        [[nodiscard]] bool IsAllowedSaveState(const SaveArbiterState current, const SaveArbiterState next) noexcept {
            using enum SaveArbiterState;
            switch (current) {
                case Queued:
                    return next == WaitingForSafePoint;
                case WaitingForSafePoint:
                    return next == Capturing;
                case Capturing:
                    return next == Encoding;
                case Encoding:
                case Committing:
                    return next == Committing;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool IsAllowedLoadState(const SaveArbiterState current, const SaveArbiterState next) noexcept {
            using enum SaveArbiterState;
            switch (current) {
                case Queued:
                    return next == Loading;
                case Loading:
                    return next == WaitingForSafePoint;
                case WaitingForSafePoint:
                case Activating:
                    return next == Activating;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool IsAllowedState(const SaveOperationKind kind, const SaveArbiterState current,
                                          const SaveArbiterState next) noexcept {
            using enum SaveArbiterState;
            using enum SaveOperationKind;
            switch (kind) {
                case Save:
                    return IsAllowedSaveState(current, next);
                case Load:
                    return IsAllowedLoadState(current, next);
                case Delete:
                    return next == Committing && (current == Queued || current == Committing);
                case RefreshCatalog:
                    return next == Loading && (current == Queued || current == Loading);
            }
            return false;
        }

        [[nodiscard]] SaveOperationStage ProgressStage(const SaveOperationKind kind, const SaveArbiterState state) noexcept {
            using enum SaveOperationKind;
            using enum SaveOperationStage;
            if (state == SaveArbiterState::Capturing)
                return CapturingSnapshot;
            if (state == SaveArbiterState::Encoding)
                return Serializing;
            if (state == SaveArbiterState::Loading)
                return kind == RefreshCatalog ? RefreshingCatalog : VerifyingArchive;
            if (state == SaveArbiterState::Activating)
                return ApplyingState;
            return CommitStarted;
        }
    }  // namespace

    namespace {
        using State = SaveOperationArbiterDetail::State;

        [[nodiscard]] bool Equivalent(const State::Record &record, const SaveArbiterRequest &request) noexcept {
            return record.request.operation.kind == request.operation.kind && record.request.mode == request.mode &&
                   record.request.address == request.address && record.request.retry.has_value() == request.retry.has_value() &&
                   (!request.retry || (record.request.retry->policy == request.retry->policy &&
                                       record.request.retry->preconditions == request.retry->preconditions));
        }

        [[nodiscard]] bool Conflicts(const State::Record &record, const SaveArbiterRequest &request) {
            if (IsTerminal(record))
                return false;
            if (!record.request.address.has_value() || !request.address.has_value())
                return true;
            return record.request.address == request.address;
        }

        void CancelRecord(State &state, State::Record &record) {
            const SaveOperationHandle handle = record.controller.Handle();
            const SaveCancellationRequestResult requested = handle.RequestCancellation();
            if (requested == SaveCancellationRequestResult::Requested || requested == SaveCancellationRequestResult::AlreadyRequested) {
                record.state = SaveArbiterState::Cancelling;
                ++record.revision;
                static_cast<void>(record.controller.ObserveCancellation());
                static_cast<void>(SynchronizeTerminal(state, record));
            }
        }

        [[nodiscard]] Result<void> PublishCommitProgress(State::Record &record, const SaveOperationProgress progress) {
            using enum SaveOperationStage;
            using enum SaveOperationTransitionResult;
            const SaveOperationKind kind = record.request.operation.kind;
            const SaveOperationStage ready = kind == SaveOperationKind::Delete ? Deleting : WritingTemporary;
            if (const SaveOperationTransitionResult readyResult = record.controller.PublishProgress(ready, {1, 1}); readyResult != Applied)
                return Result<void>::Failure(
                    ArbiterError(SaveErrors::ArbiterInvalid, "The operation could not reach its commit-ready stage."));
            if (record.controller.BeginCommit() != SaveCommitGateResult::Entered)
                return Result<void>::Failure(
                    ArbiterError(SaveErrors::ArbiterInvalid, "The operation commit gate rejected the transition."));
            if (const SaveOperationTransitionResult commit =
                    record.controller.PublishProgress(ProgressStage(kind, SaveArbiterState::Committing), progress);
                commit != Applied)
                return Result<void>::Failure(ArbiterError(SaveErrors::ArbiterInvalid, "The operation rejected commit progress."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> PublishActivationProgress(State::Record &record, const SaveOperationProgress progress) {
            using enum SaveOperationStage;
            using enum SaveOperationTransitionResult;
            if (record.controller.PublishProgress(PreparingRestore, {1, 1}) != Applied ||
                record.controller.PublishProgress(ReadyToCommit, {1, 1}) != Applied ||
                record.controller.BeginCommit() != SaveCommitGateResult::Entered ||
                record.controller.PublishProgress(ApplyingState, progress) != Applied)
                return Result<void>::Failure(ArbiterError(SaveErrors::ArbiterInvalid, "The load operation rejected activation progress."));
            return Result<void>::Success();
        }

        using RecordIterator = std::vector<State::Record>::iterator;

        [[nodiscard]] RecordIterator FindConflict(State &state, const SaveArbiterRequest &request) {
            if (state.active.has_value()) {
                const RecordIterator active = Find(state, *state.active);
                if (active != state.records.end() && Conflicts(*active, request))
                    return active;
            }
            return std::ranges::find_if(state.records, [&](const State::Record &record) {
                return Conflicts(record, request);
            });
        }

        [[nodiscard]] RecordIterator FindQueuedEquivalent(State &state, const SaveArbiterRequest &request) {
            return std::ranges::find_if(state.records, [&](const State::Record &record) {
                return record.state == SaveArbiterState::Queued && Equivalent(record, request) && !IsTerminal(record);
            });
        }

        [[nodiscard]] RecordIterator FindQueuedEquivalentForPolicy(State &state, const SaveArbiterRequest &request,
                                                                   const SaveArbiterConflictPolicy policy) {
            return request.conflict == policy ? FindQueuedEquivalent(state, request) : state.records.end();
        }

        [[nodiscard]] std::optional<Result<SaveArbiterAdmission>> ResolveExistingConflict(State &state, const SaveArbiterRequest &request) {
            if (request.conflict == SaveArbiterConflictPolicy::Reject) {
                const RecordIterator conflict = FindConflict(state, request);
                if (conflict == state.records.end())
                    return std::nullopt;
                return Result<SaveArbiterAdmission>::Failure(
                    MakeError(SaveErrors::OperationInProgress, std::format("The requested slot or session conflicts with operation {}.",
                                                                           conflict->request.operation.operation)));
            }
            const RecordIterator equivalent = FindQueuedEquivalentForPolicy(state, request, SaveArbiterConflictPolicy::CoalesceEquivalent);
            if (equivalent == state.records.end())
                return std::nullopt;
            return Result<SaveArbiterAdmission>::Success(
                {.handle = equivalent->controller.Handle(), .disposition = SaveArbiterAdmissionDisposition::Coalesced});
        }

        [[nodiscard]] std::optional<OperationId> ReplaceQueuedEquivalent(State &state, const SaveArbiterRequest &request) {
            const RecordIterator equivalent =
                FindQueuedEquivalentForPolicy(state, request, SaveArbiterConflictPolicy::ReplaceQueuedEquivalent);
            if (equivalent == state.records.end())
                return std::nullopt;
            const OperationId replaced = equivalent->request.operation.operation;
            CancelRecord(state, *equivalent);
            return replaced;
        }

        [[nodiscard]] Result<SaveArbiterAdmission> AdmitNewRecord(State &state, SaveArbiterRequest request,
                                                                  const std::optional<OperationId> replaced) {
            if (state.records.size() >= state.limits.maximumRetainedOperations)
                return Result<SaveArbiterAdmission>::Failure(MakeError(SaveErrors::ArbiterCapacityExceeded));
            auto operation = CreateSaveOperation(request.operation);
            if (operation.HasError())
                return Result<SaveArbiterAdmission>::Failure(operation.ErrorValue());
            const std::uint64_t enqueueOrder = state.nextEnqueueOrder++;
            state.records.push_back(
                State::Record{.request = std::move(request), .controller = std::move(operation).Value(), .enqueueOrder = enqueueOrder});
            const State::Record &accepted = state.records.back();
            return Result<SaveArbiterAdmission>::Success({.handle = accepted.controller.Handle(),
                                                          .disposition = replaced.has_value()
                                                                             ? SaveArbiterAdmissionDisposition::ReplacedQueued
                                                                             : SaveArbiterAdmissionDisposition::Accepted,
                                                          .replacedOperation = replaced});
        }

        [[nodiscard]] Result<void> PublishArbiterProgress(State::Record &record, const SaveArbiterState next,
                                                          const SaveOperationProgress progress) {
            using enum SaveArbiterState;
            if (next == Committing && record.state != Committing)
                return PublishCommitProgress(record, progress);
            if (next == Activating && record.state != Activating)
                return PublishActivationProgress(record, progress);
            if (next == WaitingForSafePoint)
                return Result<void>::Success();
            if (record.controller.PublishProgress(ProgressStage(record.request.operation.kind, next), progress) ==
                SaveOperationTransitionResult::Applied)
                return Result<void>::Success();
            return Result<void>::Failure(ArbiterError(SaveErrors::ArbiterInvalid, "The operation rejected phase progress."));
        }

        /** @brief Compares stable admission order within typed priority without preempting active work. */
        [[nodiscard]] bool Precedes(const State::Record &candidate, const State::Record &selected) noexcept {
            return candidate.request.priority > selected.request.priority ||
                   (candidate.request.priority == selected.request.priority && candidate.enqueueOrder < selected.enqueueOrder);
        }

        [[nodiscard]] bool IsReadyToComplete(const State::Record &record) noexcept {
            using enum SaveArbiterState;
            using enum SaveOperationKind;
            switch (record.request.operation.kind) {
                case RefreshCatalog:
                    return record.state == Loading;
                case Load:
                    return record.state == Activating;
                case Save:
                case Delete:
                    return record.state == Committing;
            }
            return false;
        }

    }  // namespace

    /** @copydoc SaveOperationArbiter::~SaveOperationArbiter */
    SaveOperationArbiter::~SaveOperationArbiter() = default;
    SaveOperationArbiter::SaveOperationArbiter(SaveOperationArbiter &&) noexcept = default;
    SaveOperationArbiter &SaveOperationArbiter::operator=(SaveOperationArbiter &&) noexcept = default;

    SaveOperationArbiter::SaveOperationArbiter(std::unique_ptr<SaveOperationArbiterDetail::State> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc SaveOperationArbiter::Admit */
    Result<SaveArbiterAdmission> SaveOperationArbiter::Admit(SaveArbiterRequest request) {
        SaveStageObservation observation{SaveTelemetryStage::Queue, request.operation.operation};
        auto admitted = [this, &request] {
            if (state_->closed || !IsRequestValid(request) || Find(*state_, request.operation.operation) != state_->records.end())
                return Result<SaveArbiterAdmission>::Failure(
                    ArbiterError(SaveErrors::ArbiterInvalid, "The arbiter request is malformed or reuses an operation identity."));

            if (const auto resolved = ResolveExistingConflict(*state_, request); resolved.has_value())
                return *resolved;
            const std::optional<OperationId> replaced = ReplaceQueuedEquivalent(*state_, request);
            return AdmitNewRecord(*state_, std::move(request), replaced);
        }();
        const SaveTelemetryEvidence evidence{.droppedWork = admitted.HasError() || admitted.Value().disposition ==
                                                                                       SaveArbiterAdmissionDisposition::ReplacedQueued
                                                                ? 1U
                                                                : 0U,
                                             .queueDepth = observation.IsActive() ? QueuedCount() : 0U};
        if (admitted.HasError())
            observation.Fail(admitted.ErrorValue(), evidence);
        else
            observation.Complete(SaveTelemetryOutcome::Succeeded, evidence);
        return admitted;
    }

    /** @copydoc SaveOperationArbiter::StartNext */
    std::optional<SaveArbiterSnapshot> SaveOperationArbiter::StartNext() {
        if (state_->closed || state_->active.has_value())
            return std::nullopt;
        auto selected = state_->records.end();
        for (auto iterator = state_->records.begin(); iterator != state_->records.end(); ++iterator) {
            if (iterator->state != SaveArbiterState::Queued || IsTerminal(*iterator))
                continue;
            if (selected == state_->records.end() || Precedes(*iterator, *selected))
                selected = iterator;
        }
        if (selected == state_->records.end())
            return std::nullopt;
        state_->active = selected->request.operation.operation;
        return CopySnapshot(*selected);
    }

    /** @copydoc SaveOperationArbiter::Advance */
    Result<void> SaveOperationArbiter::Advance(const OperationId operation, const SaveArbiterState next,
                                               const SaveOperationProgress progress) {
        const auto found = Find(*state_, operation);
        if (!state_->active.has_value() || *state_->active != operation || found == state_->records.end() ||
            !IsAllowedState(found->request.operation.kind, found->state, next))
            return Result<void>::Failure(
                ArbiterError(SaveErrors::ArbiterInvalid, "Only the active operation may perform an allowed state transition."));

        if (Result<void> published = PublishArbiterProgress(*found, next, progress);
            published.HasError() && !SynchronizeTerminal(*state_, *found))
            return published;
        if (found->state == SaveArbiterState::Terminal)
            return Result<void>::Success();
        found->state = next;
        ++found->revision;
        return Result<void>::Success();
    }

    /** @copydoc SaveOperationArbiter::Complete */
    Result<void> SaveOperationArbiter::Complete(const OperationId operation) {
        const auto found = FindActiveRecord(*state_, operation);
        if (found == state_->records.end())
            return Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        const bool query = found->request.operation.kind == SaveOperationKind::RefreshCatalog;
        const SaveOperationCommitOutcome outcome = query ? SaveOperationCommitOutcome::NotCommitted : SaveOperationCommitOutcome::Committed;
        if (!IsReadyToComplete(*found))
            return Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        return FinishTerminalTransition(*state_, *found, found->controller.Complete(outcome));
    }

    /** @copydoc SaveOperationArbiter::Fail */
    Result<void> SaveOperationArbiter::Fail(const OperationId operation, Error error, const SaveOperationCommitOutcome outcome) {
        const auto found = FindActiveRecord(*state_, operation);
        if (found == state_->records.end())
            return Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        return FinishTerminalTransition(*state_, *found, found->controller.Fail(std::move(error), outcome));
    }

    /** @copydoc SaveOperationArbiter::BeginShutdown */
    Result<void> SaveOperationArbiter::BeginShutdown() {
        state_->closed = true;
        for (auto &record : state_->records) {
            if (IsTerminal(record))
                continue;
            if (const auto requested = record.controller.RequestShutdownCancellation();
                requested != SaveCancellationRequestResult::Requested && requested != SaveCancellationRequestResult::AlreadyRequested)
                continue;
            record.state = SaveArbiterState::Cancelling;
            ++record.revision;
            if (state_->active != record.request.operation.operation) {
                static_cast<void>(record.controller.ObserveCancellation());
                static_cast<void>(SynchronizeTerminal(*state_, record));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc SaveOperationArbiter::Cancel */
    SaveCancellationRequestResult SaveOperationArbiter::Cancel(const OperationId operation) {
        using enum SaveCancellationRequestResult;
        const auto found = Find(*state_, operation);
        if (found == state_->records.end())
            return InvalidHandle;
        const bool queued = state_->active != operation;
        const SaveCancellationRequestResult result = found->controller.Handle().RequestCancellation();
        if (result == Requested || result == AlreadyRequested) {
            found->state = SaveArbiterState::Cancelling;
            ++found->revision;
            if (queued) {
                static_cast<void>(found->controller.ObserveCancellation());
                static_cast<void>(SynchronizeTerminal(*state_, *found));
            }
        }
        return result;
    }

    /** @copydoc SaveOperationArbiter::ObserveCancellation */
    Result<void> SaveOperationArbiter::ObserveCancellation(const OperationId operation) {
        const auto found = Find(*state_, operation);
        if (!state_->active.has_value() || *state_->active != operation || found == state_->records.end() ||
            found->state != SaveArbiterState::Cancelling)
            return Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        static_cast<void>(found->controller.ObserveCancellation());
        return SynchronizeTerminal(*state_, *found) ? Result<void>::Success()
                                                    : Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
    }

    /** @copydoc SaveOperationArbiter::PollCancellation */
    Result<bool> SaveOperationArbiter::PollCancellation(const OperationId operation) {
        const auto found = FindActiveRecord(*state_, operation);
        if (found == state_->records.end())
            return Result<bool>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        const auto observed = found->controller.ObserveCancellation();
        const bool cancelled = observed == SaveCancellationObservation::Cancelled;
        if (cancelled)
            static_cast<void>(SynchronizeTerminal(*state_, *found));
        return Result<bool>::Success(cancelled);
    }

    /** @copydoc SaveOperationArbiter::Snapshot */
    std::optional<SaveArbiterSnapshot> SaveOperationArbiter::Snapshot(const OperationId operation) const {
        const auto found = Find(*state_, operation);
        return found == state_->records.end() ? std::nullopt : std::optional<SaveArbiterSnapshot>{CopySnapshot(*found)};
    }

    /** @copydoc SaveOperationArbiter::Acknowledge */
    bool SaveOperationArbiter::Acknowledge(const OperationId operation) {
        const auto found = Find(*state_, operation);
        if (found == state_->records.end() || !IsTerminal(*found))
            return false;
        state_->records.erase(found);
        return true;
    }

    /** @copydoc SaveOperationArbiter::ActiveOperation */
    std::optional<OperationId> SaveOperationArbiter::ActiveOperation() const noexcept {
        return state_->active;
    }

    /** @copydoc SaveOperationArbiter::QueuedCount */
    std::size_t SaveOperationArbiter::QueuedCount() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(state_->records, [](const State::Record &record) {
            return record.state == SaveArbiterState::Queued && !IsTerminal(record);
        }));
    }

    /** @copydoc CreateSaveOperationArbiter */
    Result<SaveOperationArbiter> CreateSaveOperationArbiter(const SaveOperationArbiterLimits limits) {
        if (limits.maximumRetainedOperations == 0)
            return Result<SaveOperationArbiter>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        auto state = std::unique_ptr<State>{new (std::nothrow) State{.limits = limits}};
        if (!state)
            return Result<SaveOperationArbiter>::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        try {
            state->records.reserve(limits.maximumRetainedOperations);
        } catch (const std::bad_alloc &) {
            return Result<SaveOperationArbiter>::Failure(MakeError(SaveErrors::OperationAllocationFailed));
        }
        return Result<SaveOperationArbiter>::Success(SaveOperationArbiter{std::move(state)});
    }
}  // namespace Horo::Runtime
