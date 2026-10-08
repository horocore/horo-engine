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

    /** @copydoc UiControlStateMachine::PrepareInteractionReplacement */
    Result<void> UiControlStateMachine::PrepareInteractionReplacement(const UiActionOwnerContext &owner) {
        if (!storage_ || !replacement_ || replacementSource_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Result<void>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        if (!owner.IsValid() || !SameAudience(Owner(), owner) ||
            owner.interaction.Compare(Owner().interaction) == UiRevisionRelation::Older)
            return Result<void>::Failure(MakeError(UiErrors::ControlSourceStale));
        const bool swap = Owner() != owner;
        if (swap && (replacement_->asyncAction || storage_->pending || (storage_->asyncAction && storage_->asyncAction->Busy())))
            return Result<void>::Failure(MakeError(UiErrors::ControlDefaultPending));
        auto stamp = CaptureReloadStamp();
        if (stamp.HasError())
            return Result<void>::Failure(stamp.ErrorValue());
        if (!swap) {
            replacementSwap_ = false;
            replacementSource_.emplace(std::move(stamp).Value());
            return Result<void>::Success();
        }
        replacement_->descriptor = storage_->descriptor;
        std::visit([&owner](auto &typed) noexcept {
            typed.base.owner = owner;
        }, replacement_->descriptor);
        replacement_->state = storage_->state;
        replacement_->editStartText = storage_->editStartText;
        replacement_->configuredAvailability = storage_->configuredAvailability;
        replacement_->asyncAction.reset();
        replacement_->lastSequence = storage_->lastSequence;
        replacement_->lastTick = storage_->lastTick;
        replacement_->pending = false;
        replacement_->repeatArmed = false;
        replacement_->repeatNextTick = 0;
        replacement_->adjustment = UiControlAdjustment::Count;
        replacement_->lifecycle = UiControlLifecycleState::Active;
        UiControlDetail::SetPressed(replacement_->state, false);
        UiControlDetail::SetRepeating(replacement_->state, false);
        // Editing and its Cancel baseline are logical form state; neither is discarded by an interaction-only replacement.
        replacementSwap_ = true;
        replacementSource_.emplace(std::move(stamp).Value());
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::CanPublishInteractionReplacement */
    Result<void> UiControlStateMachine::CanPublishInteractionReplacement(const UiActionOwnerContext &owner) const {
        if (!replacement_ || !replacementSource_ || !MatchesReloadStamp(*replacementSource_) ||
            (replacementSwap_ ? UiControlDetail::BaseOf(replacement_->descriptor).owner : Owner()) != owner)
            return Result<void>::Failure(MakeError(UiErrors::ControlSourceStale));
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::PreparedInteractionState */
    const UiControlState &UiControlStateMachine::PreparedInteractionState() const noexcept {
        return replacementSwap_ ? replacement_->state : storage_->state;
    }

    /** @copydoc UiControlStateMachine::PublishInteractionReplacement */
    void UiControlStateMachine::PublishInteractionReplacement() noexcept {
        if (replacementSwap_)
            storage_.swap(replacement_);
        replacementSource_.reset();
        replacementSwap_ = false;
    }

    /** @copydoc UiControlStateMachine::AbandonInteractionReplacement */
    void UiControlStateMachine::AbandonInteractionReplacement() noexcept {
        replacementSource_.reset();
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
