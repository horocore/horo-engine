#include "UiFocusGraphInternal.h"

namespace Horo::Runtime::Ui {
    using FocusGraphDetail::DirectionIndex;
    using FocusGraphDetail::IsKnown;

    /** @copydoc UiFocusScope::IsValid */
    bool UiFocusScope::IsValid(const UiOwnershipGeneration expectedOwnership) const noexcept {
        if (!expectedOwnership.IsValid() || !presentationLayer.IsValid() || presentationLayer.ownership != expectedOwnership)
            return false;
        return !player.has_value() || (player->IsValid() && player->ownership == expectedOwnership);
    }

    /** @copydoc UiFocusOwnerContext::IsValid */
    bool UiFocusOwnerContext::IsValid() const noexcept {
        return instance.IsValid() && canvas.IsValid() && canvas.ownership == instance.ownership && document.IsValid() &&
               documentRevision.IsValid() && treeRevision.IsValid() && interaction.IsValid() && scope.IsValid(instance.ownership);
    }

    /** @copydoc UiFocusTarget::IsValid */
    bool UiFocusTarget::IsValid() const noexcept {
        return id.IsValid() && element.IsValid();
    }

    /** @copydoc UiFocusBringIntoViewRequest::IsValid */
    bool UiFocusBringIntoViewRequest::IsValid() const noexcept {
        return owner.IsValid() && target.IsValid() && target.element.ownership == owner.instance.ownership &&
               IsKnown(policy, UiFocusBringIntoViewPolicy::Count) && policy != UiFocusBringIntoViewPolicy::None;
    }

    /** @copydoc UiFocusChange::IsValid */
    bool UiFocusChange::IsValid() const noexcept {
        if (!IsKnown(kind, UiFocusChangeKind::Count) || !IsKnown(reason, UiFocusChangeReason::Count))
            return false;
        if ((previous.has_value() && !previous->IsValid()) || (current.has_value() && !current->IsValid()))
            return false;
        if (bringIntoView.has_value() && (!bringIntoView->IsValid() || !current.has_value() || bringIntoView->target != *current))
            return false;
        switch (kind) {
            case UiFocusChangeKind::Unchanged:
                return previous == current;
            case UiFocusChangeKind::FocusMoved:
            case UiFocusChangeKind::FocusRecovered:
                return current.has_value() && (!previous.has_value() || previous != current);
            case UiFocusChangeKind::NoTarget:
                return previous == current;
            case UiFocusChangeKind::FocusCleared:
                return previous.has_value() && !current.has_value();
            case UiFocusChangeKind::Count:
                return false;
        }
        return false;
    }

    /** @copydoc UiFocusGraphDescriptor::IsValid */
    bool UiFocusGraphDescriptor::IsValid() const noexcept {
        return owner.IsValid() && IsKnown(recovery, UiFocusRecoveryPolicy::Count) && nodeCapacity > 0 &&
               nodeCapacity <= MaximumUiFocusNodes && modalCapacity > 0 && modalCapacity <= MaximumUiFocusModalDepth &&
               restorationCapacity > 0 && restorationCapacity <= MaximumUiFocusRestorationDepth && restorationCapacity >= modalCapacity;
    }

    /** @copydoc UiFocusNeighborLinks::Target */
    UiElementId UiFocusNeighborLinks::Target(const UiNavigationDirection direction) const noexcept {
        const auto index = DirectionIndex(direction);
        return index.has_value() ? targets[*index] : UiElementId{};
    }

    /** @copydoc UiFocusNodeDescriptor::IsValid */
    bool UiFocusNodeDescriptor::IsValid() const noexcept {
        if (!element.IsValid() || !id.IsValid() || (parent.IsValid() && parent == id) ||
            !IsKnown(bringIntoView, UiFocusBringIntoViewPolicy::Count))
            return false;
        for (const UiElementId target : links.targets) {
            if (target.IsValid() && target == id)
                return false;
        }
        return true;
    }

    /** @copydoc UiFocusModalDescriptor::IsValid */
    bool UiFocusModalDescriptor::IsValid() const noexcept {
        return root.IsValid() && IsKnown(policy, UiFocusModalScopePolicy::Count);
    }

    /** @copydoc UiFocusModalActivation::IsValid */
    bool UiFocusModalActivation::IsValid() const noexcept {
        return modal.IsValid() && change.IsValid();
    }

    /** @copydoc UiFocusSnapshot::IsValid */
    bool UiFocusSnapshot::IsValid() const noexcept {
        return owner.IsValid() && (!focused.has_value() || focused->IsValid()) && (!activeModal.has_value() || activeModal->IsValid()) &&
               modalDepth <= MaximumUiFocusModalDepth &&
               ((modalDepth == 0 && !activeModal.has_value()) || (modalDepth != 0 && activeModal.has_value()));
    }
}  // namespace Horo::Runtime::Ui
