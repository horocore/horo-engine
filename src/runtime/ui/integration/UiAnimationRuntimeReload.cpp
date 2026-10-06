#include "UiAnimationRuntimeInternal.h"

#include <new>

namespace Horo::Runtime {
    /** @copydoc UiAnimationRuntimeParticipant::Reload */
    Result<UiAnimationRuntimeReload> UiAnimationRuntimeParticipant::Reload(
        Ui::UiReloadGeneration replacement, Ui::UiElementSlotAllocator &allocator, Ui::RuntimeStyleRegistry registry,
        Ui::UiStyleResolver styles, Ui::UiAnimationCanvasDefinition definition, const Ui::UiAnimationReloadPolicy policy,
        const Ui::UiStructuralCommitPoint point, const CancellationToken &cancellation) {
        if (storage_->ownerThread != std::this_thread::get_id() || storage_->stopped || storage_->prepared ||
            storage_->source.BindingStatus() != RuntimeDispatchStatus::Valid)
            return Result<UiAnimationRuntimeReload>::Failure(MakeError(Ui::UiErrors::AnimationLifecycleUnavailable));
        std::shared_ptr<UiAnimationClockController::Storage> controls;
        try {
            controls = std::make_shared<UiAnimationClockController::Storage>();
        } catch (const std::bad_alloc &) {
            return Result<UiAnimationRuntimeReload>::Failure(MakeError(Ui::UiErrors::AnimationStorageExhausted));
        }
        controls->capacity = storage_->controls->capacity;
        auto result = storage_->owner.Reload(std::move(replacement), allocator, std::move(registry), std::move(styles),
                                             std::move(definition), policy, point, cancellation);
        if (result.HasError())
            return Result<UiAnimationRuntimeReload>::Failure(result.ErrorValue());
        const auto clocks = storage_->owner.ClockBindings();
        for (std::size_t index = static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview); index < Ui::UiTimeDomainCount; ++index) {
            controls->domains[index] = storage_->controls->domains[index];
            controls->domains[index].clock = clocks.domains[index].clock;
            controls->domains[index].pending = {};
            controls->domains[index].seek.reset();
            controls->domains[index].revision = 1;
        }
        storage_->controls->retired.store(true);
        storage_->controls = controls;
        storage_->binding = storage_->owner.SourceBinding();
        return Result<UiAnimationRuntimeReload>::Success({std::move(result).Value(), UiAnimationClockController{std::move(controls)}});
    }
}  // namespace Horo::Runtime
