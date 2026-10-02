#include "UiFocusGraphInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiFocusGraph::SetParticipation */
    Result<UiFocusChange> UiFocusGraph::SetParticipation(const UiFocusOwnerContext &expectedOwner,
                                                         const UiFocusParticipation &participation) {
        using FocusGraphDetail::Failure;
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        if (!FocusGraphDetail::SameStaticScope(expectedOwner, storage_->descriptor.owner))
            return Failure<UiFocusChange>(UiErrors::FocusScopeMismatch);
        if (expectedOwner != storage_->descriptor.owner || !participation.element.IsValid() ||
            participation.element.ownership != expectedOwner.instance.ownership)
            return Failure<UiFocusChange>(UiErrors::FocusSourceStale);
        const std::size_t index = storage_->FindNode(participation.element);
        if (index == Storage::InvalidIndex)
            return Failure<UiFocusChange>(UiErrors::FocusTargetUnavailable);
        const auto previous = storage_->CurrentTarget();
        auto &node = storage_->nodes[index].descriptor;
        node.focusable = participation.focusable;
        node.enabled = participation.enabled;
        node.visible = participation.visible;
        UiFocusChangeReason reason = UiFocusChangeReason::Explicit;
        if (storage_->focusedIndex.has_value() && !storage_->IsAllowed(*storage_->focusedIndex)) {
            Storage::RestorationEntry recovery;
            storage_->CollectPath(storage_->focusedIndex, recovery);
            recovery.focused = {};
            storage_->focusedIndex = storage_->ResolveRestoration(recovery);
            reason = UiFocusChangeReason::InvalidTarget;
        }
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, reason));
    }
}  // namespace Horo::Runtime::Ui
