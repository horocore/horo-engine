#include "Horo/WorldStreaming/OriginRebaseTransaction.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        [[nodiscard]] bool IsKnown(const OriginRebaseLifecycle value) noexcept {
            return value < OriginRebaseLifecycle::Count;
        }

        [[nodiscard]] bool IsKnown(const OriginRebaseCommitPoint value) noexcept {
            return value < OriginRebaseCommitPoint::Count;
        }

        [[nodiscard]] bool IsKnown(const OriginShiftDecisionKind value) noexcept {
            return value == OriginShiftDecisionKind::Remain || value == OriginShiftDecisionKind::RequestShift;
        }

        [[nodiscard]] bool IsKnown(const OriginShiftRequester value) noexcept {
            return value < OriginShiftRequester::Count;
        }

        [[nodiscard]] bool IsValid(const OriginRebaseParticipantRequirement &value) noexcept {
            return value.participant.IsValid() && value.revision.IsValid();
        }

        void RollbackPrepared(std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>> &prepared) noexcept {
            for (auto iterator = prepared.rbegin(); iterator != prepared.rend(); ++iterator) {
                if (*iterator)
                    (*iterator)->RollbackPrepared();
            }
        }

        [[nodiscard]] Result<void> ValidateContextShape(const OriginRebaseTransactionContext &context) {
            if (!context.transaction.IsValid() || !context.decision.request.IsValid() || !context.decision.observedFrame.IsValid() ||
                context.maximumParticipants == 0 || !IsKnown(context.lifecycle) || !IsKnown(context.decision.kind) ||
                !IsKnown(context.decision.requester))
                return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseInvalid);
            if (context.lifecycle != OriginRebaseLifecycle::Active)
                return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDecision(const OriginRebaseTransactionContext &context) {
            if (context.decision.thresholdMillimeters == 0 ||
                context.decision.thresholdMillimeters > static_cast<std::uint64_t>(OriginFrame::MaximumLocalHalfExtentMillimeters) ||
                context.decision.kind != OriginShiftDecisionKind::RequestShift ||
                context.activeFrame.Origin() == context.decision.targetOrigin)
                return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseInvalid);
            if (context.activeFrame.Binding() != context.decision.observedFrame)
                return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<OriginRebaseEvent> BuildEvent(const OriginRebaseTransactionContext &context) {
            const auto nextRevision = NextOriginFrameRevision(context.decision.observedFrame.revision);
            if (nextRevision.HasError())
                return Result<OriginRebaseEvent>::Failure(nextRevision.ErrorValue());
            const auto nextGeneration = NextOriginGeneration(context.decision.observedFrame.generation);
            if (nextGeneration.HasError())
                return Result<OriginRebaseEvent>::Failure(nextGeneration.ErrorValue());
            auto replacement = OriginFrame::Create({.identity = context.decision.observedFrame.identity,
                                                    .revision = nextRevision.Value(),
                                                    .generation = nextGeneration.Value()},
                                                   context.decision.targetOrigin);
            if (replacement.HasError())
                return Result<OriginRebaseEvent>::Failure(replacement.ErrorValue());
            return Result<OriginRebaseEvent>::Success({.transaction = context.transaction,
                                                       .request = context.decision.request,
                                                       .previousFrame = context.activeFrame,
                                                       .replacementFrame = std::move(replacement).Value()});
        }

        [[nodiscard]] Result<std::vector<OriginRebaseParticipantRequirement>> CanonicalizeRequirements(
            const std::span<const OriginRebaseParticipantRequirement> required, const std::size_t participantCount,
            const std::size_t maximumParticipants) {
            if (required.empty() || required.size() != participantCount)
                return Internal::Failure<std::vector<OriginRebaseParticipantRequirement>>(WorldStreamingErrors::OriginRebaseIncomplete);
            if (required.size() > maximumParticipants)
                return Internal::Failure<std::vector<OriginRebaseParticipantRequirement>>(
                    WorldStreamingErrors::OriginRebaseCapacityExceeded);

            std::vector<OriginRebaseParticipantRequirement> canonical{required.begin(), required.end()};
            std::ranges::sort(canonical, {}, &OriginRebaseParticipantRequirement::participant);
            if (std::ranges::any_of(canonical, [](const auto &requirement) {
                return !IsValid(requirement);
            }))
                return Internal::Failure<std::vector<OriginRebaseParticipantRequirement>>(WorldStreamingErrors::OriginRebaseInvalid);
            if (std::ranges::adjacent_find(canonical, {}, &OriginRebaseParticipantRequirement::participant) != canonical.end())
                return Internal::Failure<std::vector<OriginRebaseParticipantRequirement>>(WorldStreamingErrors::OriginRebaseInvalid);
            return Result<std::vector<OriginRebaseParticipantRequirement>>::Success(std::move(canonical));
        }

        [[nodiscard]] bool ParticipantLess(const IOriginRebaseParticipant *left, const IOriginRebaseParticipant *right) noexcept {
            if (left == nullptr)
                return right != nullptr;
            if (right == nullptr)
                return false;
            return left->Requirement().participant < right->Requirement().participant;
        }

        [[nodiscard]] Result<std::vector<IOriginRebaseParticipant *>> CanonicalizeParticipants(
            const std::span<IOriginRebaseParticipant *const> participants,
            const std::span<const OriginRebaseParticipantRequirement> requirements) {
            std::vector<IOriginRebaseParticipant *> canonical{participants.begin(), participants.end()};
            std::ranges::sort(canonical, ParticipantLess);
            for (std::size_t index{}; index < canonical.size(); ++index) {
                if (canonical[index] == nullptr || !IsValid(canonical[index]->Requirement()))
                    return Internal::Failure<std::vector<IOriginRebaseParticipant *>>(WorldStreamingErrors::OriginRebaseInvalid);
                if (index > 0 && canonical[index - 1]->Requirement().participant == canonical[index]->Requirement().participant)
                    return Internal::Failure<std::vector<IOriginRebaseParticipant *>>(WorldStreamingErrors::OriginRebaseInvalid);
                if (canonical[index]->Requirement().participant != requirements[index].participant)
                    return Internal::Failure<std::vector<IOriginRebaseParticipant *>>(WorldStreamingErrors::OriginRebaseIncomplete);
                if (canonical[index]->Requirement().revision != requirements[index].revision)
                    return Internal::Failure<std::vector<IOriginRebaseParticipant *>>(WorldStreamingErrors::OriginRebaseStale);
            }
            return Result<std::vector<IOriginRebaseParticipant *>>::Success(std::move(canonical));
        }

        [[nodiscard]] bool ReceiptMatches(const OriginRebasePreparedBinding &binding, const OriginRebaseParticipantRequirement &requirement,
                                          const OriginRebaseEvent &event) noexcept {
            return binding.requirement == requirement && binding.transaction == event.transaction &&
                   binding.previousFrame == event.previousFrame.Binding() && binding.replacementFrame == event.replacementFrame.Binding();
        }

        [[nodiscard]] Result<std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>>> PrepareParticipants(
            const std::span<IOriginRebaseParticipant *const> participants,
            const std::span<const OriginRebaseParticipantRequirement> requirements, const OriginRebaseEvent &event) {
            std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>> prepared;
            prepared.reserve(participants.size());
            for (std::size_t index{}; index < participants.size(); ++index) {
                auto receiptResult = participants[index]->Prepare(event);
                if (receiptResult.HasError()) {
                    RollbackPrepared(prepared);
                    return Result<std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>>>::Failure(receiptResult.ErrorValue());
                }
                auto receipt = std::move(receiptResult).Value();
                if (!receipt || !ReceiptMatches(receipt->Binding(), requirements[index], event)) {
                    if (receipt)
                        receipt->RollbackPrepared();
                    RollbackPrepared(prepared);
                    return Internal::Failure<std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>>>(
                        WorldStreamingErrors::OriginRebaseStale);
                }
                prepared.push_back(std::move(receipt));
            }
            return Result<std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>>>::Success(std::move(prepared));
        }

        [[nodiscard]] Result<void> ValidateActiveFrame(const OriginFrameOwner &owner, const OriginFrame &expected) {
            const auto lease = owner.Lease();
            if (lease.HasError())
                return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
            if (const auto active = lease.Value().Get(); active.HasError() || active.Value() != expected)
                return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseStale);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> PublishFrame(OriginFrameOwner &owner, const OriginFrame &replacement) {
            if (const auto staged = owner.StageReplacement(replacement); staged.HasError())
                return Result<void>::Failure(staged.ErrorValue());
            if (const auto published = owner.PublishReplacement(); published.HasError()) {
                owner.CancelReplacement();
                return Result<void>::Failure(published.ErrorValue());
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OriginRebaseTransaction::~OriginRebaseTransaction */
    OriginRebaseTransaction::~OriginRebaseTransaction() {
        Rollback();
    }

    /** @copydoc OriginRebaseTransaction::OriginRebaseTransaction */
    OriginRebaseTransaction::OriginRebaseTransaction(OriginRebaseTransaction &&other) noexcept
        : context_(other.context_), event_(other.event_), requirements_(std::move(other.requirements_)),
          prepared_(std::move(other.prepared_)), state_(other.state_) {
        other.state_ = OriginRebaseTransactionState::RolledBack;
    }

    /** @copydoc OriginRebaseTransaction::operator= */
    OriginRebaseTransaction &OriginRebaseTransaction::operator=(OriginRebaseTransaction &&other) noexcept {
        if (this == &other)
            return *this;
        Rollback();
        context_ = other.context_;
        event_ = other.event_;
        requirements_ = std::move(other.requirements_);
        prepared_ = std::move(other.prepared_);
        state_ = other.state_;
        other.state_ = OriginRebaseTransactionState::RolledBack;
        return *this;
    }

    /** @copydoc OriginRebaseTransaction::Prepare */
    Result<OriginRebaseTransaction> OriginRebaseTransaction::Prepare(const OriginRebaseTransactionContext &context,
                                                                     const std::span<const OriginRebaseParticipantRequirement> required,
                                                                     const std::span<IOriginRebaseParticipant *const> participants) {
        if (const auto valid = ValidateContextShape(context); valid.HasError())
            return Result<OriginRebaseTransaction>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateDecision(context); valid.HasError())
            return Result<OriginRebaseTransaction>::Failure(valid.ErrorValue());

        try {
            auto requirements = CanonicalizeRequirements(required, participants.size(), context.maximumParticipants);
            if (requirements.HasError())
                return Result<OriginRebaseTransaction>::Failure(requirements.ErrorValue());
            auto canonicalParticipants = CanonicalizeParticipants(participants, requirements.Value());
            if (canonicalParticipants.HasError())
                return Result<OriginRebaseTransaction>::Failure(canonicalParticipants.ErrorValue());
            auto event = BuildEvent(context);
            if (event.HasError())
                return Result<OriginRebaseTransaction>::Failure(event.ErrorValue());

            auto prepared = PrepareParticipants(canonicalParticipants.Value(), requirements.Value(), event.Value());
            if (prepared.HasError())
                return Result<OriginRebaseTransaction>::Failure(prepared.ErrorValue());

            return Result<OriginRebaseTransaction>::Success(
                OriginRebaseTransaction{context, std::move(event).Value(), std::move(requirements).Value(), std::move(prepared).Value()});
        } catch (const std::bad_alloc &) {
            return Internal::Failure<OriginRebaseTransaction>(WorldStreamingErrors::OriginRebaseStorageUnavailable);
        }
    }

    /** @copydoc OriginRebaseTransaction::Commit */
    Result<void> OriginRebaseTransaction::Commit(OriginFrameOwner &owner, const OriginRebaseCommitPoint commitPoint,
                                                 const OriginRebaseLifecycle lifecycle) {
        if (state_ != OriginRebaseTransactionState::Prepared)
            return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
        if (!IsKnown(commitPoint))
            return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseUnsupported);
        if (commitPoint != OriginRebaseCommitPoint::PostSimulation)
            return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseSafePointUnavailable);
        if (!IsKnown(lifecycle)) {
            Rollback();
            return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseUnsupported);
        }
        if (lifecycle != OriginRebaseLifecycle::Active) {
            Rollback();
            return Internal::Failure<void>(WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
        }

        if (const auto active = ValidateActiveFrame(owner, event_.previousFrame); active.HasError()) {
            Rollback();
            return Result<void>::Failure(active.ErrorValue());
        }
        if (const auto published = PublishFrame(owner, event_.replacementFrame); published.HasError()) {
            Rollback();
            return Result<void>::Failure(published.ErrorValue());
        }

        state_ = OriginRebaseTransactionState::Published;
        for (const auto &receipt : prepared_)
            receipt->ApplyPrepared();
        return Result<void>::Success();
    }

    /** @copydoc OriginRebaseTransaction::Rollback */
    void OriginRebaseTransaction::Rollback() noexcept {
        if (state_ != OriginRebaseTransactionState::Prepared)
            return;
        RollbackPrepared(prepared_);
        state_ = OriginRebaseTransactionState::RolledBack;
    }

    /** @copydoc OriginRebaseTransaction::Id */
    OriginRebaseTransactionId OriginRebaseTransaction::Id() const noexcept {
        return context_.transaction;
    }

    /** @copydoc OriginRebaseTransaction::Event */
    const OriginRebaseEvent &OriginRebaseTransaction::Event() const noexcept {
        return event_;
    }

    /** @copydoc OriginRebaseTransaction::State */
    OriginRebaseTransactionState OriginRebaseTransaction::State() const noexcept {
        return state_;
    }

    /** @copydoc OriginRebaseTransaction::Requirements */
    std::span<const OriginRebaseParticipantRequirement> OriginRebaseTransaction::Requirements() const noexcept {
        return requirements_;
    }

    OriginRebaseTransaction::OriginRebaseTransaction(OriginRebaseTransactionContext context, OriginRebaseEvent event,
                                                     std::vector<OriginRebaseParticipantRequirement> requirements,
                                                     std::vector<std::unique_ptr<IOriginRebasePreparedParticipant>> prepared) noexcept
        : context_(std::move(context)), event_(std::move(event)), requirements_(std::move(requirements)), prepared_(std::move(prepared)),
          state_(OriginRebaseTransactionState::Prepared) {}
}  // namespace Horo::WorldStreaming
