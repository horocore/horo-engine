#include "UiHotReloadInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    /** @copydoc UiHotReload::ApplyPresentation */
    Result<bool> UiHotReload::ApplyPresentation(const UiCanvasId canvasId, const UiPresentationReceipt &receipt) {
        if (!storage_ || storage_->stopped || storage_->collecting)
            return Result<bool>::Failure(MakeError(UiErrors::InstanceStateInvalid));
        // Updating eligibility requires the mutable publisher authority, never a const facade's borrowed state.
        const auto &publisher = storage_.PublisherPin();
        auto *canvas = publisher->current->Canvas(canvasId);
        if (!canvas || !canvas->layout || receipt.canvas != canvas->tree.Canvas() ||
            receipt.interactionRevision != canvas->layout->Descriptor().interaction)
            return Result<bool>::Failure(MakeError(UiErrors::RevisionStale));
        const auto tracker = std::ranges::find(canvas->presentations, receipt.view, &UiPresentedInteractionState::View);
        if (tracker == canvas->presentations.end())
            return Result<bool>::Failure(MakeError(UiErrors::HandleStale));
        return tracker->Apply(receipt);
    }

    /** @copydoc UiHotReload::InputEligible */
    bool UiHotReload::InputEligible(const UiCanvasId canvasId, const UiRenderViewId view) const noexcept {
        if (!storage_ || storage_->stopped || storage_->collecting)
            return false;
        const auto &generation = *storage_->current;
        const auto canvas = std::ranges::find(generation.Canvases(), canvasId, &UiReloadCanvas::id);
        if (canvas == generation.Canvases().end() || !canvas->layout)
            return false;
        const auto tracker = std::ranges::find(canvas->presentations, view, &UiPresentedInteractionState::View);
        return tracker != canvas->presentations.end() && tracker->LastPresentedInteraction() == canvas->layout->Descriptor().interaction;
    }
}  // namespace Horo::Runtime::Ui
