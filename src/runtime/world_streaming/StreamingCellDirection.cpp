#include "Horo/WorldStreaming/StreamingCellDirection.h"

#include "WorldStreamingInternal.h"

#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Checks supported effective residency without selecting a fallback. */
        [[nodiscard]] bool IsKnown(const StreamingDesiredResidency desired) noexcept {
            return desired <= StreamingDesiredResidency::Activated;
        }

        /** @brief Compares complete canonical phase evidence, including the immutable terminal disposition. */
        [[nodiscard]] bool Matches(const StreamingCellOperation &left, const StreamingCellOperation &right) noexcept {
            return left.Handle() == right.Handle() && left.Kind() == right.Kind() && left.State() == right.State() &&
                   left.Outcome() == right.Outcome();
        }

        /** @brief Checks whether the unchanged operation target can satisfy the new demand. */
        [[nodiscard]] bool Reverses(const StreamingCellOperationKind kind, const StreamingDesiredResidency desired) noexcept {
            return (kind == StreamingCellOperationKind::Load && desired == StreamingDesiredResidency::Unloaded) ||
                   (kind == StreamingCellOperationKind::Activate && desired != StreamingDesiredResidency::Activated);
        }

        /** @brief Validates every immutable participant before ownership or admission changes. */
        [[nodiscard]] Result<void> ValidateParticipants(
            const StreamingCellDirectionConfig &config,
            const std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> &participants) {
            if (config.maximumParticipants == 0 || config.maximumParticipants > 1024 || !config.revision.IsValid() ||
                config.operation.State() != StreamingCellOperationState::Queued || config.capacityUnits == 0)
                return Internal::Failure<void>(WorldStreamingErrors::CellDirectionInvalid);
            if (!IsKnown(config.desired))
                return Internal::Failure<void>(WorldStreamingErrors::CellDirectionUnsupported);
            if (Reverses(config.operation.Kind(), config.desired))
                return Internal::Failure<void>(WorldStreamingErrors::CellDirectionInvalid);
            if (participants.size() > config.maximumParticipants)
                return Internal::Failure<void>(WorldStreamingErrors::CellDirectionCapacityExceeded);
            for (std::size_t index{}; index < participants.size(); ++index) {
                const auto &participant = participants[index];
                if (!participant || !participant->Requirement().participant.IsValid() || !participant->Requirement().revision.IsValid())
                    return Internal::Failure<void>(WorldStreamingErrors::CellDirectionInvalid);
                if (participant->Operation() != config.operation.Handle())
                    return Internal::Failure<void>(WorldStreamingErrors::CellDirectionStale);
                for (std::size_t prior{}; prior < index; ++prior)
                    if (participants[prior]->Requirement().participant == participant->Requirement().participant)
                        return Internal::Failure<void>(WorldStreamingErrors::CellDirectionInvalid);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc StreamingCellDirectionOwner::Create */
    Result<StreamingCellDirectionOwner> StreamingCellDirectionOwner::Create(
        StreamingSchedulerAdmissionLedger &scheduler, const StreamingCellDirectionConfig &config,
        std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> &participants) {
        if (const auto valid = ValidateParticipants(config, participants); valid.HasError())
            return Result<StreamingCellDirectionOwner>::Failure(valid.ErrorValue());
        auto reservation = scheduler.TryAdmit(config.operation, config.capacityUnits, scheduler.Limits().concurrency.revision);
        if (reservation.HasError())
            return Result<StreamingCellDirectionOwner>::Failure(reservation.ErrorValue());
        const auto admitted = config.operation.Advance(config.operation.Handle(), StreamingCellOperationTransition::Admit).Value();
        return Result<StreamingCellDirectionOwner>::Success(
            StreamingCellDirectionOwner{scheduler, admitted, reservation.Value(), config, std::move(participants)});
    }

    /** @copydoc StreamingCellDirectionOwner::StreamingCellDirectionOwner */
    StreamingCellDirectionOwner::StreamingCellDirectionOwner(StreamingCellDirectionOwner &&other) noexcept
        : scheduler_(std::exchange(other.scheduler_, nullptr)), operation_(other.operation_),
          reservation_(std::exchange(other.reservation_, std::nullopt)), revision_(other.revision_), desired_(other.desired_),
          participants_(std::move(other.participants_)), nextRetirement_(other.nextRetirement_),
          retirementStarted_(other.retirementStarted_), demandClosed_(other.demandClosed_), terminalConsumed_(other.terminalConsumed_),
          participantsConsumed_(other.participantsConsumed_) {
        other.demandClosed_ = true;
        other.terminalConsumed_ = true;
        other.participantsConsumed_ = true;
    }

    /** @copydoc StreamingCellDirectionOwner::UpdateDemand */
    Result<void> StreamingCellDirectionOwner::UpdateDemand(const StreamingCellOperationHandle &expected,
                                                           const StreamingCellDemandRevision revision,
                                                           const StreamingCellDemandRevision successor,
                                                           const StreamingDesiredResidency desired) {
        if (!expected.IsValid() || !revision.IsValid() || !successor.IsValid())
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionInvalid);
        if (!IsKnown(desired))
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionUnsupported);
        if (expected != operation_.Handle() || revision != revision_ || successor <= revision_)
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionStale);
        if (!scheduler_ || demandClosed_ || operation_.IsTerminal() || scheduler_->State() != StreamingSchedulerAdmissionState::Accepting)
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        if (Reverses(operation_.Kind(), desired)) {
            if (const auto interrupted = BeginInterruption(StreamingCellOperationTransition::Cancel); interrupted.HasError())
                return interrupted;
        }
        revision_ = successor;
        desired_ = desired;
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellDirectionOwner::Advance */
    Result<StreamingCellOperation> StreamingCellDirectionOwner::Advance(const StreamingCellOperation &expected,
                                                                        const StreamingCellOperationTransition transition) {
        using enum StreamingCellOperationTransition;
        if (!Matches(expected, operation_))
            return Internal::Failure<StreamingCellOperation>(WorldStreamingErrors::CellDirectionStale);
        if (const bool supported = transition == BeginPreparation || transition == BeginActivation || transition == BeginRetirement ||
                                   (transition == Complete && operation_.Kind() == StreamingCellOperationKind::Load);
            !supported)
            return Internal::Failure<StreamingCellOperation>(WorldStreamingErrors::CellDirectionUnsupported);
        if (!scheduler_ || !reservation_ || demandClosed_ || scheduler_->State() != StreamingSchedulerAdmissionState::Accepting)
            return Internal::Failure<StreamingCellOperation>(WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        auto next = scheduler_->Advance(*reservation_, transition);
        if (next.HasError())
            return next;
        operation_ = next.Value();
        if (operation_.State() == StreamingCellOperationState::Retiring)
            RevokeParticipants();
        if (const auto released = ReleaseTerminal(); released.HasError())
            return Result<StreamingCellOperation>::Failure(released.ErrorValue());
        return Result<StreamingCellOperation>::Success(operation_);
    }

    /** @copydoc StreamingCellDirectionOwner::CommitActivation */
    Result<void> StreamingCellDirectionOwner::CommitActivation(StreamingCellActivationTransaction &transaction,
                                                               const StreamingCellActivationCommitPoint point) {
        using enum StreamingCellActivationLifecycle;
        const auto lifecycle = scheduler_ && scheduler_->State() == StreamingSchedulerAdmissionState::Accepting && !demandClosed_ &&
                                       reservation_ && operation_.State() == StreamingCellOperationState::Activating
                                   ? Active
                                   : Cancelling;
        if (lifecycle != Active) {
            transaction.Rollback();
            return Internal::Failure<void>(WorldStreamingErrors::CellActivationLifecycleUnavailable);
        }
        if (const auto committed = transaction.Commit(operation_, point, lifecycle); committed.HasError())
            return committed;
        auto next = scheduler_->Advance(*reservation_, StreamingCellOperationTransition::Complete);
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        operation_ = next.Value();
        return ReleaseTerminal();
    }

    /** @copydoc StreamingCellDirectionOwner::Interrupt */
    Result<void> StreamingCellDirectionOwner::Interrupt(const StreamingCellOperationHandle &expected,
                                                        const StreamingCellOperationTransition reason) {
        using enum StreamingCellOperationTransition;
        if (!expected.IsValid())
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionInvalid);
        if (expected != operation_.Handle())
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionStale);
        if (reason != Cancel && reason != Fail && reason != Replace && reason != Shutdown)
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionUnsupported);
        if (!scheduler_ || operation_.IsTerminal())
            return Internal::Failure<void>(WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        if (const auto interrupted = BeginInterruption(reason); interrupted.HasError())
            return interrupted;
        demandClosed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellDirectionOwner::BeginInterruption */
    Result<void> StreamingCellDirectionOwner::BeginInterruption(const StreamingCellOperationTransition reason) {
        if (operation_.State() == StreamingCellOperationState::Retiring)
            return Result<void>::Success();
        auto next = scheduler_->Advance(*reservation_, reason);
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        operation_ = next.Value();
        RevokeParticipants();
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellDirectionOwner::RevokeParticipants */
    void StreamingCellDirectionOwner::RevokeParticipants() const noexcept {
        for (const auto &participant : participants_)
            participant->RevokeAccess();
    }

    /** @copydoc StreamingCellDirectionOwner::PollRetirement */
    Result<StreamingCellOperation> StreamingCellDirectionOwner::PollRetirement() {
        if (!scheduler_ || operation_.State() != StreamingCellOperationState::Retiring)
            return Internal::Failure<StreamingCellOperation>(WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        while (nextRetirement_ < participants_.size()) {
            const auto &participant = participants_[nextRetirement_];
            if (!retirementStarted_) {
                participant->BeginRetirement();
                retirementStarted_ = true;
            }
            auto polled = participant->PollRetirement();
            if (polled.HasError())
                return Result<StreamingCellOperation>::Failure(polled.ErrorValue());
            if (!polled.Value())
                return Result<StreamingCellOperation>::Success(operation_);
            if (const auto &ack = *polled.Value(); ack.operation != operation_.Handle() || ack.participant != participant->Requirement())
                return Internal::Failure<StreamingCellOperation>(WorldStreamingErrors::CellDirectionStale);
            ++nextRetirement_;
            retirementStarted_ = false;
        }
        auto next = scheduler_->Advance(*reservation_, StreamingCellOperationTransition::AcknowledgeRetirement);
        if (next.HasError())
            return next;
        operation_ = next.Value();
        if (const auto released = ReleaseTerminal(); released.HasError())
            return Result<StreamingCellOperation>::Failure(released.ErrorValue());
        participants_.clear();
        return Result<StreamingCellOperation>::Success(operation_);
    }

    /** @copydoc StreamingCellDirectionOwner::ReleaseTerminal */
    Result<void> StreamingCellDirectionOwner::ReleaseTerminal() {
        if (!operation_.IsTerminal() || !reservation_)
            return Result<void>::Success();
        if (const auto released = scheduler_->Release(*reservation_); released.HasError())
            return released;
        reservation_.reset();
        return Result<void>::Success();
    }

    /** @copydoc StreamingCellDirectionOwner::TakeSucceededParticipants */
    Result<std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>>> StreamingCellDirectionOwner::TakeSucceededParticipants() {
        using Participants = std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>>;
        if (!scheduler_ || !operation_.IsTerminal() || reservation_ || operation_.Kind() == StreamingCellOperationKind::Retire ||
            operation_.Outcome() != StreamingCellOperationOutcome::Succeeded || participantsConsumed_)
            return Internal::Failure<Participants>(WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        participantsConsumed_ = true;
        return Result<Participants>::Success(std::move(participants_));
    }

    /** @copydoc StreamingCellDirectionOwner::TakeTerminalResult */
    Result<StreamingCellOperation> StreamingCellDirectionOwner::TakeTerminalResult() {
        if (!scheduler_ || !operation_.IsTerminal() || reservation_ || terminalConsumed_)
            return Internal::Failure<StreamingCellOperation>(WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        terminalConsumed_ = true;
        return Result<StreamingCellOperation>::Success(operation_);
    }

    /** @copydoc StreamingCellDirectionOwner::Operation */
    const StreamingCellOperation &StreamingCellDirectionOwner::Operation() const noexcept {
        return operation_;
    }

    /** @copydoc StreamingCellDirectionOwner::Revision */
    StreamingCellDemandRevision StreamingCellDirectionOwner::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc StreamingCellDirectionOwner::Desired */
    StreamingDesiredResidency StreamingCellDirectionOwner::Desired() const noexcept {
        return desired_;
    }

    /** @copydoc StreamingCellDirectionOwner::RequiresFreshAttempt */
    bool StreamingCellDirectionOwner::RequiresFreshAttempt() const noexcept {
        return scheduler_ && scheduler_->State() == StreamingSchedulerAdmissionState::Accepting && !demandClosed_ &&
               operation_.IsTerminal() &&
               (operation_.Kind() == StreamingCellOperationKind::Retire ||
                operation_.Outcome() != StreamingCellOperationOutcome::Succeeded) &&
               desired_ != StreamingDesiredResidency::Unloaded;
    }

    StreamingCellDirectionOwner::StreamingCellDirectionOwner(
        StreamingSchedulerAdmissionLedger &scheduler, StreamingCellOperation operation, StreamingSchedulerReservation reservation,
        const StreamingCellDirectionConfig &config, std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>> participants) noexcept
        : scheduler_(&scheduler), operation_(std::move(operation)), reservation_(reservation), revision_(config.revision),
          desired_(config.desired), participants_(std::move(participants)) {}
}  // namespace Horo::WorldStreaming
