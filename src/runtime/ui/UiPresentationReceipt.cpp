#include "Horo/Runtime/Ui/UiPresentationReceipt.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <cmath>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    /** @copydoc UiWorldCanvasProjection::IsValid */
    bool UiWorldCanvasProjection::IsValid() const noexcept {
        if (!Math::IsFinite(origin) || !Math::IsFinite(horizontalExtent) || !Math::IsFinite(verticalExtent))
            return false;
        const auto normalLength = Math::LengthSquared(Math::Cross(horizontalExtent, verticalExtent));
        const double xx = Math::Dot(horizontalExtent, horizontalExtent);
        const double xy = Math::Dot(horizontalExtent, verticalExtent);
        const double yy = Math::Dot(verticalExtent, verticalExtent);
        const double determinant = xx * yy - xy * xy;
        return std::isfinite(normalLength) && normalLength > Math::DefaultEpsilon && std::isfinite(determinant) &&
               determinant > static_cast<double>(Math::DefaultEpsilon) * normalLength;
    }

    /** @copydoc UiPresentationReceipt::IsValid */
    bool UiPresentationReceipt::IsValid() const noexcept {
        using enum UiPresentationOutcome;
        using enum UiPresentationReason;
        if (!view.IsValid() || !canvas.IsValid() || !interactionRevision.IsValid() || !snapshotRevision.IsValid())
            return false;
        if (worldProjection && (outcome != Presented || !worldProjection->IsValid()))
            return false;
        if (outcome == Presented)
            return reason == None;
        if (outcome == Skipped)
            return reason >= Suppressed && reason <= ViewReplaced;
        return outcome == Failed && reason >= ResourceUnavailable && reason <= ExecutionFailure;
    }

    /** @copydoc UiPresentedInteractionState::Create */
    Result<UiPresentedInteractionState> UiPresentedInteractionState::Create(const UiRenderViewId view, const UiCanvasInstanceId canvas) {
        if (!view.IsValid() || !canvas.IsValid())
            return Failure<UiPresentedInteractionState>(UiErrors::HandleMalformed);
        if (view.ownership != canvas.ownership)
            return Failure<UiPresentedInteractionState>(UiErrors::HandleOwnerMismatch);
        return Result<UiPresentedInteractionState>::Success(UiPresentedInteractionState(view, canvas));
    }

    /** @copydoc UiPresentedInteractionState::Apply */
    Result<bool> UiPresentedInteractionState::Apply(const UiPresentationReceipt &receipt) {
        if (!receipt.IsValid())
            return Failure<bool>(UiErrors::RenderPresentationInvalid);
        if (receipt.view != view_ || receipt.canvas != canvas_)
            return Failure<bool>(UiErrors::HandleOwnerMismatch);
        if (lastObservedSnapshot_.IsValid() && receipt.snapshotRevision.Compare(lastObservedSnapshot_) != UiRevisionRelation::Newer)
            return Failure<bool>(UiErrors::RenderPresentationStale);
        if (lastPresentedInteraction_.IsValid() &&
            receipt.interactionRevision.Compare(lastPresentedInteraction_) == UiRevisionRelation::Older)
            return Failure<bool>(UiErrors::RenderPresentationStale);

        lastObservedSnapshot_ = receipt.snapshotRevision;
        worldProjection_ = receipt.worldProjection;
        if (receipt.outcome != UiPresentationOutcome::Presented)
            return Result<bool>::Success(false);

        const bool advanced = !lastPresentedInteraction_.IsValid() ||
                              receipt.interactionRevision.Compare(lastPresentedInteraction_) == UiRevisionRelation::Newer;
        lastPresentedInteraction_ = receipt.interactionRevision;
        return Result<bool>::Success(advanced);
    }

    /** @copydoc UiPresentedInteractionState::LastObservedSnapshot */
    UiRenderSnapshotRevision UiPresentedInteractionState::LastObservedSnapshot() const noexcept {
        return lastObservedSnapshot_;
    }

    /** @copydoc UiPresentedInteractionState::LastPresentedInteraction */
    UiInteractionRevision UiPresentedInteractionState::LastPresentedInteraction() const noexcept {
        return lastPresentedInteraction_;
    }

    /** @copydoc UiPresentedInteractionState::View */
    UiRenderViewId UiPresentedInteractionState::View() const noexcept {
        return view_;
    }

    /** @copydoc UiPresentedInteractionState::Canvas */
    UiCanvasInstanceId UiPresentedInteractionState::Canvas() const noexcept {
        return canvas_;
    }

    UiPresentedInteractionState::UiPresentedInteractionState(const UiRenderViewId view, const UiCanvasInstanceId canvas) noexcept
        : view_(view), canvas_(canvas) {}
}  // namespace Horo::Runtime::Ui
