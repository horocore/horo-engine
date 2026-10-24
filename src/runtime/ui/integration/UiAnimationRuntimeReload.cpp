#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "UiAnimationRuntimeInternal.h"

#include <new>

namespace Horo::Runtime {
    /** @copydoc UiAnimationRuntimeParticipant::Reload */
    Result<UiAnimationRuntimeReload> UiAnimationRuntimeParticipant::Reload(Ui::UiReloadGeneration replacement,
                                                                           Ui::UiElementSlotAllocator &allocator,
                                                                           Ui::RuntimeStyleRegistry registry, Ui::UiStyleResolver styles,
                                                                           Ui::UiAnimationCanvasDefinition definition,
                                                                           const Ui::UiAnimationReloadAdmission &admission,
                                                                           const CancellationToken &cancellation) {
        return ReloadSource(nullptr, {},
                            {std::move(replacement), allocator, std::move(registry), std::move(styles), std::move(definition), admission},
                            cancellation);
    }

    /** @copydoc UiAnimationRuntimeParticipant::Reload(Ui::RuntimeUiService &, Ui::RuntimeUiInstanceId,
     * Ui::UiAnimationReloadPreparation, const CancellationToken &) */
    Result<UiAnimationRuntimeReload> UiAnimationRuntimeParticipant::Reload(Ui::RuntimeUiService &service,
                                                                           const Ui::RuntimeUiInstanceId instance,
                                                                           Ui::UiAnimationReloadPreparation preparation,
                                                                           const CancellationToken &cancellation) {
        return ReloadSource(&service, instance, std::move(preparation), cancellation);
    }

    /** @copydoc UiAnimationRuntimeParticipant::ReloadSource */
    Result<UiAnimationRuntimeReload> UiAnimationRuntimeParticipant::ReloadSource(Ui::RuntimeUiService *service,
                                                                                 const Ui::RuntimeUiInstanceId instance,
                                                                                 Ui::UiAnimationReloadPreparation preparation,
                                                                                 const CancellationToken &cancellation) {
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
        auto result = service ? service->Reload(instance, storage_->owner, std::move(preparation), cancellation)
                              : storage_->owner.Reload(std::move(preparation.generation), preparation.allocator,
                                                       std::move(preparation.registry), std::move(preparation.styles),
                                                       std::move(preparation.definition), preparation.admission, cancellation);
        if (result.HasError())
            return Result<UiAnimationRuntimeReload>::Failure(result.ErrorValue());
        const auto clocks = storage_->owner.ClockBindings();
        for (auto index = static_cast<std::size_t>(Ui::UiTimeDomain::EditorPreview); index < Ui::UiTimeDomainCount; ++index) {
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
