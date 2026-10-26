#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "RuntimeUiAccessState.h"
#include "UiAnimationOwnerInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc RuntimeUiService::Reload(RuntimeUiInstanceId, UiAnimationOwner &, UiAnimationReloadPreparation,
     * const CancellationToken &) */
    Result<UiAnimationReloadResult> RuntimeUiService::Reload(const RuntimeUiInstanceId instance, UiAnimationOwner &animation,
                                                             UiAnimationReloadPreparation preparation,
                                                             const CancellationToken &cancellation) const {
        if (const auto checked = Detail::CheckOwner(state_); checked.HasError())
            return Result<UiAnimationReloadResult>::Failure(checked.ErrorValue());
        if (!animation.storage_ || !animation.storage_->publisher.Matches(state_, instance) ||
            preparation.definition.canvas != animation.storage_->definition.canvas)
            return Result<UiAnimationReloadResult>::Failure(
                MakeError(UiErrors::CapabilityDenied, "Coordinated reload requires this service's exact live source reservation."));
        return animation.Reload(std::move(preparation.generation), preparation.allocator, std::move(preparation.registry),
                                std::move(preparation.styles), std::move(preparation.definition), preparation.admission, cancellation);
    }
}  // namespace Horo::Runtime::Ui
