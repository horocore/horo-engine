#include "UiAnimationOwnerInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::ApplyPresentation */
    Result<bool> UiAnimationOwner::ApplyPresentation(const UiPresentationReceipt &receipt) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped || storage_->draining ||
            storage_->candidate.admitted || !storage_->currentFrame.has_value())
            return Result<bool>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto &frame = *storage_->frames[*storage_->currentFrame];
        if (!frame.layout || !storage_->publisher.IsCurrent(frame.generation) || receipt.canvas != frame.layout->Descriptor().canvas ||
            receipt.interactionRevision != frame.layout->Descriptor().interaction)
            return Result<bool>::Failure(MakeError(UiErrors::RevisionStale));
        auto *canvas = storage_->publisher.Current()->Canvas(storage_->definition.canvas);
        const auto &layout = frame.layout->Descriptor();
        const UiActionOwnerContext source{layout.instance,         layout.canvas,       layout.document,
                                          layout.sources.document, layout.sources.tree, layout.interaction};
        bool adoptWrites{};
        if (canvas->bindings && receipt.outcome == UiPresentationOutcome::Presented) {
            const auto writable = canvas->bindings->CanAdoptAnimationPresentation(canvas->tree, source);
            if (writable.HasError())
                return Result<bool>::Failure(writable.ErrorValue());
            adoptWrites = writable.Value();
        }
        auto presented = storage_->publisher.ApplyPresentation(storage_->definition.canvas, receipt);
        if (presented.HasValue() && presented.Value() && adoptWrites)
            canvas->bindings->AdoptAnimationPresentationValidated(source);
        return presented;
    }

    /** @copydoc UiAnimationOwner::InputEligible */
    bool UiAnimationOwner::InputEligible(const UiRenderViewId view) const noexcept {
        return storage_ && storage_->ownerThread == std::this_thread::get_id() && !storage_->stopped && !storage_->draining &&
               !storage_->candidate.admitted && !storage_->route.gate && storage_->currentFrame.has_value() &&
               storage_->publisher.InputEligible(storage_->definition.canvas, view) &&
               (storage_->definition.routes.empty() || (ReadCanvas(*storage_)->routes && ReadCanvas(*storage_)->routes->Top()));
    }
}  // namespace Horo::Runtime::Ui
