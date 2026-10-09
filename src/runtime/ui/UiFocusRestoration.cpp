#include "UiFocusGraphInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiFocusGraph::CaptureRestoration */
    Result<UiFocusRestoration> UiFocusGraph::CaptureRestoration() const {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return FocusGraphDetail::Failure<UiFocusRestoration>(UiErrors::FocusLifecycleUnavailable);
        Storage::RestorationEntry path;
        storage_->CollectPath(storage_->focusedIndex, path);
        return Result<UiFocusRestoration>::Success({storage_->descriptor.owner, path.focused, path.ancestors, path.ancestorCount});
    }

    /** @copydoc UiFocusGraph::Restore */
    Result<UiFocusChange> UiFocusGraph::Restore(const UiFocusRestoration &restoration) {
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return FocusGraphDetail::Failure<UiFocusChange>(UiErrors::FocusLifecycleUnavailable);
        const auto &current = storage_->descriptor.owner;
        if (!restoration.source.IsValid() || restoration.ancestorCount > restoration.ancestors.size())
            return FocusGraphDetail::Failure<UiFocusChange>(UiErrors::FocusInvalid);
        if (!FocusGraphDetail::SameStaticScope(restoration.source, current))
            return FocusGraphDetail::Failure<UiFocusChange>(UiErrors::FocusScopeMismatch);
        if (restoration.source.documentRevision > current.documentRevision || restoration.source.treeRevision > current.treeRevision ||
            restoration.source.interaction > current.interaction)
            return FocusGraphDetail::Failure<UiFocusChange>(UiErrors::FocusSourceStale);
        const auto previous = storage_->CurrentTarget();
        const Storage::RestorationEntry path{restoration.focused, restoration.ancestors, restoration.ancestorCount};
        storage_->focusedIndex = storage_->ResolveRestoration(path);
        return Result<UiFocusChange>::Success(storage_->BuildChange(previous, UiFocusChangeReason::ModalClosed));
    }
}  // namespace Horo::Runtime::Ui
