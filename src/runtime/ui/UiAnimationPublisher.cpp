#include "UiAnimationPublisher.h"

#include "RuntimeUiAccessState.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime::Ui::Detail {
    /** @copydoc HasAnimationSource */
    bool HasAnimationSource(const RuntimeUiServiceState &service, const RuntimeUiInstanceId instance) noexcept {
        return std::ranges::any_of(service.animationSources, [instance](const auto &entry) {
            const auto source = entry.lock();
            return source && source->instance == instance;
        });
    }

    /** @copydoc InvalidateRuntimeUiAttribution */
    void InvalidateRuntimeUiAttribution(RuntimeUiServiceState &service, const RuntimeUiInstanceId instance) noexcept {
        for (auto &entry : service.dispatchers)
            if (entry.instance == instance)
                entry = {};
        for (auto &entry : service.actions)
            if (entry.instance == instance)
                entry = {};
    }

    UiAnimationPublisher::UiAnimationPublisher(UiHotReload publisher) noexcept : standalone_(std::move(publisher)) {}

    UiAnimationPublisher::UiAnimationPublisher(std::shared_ptr<RuntimeUiServiceState> service,
                                               std::shared_ptr<UiAnimationSourceReservation> reservation) noexcept
        : service_(std::move(service)), reservation_(std::move(reservation)) {}

    /** @copydoc UiAnimationPublisher::Attach */
    Result<UiAnimationPublisher> UiAnimationPublisher::Attach(std::shared_ptr<RuntimeUiServiceState> service,
                                                              const RuntimeUiInstanceId instance, const UiCanvasId canvas) {
        using Admission = Result<UiAnimationPublisher>;
        if (const auto valid = CheckOwner(service); valid.HasError())
            return Admission::Failure(valid.ErrorValue());
        auto *publisher = service->registry.Publisher(instance);
        if (!publisher || !publisher->Current() || !publisher->Current()->Canvas(canvas))
            return Admission::Failure(MakeError(UiErrors::CapabilityUnavailable));
        if (HasAnimationSource(*service, instance))
            return Admission::Failure(MakeError(UiErrors::CapabilityBusy));
        const auto slot = std::ranges::find_if(service->animationSources, [](const auto &entry) {
            return entry.expired();
        });
        if (slot == service->animationSources.end())
            return Admission::Failure(MakeError(UiErrors::CapacityExceeded));
        try {
            auto acquired = publisher->Acquire();
            if (acquired.HasError())
                return Admission::Failure(acquired.ErrorValue());
            auto reservation =
                std::make_shared<UiAnimationSourceReservation>(UiAnimationSourceReservation{instance, canvas, std::move(acquired).Value()});
            *slot = reservation;
            return Admission::Success(UiAnimationPublisher{std::move(service), std::move(reservation)});
        } catch (const std::bad_alloc &) {
            return Admission::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiAnimationPublisher::Resolve */
    UiHotReload *UiAnimationPublisher::Resolve() noexcept {
        if (standalone_)
            return &*standalone_;
        if (!service_ || !reservation_ || reservation_->retiring || !service_->active || service_->shutdownRequested ||
            service_->thread != std::this_thread::get_id())
            return nullptr;
        auto *publisher = service_->registry.Publisher(reservation_->instance);
        return publisher && publisher->IsCurrent(reservation_->generation) ? publisher : nullptr;
    }

    /** @copydoc UiAnimationPublisher::Resolve */
    const UiHotReload *UiAnimationPublisher::Resolve() const noexcept {
        if (standalone_)
            return &*standalone_;
        if (!service_ || !reservation_ || reservation_->retiring || !service_->active || service_->shutdownRequested ||
            service_->thread != std::this_thread::get_id())
            return nullptr;
        auto *publisher = service_->registry.Publisher(reservation_->instance);
        return publisher && publisher->IsCurrent(reservation_->generation) ? publisher : nullptr;
    }

    /** @copydoc UiAnimationPublisher::IsAvailable */
    bool UiAnimationPublisher::IsAvailable() const noexcept {
        const auto *publisher = Resolve();
        return publisher && publisher->Acquire().HasValue();
    }

    /** @copydoc UiAnimationPublisher::Replacement */
    UiReloadGeneration *UiAnimationPublisher::Replacement(const UiHotReload::Prepared &prepared) noexcept {
        auto *publisher = Resolve();
        return publisher ? publisher->AnimationReplacement(prepared) : nullptr;
    }

    /** @copydoc UiAnimationPublisher::Current */
    UiReloadGeneration *UiAnimationPublisher::Current() noexcept {
        auto *publisher = Resolve();
        return publisher ? publisher->Current() : nullptr;
    }

    /** @copydoc UiAnimationPublisher::Acquire */
    Result<UiReloadLease> UiAnimationPublisher::Acquire() const {
        const auto *publisher = Resolve();
        return publisher ? publisher->Acquire() : Result<UiReloadLease>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
    }

    /** @copydoc UiAnimationPublisher::IsCurrent */
    bool UiAnimationPublisher::IsCurrent(const UiReloadLease &lease) const noexcept {
        const auto *publisher = Resolve();
        return publisher && publisher->IsCurrent(lease);
    }

    /** @copydoc UiAnimationPublisher::Prepare */
    Result<UiHotReload::Prepared> UiAnimationPublisher::Prepare(UiReloadGeneration replacement, const CancellationToken &cancellation) {
        auto *publisher = Resolve();
        return publisher ? publisher->Prepare(std::move(replacement), cancellation)
                         : Result<UiHotReload::Prepared>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
    }

    /** @copydoc UiAnimationPublisher::Commit */
    Result<UiReloadReconciliation> UiAnimationPublisher::Commit(UiHotReload::Prepared &prepared, const UiStructuralCommitPoint point) {
        auto *publisher = Resolve();
        if (!publisher)
            return Result<UiReloadReconciliation>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        // Pin the exact private candidate before commit. Transfer is non-fallible and no postcommit Acquire is needed.
        auto nextSource = service_ ? publisher->AnimationReplacementLease(prepared) : UiReloadLease{};
        if (service_ && !nextSource.Get())
            return Result<UiReloadReconciliation>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto committed = publisher->Commit(prepared, point);
        if (committed.HasValue() && service_) {
            reservation_->generation = std::move(nextSource);
            InvalidateRuntimeUiAttribution(*service_, reservation_->instance);
        }
        return committed;
    }

    /** @copydoc UiAnimationPublisher::ApplyPresentation */
    Result<bool> UiAnimationPublisher::ApplyPresentation(const UiCanvasId canvas, const UiPresentationReceipt &receipt) {
        auto *publisher = Resolve();
        return publisher ? publisher->ApplyPresentation(canvas, receipt)
                         : Result<bool>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
    }

    /** @copydoc UiAnimationPublisher::InputEligible */
    bool UiAnimationPublisher::InputEligible(const UiCanvasId canvas, const UiRenderViewId view) const noexcept {
        const auto *publisher = Resolve();
        return publisher && publisher->InputEligible(canvas, view);
    }

    /** @copydoc UiAnimationPublisher::Shutdown */
    void UiAnimationPublisher::Shutdown() noexcept {
        if (standalone_) {
            standalone_->Shutdown();
            return;
        }
        if (auto *generation = Current())
            if (auto *canvas = generation->Canvas(reservation_->canvas))
                canvas->presentations.clear();
        if (reservation_)
            reservation_->retiring = true;
    }

    /** @copydoc UiAnimationPublisher::CollectRetired */
    Result<std::size_t> UiAnimationPublisher::CollectRetired() {
        return standalone_ ? standalone_->CollectRetired() : Result<std::size_t>::Success(0);
    }

    /** @copydoc UiAnimationPublisher::CanReclaim */
    bool UiAnimationPublisher::CanReclaim() const noexcept {
        return standalone_ ? standalone_->CanReclaim() : !reservation_ || reservation_->retiring;
    }

    /** @copydoc UiAnimationPublisher::Matches */
    bool UiAnimationPublisher::Matches(const std::shared_ptr<RuntimeUiServiceState> &service,
                                       const RuntimeUiInstanceId instance) const noexcept {
        if (service_ != service || !reservation_ || reservation_->instance != instance || !Resolve())
            return false;
        return std::ranges::any_of(service_->animationSources, [this](const auto &entry) {
            return entry.lock() == reservation_;
        });
    }

}  // namespace Horo::Runtime::Ui::Detail
