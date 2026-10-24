#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiControlsInternal.h"

#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Interaction-only replacement cannot rebind the immutable semantic or retained-tree audience. */
        bool SameAudience(const UiActionOwnerContext &first, const UiActionOwnerContext &second) noexcept {
            return first.instance == second.instance && first.canvas == second.canvas && first.document == second.document &&
                   first.documentRevision == second.documentRevision && first.treeRevision == second.treeRevision;
        }
    }  // namespace

    /** @copydoc UiControlStateMachine::ReserveInteractionReplacement */
    Result<void> UiControlStateMachine::ReserveInteractionReplacement() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active || replacementSource_)
            return Result<void>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        if (replacement_)
            return Result<void>::Success();
        try {
            replacement_ = std::make_unique<Storage>(storage_->descriptor);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiControlStateMachine::CanPrepareInteractionReplacement */
    Result<void> UiControlStateMachine::CanPrepareInteractionReplacement(const UiActionOwnerContext &owner) const {
        if (!storage_ || !replacement_ || replacementSource_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Result<void>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        if (!owner.IsValid() || !SameAudience(Owner(), owner) ||
            owner.interaction.Compare(Owner().interaction) == UiRevisionRelation::Older)
            return Result<void>::Failure(MakeError(UiErrors::ControlSourceStale));
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Storage::CopyInteractionState */
    void UiControlStateMachine::Storage::CopyInteractionState(const Storage &source) {
        descriptor = source.descriptor;
        state = source.state;
        editStartText = source.editStartText;
        if (textEditor)
            *textEditor = *source.textEditor;
        configuredAvailability = source.configuredAvailability;
        asyncAction = source.asyncAction;
        pendingDefault = source.pendingDefault;
        pressSource = source.pressSource;
        adjustment = source.adjustment;
        lastSequence = source.lastSequence;
        lastTick = source.lastTick;
        textResetRevision = source.textResetRevision;
        repeatNextTick = source.repeatNextTick;
        pending = source.pending;
        repeatArmed = source.repeatArmed;
        lifecycle = source.lifecycle;
    }

    /** @copydoc UiControlStateMachine::Storage::ProjectAsync */
    void UiControlStateMachine::Storage::ProjectAsync(std::optional<UiAsyncActionSnapshot> projection) {
        const bool busy = projection && projection->Busy();
        if (busy)
            ClearTransient(false);
        asyncAction = std::move(projection);
        SetAvailabilityProjection(configuredAvailability == UiControlAvailability::Enabled && busy ? UiControlAvailability::Busy
                                                                                                   : configuredAvailability);
    }

    /** @copydoc UiControlStateMachine::PrepareInteractionReplacement */
    Result<void> UiControlStateMachine::PrepareInteractionReplacement(const UiActionOwnerContext &owner,
                                                                      const UiAsyncActionStore *actions) {
        if (const auto admitted = CanPrepareInteractionReplacement(owner); admitted.HasError())
            return admitted;
        auto stamp = CaptureReloadStamp();
        if (stamp.HasError())
            return Result<void>::Failure(stamp.ErrorValue());
        auto projected =
            actions ? actions->Project({Owner(), Element()}) : Result<std::optional<UiAsyncActionSnapshot>>::Success(std::nullopt);
        if (projected.HasError())
            return Result<void>::Failure(projected.ErrorValue());
        replacement_->CopyInteractionState(*storage_);
        replacement_->ProjectAsync(std::move(projected).Value());
        replacementActions_ = actions;
        replacementSwap_ = true;
        replacementSource_.emplace(std::move(stamp).Value());
        return RebasePreparedInteraction(owner);
    }

    /** @copydoc UiControlStateMachine::RebasePreparedInteraction */
    Result<void> UiControlStateMachine::RebasePreparedInteraction(const UiActionOwnerContext &owner) {
        if (!replacementSource_ || !owner.IsValid() || !SameAudience(Owner(), owner) ||
            owner.interaction.Compare(Owner().interaction) == UiRevisionRelation::Older)
            return Result<void>::Failure(MakeError(UiErrors::ControlSourceStale));
        if (owner != Owner()) {
            if (replacement_->pending)
                return Result<void>::Failure(MakeError(UiErrors::ControlDefaultPending));
            UiControlDetail::SetPressed(replacement_->state, false);
            UiControlDetail::SetRepeating(replacement_->state, false);
            replacement_->repeatArmed = false;
            replacement_->repeatNextTick = 0;
            replacement_->adjustment = UiControlAdjustment::Count;
        }
        std::visit([&owner](auto &typed) noexcept {
            typed.base.owner = owner;
        }, replacement_->descriptor);
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::CanPublishInteractionReplacement */
    Result<void> UiControlStateMachine::CanPublishInteractionReplacement(const UiActionOwnerContext &owner) const {
        if (!replacement_ || !replacementSource_ || !MatchesReloadStamp(*replacementSource_) ||
            (replacementSwap_ ? UiControlDetail::BaseOf(replacement_->descriptor).owner : Owner()) != owner)
            return Result<void>::Failure(MakeError(UiErrors::ControlSourceStale));
        if (replacementActions_ &&
            !replacementActions_->CanPublishObservation({Owner(), Element()},
                                                        replacement_->asyncAction ? &*replacement_->asyncAction : nullptr))
            return Result<void>::Failure(MakeError(UiErrors::ActionSourceStale));
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::PreparedInteractionState */
    const UiControlState &UiControlStateMachine::PreparedInteractionState() const noexcept {
        return replacementSwap_ ? replacement_->state : storage_->state;
    }

    /** @copydoc UiControlStateMachine::PreparedAsyncAction */
    const std::optional<UiAsyncActionSnapshot> &UiControlStateMachine::PreparedAsyncAction() const noexcept {
        return replacement_->asyncAction;
    }

    /** @copydoc UiControlStateMachine::PublishInteractionReplacement */
    void UiControlStateMachine::PublishInteractionReplacement() noexcept {
        if (replacementSwap_)
            storage_.swap(replacement_);
        replacementSource_.reset();
        replacementActions_ = nullptr;
        replacementSwap_ = false;
    }

    /** @copydoc UiControlStateMachine::AbandonInteractionReplacement */
    void UiControlStateMachine::AbandonInteractionReplacement() noexcept {
        replacementSource_.reset();
        replacementActions_ = nullptr;
        replacementSwap_ = false;
    }

    /** @copydoc UiControlStateMachine::DrainInteractionReplacement */
    std::size_t UiControlStateMachine::DrainInteractionReplacement() noexcept {
        if (replacementSource_ || !replacement_ || !replacement_->asyncAction)
            return 0;
        replacement_->asyncAction.reset();
        return 1;
    }

}  // namespace Horo::Runtime::Ui
