#include "UiAnimationPublisher.h"

#include "RuntimeUiAccessState.h"
#include "UiSceneReconciliationInternal.h"

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
        if (!publisher)
            return Admission::Failure(MakeError(UiErrors::CapabilityUnavailable));
        return AttachPublisher(std::move(service), instance, canvas, *publisher);
    }

    /** @copydoc UiAnimationPublisher::AttachPrepared */
    Result<UiAnimationPublisher> UiAnimationPublisher::AttachPrepared(std::shared_ptr<RuntimeUiServiceState> service,
                                                                      UiSceneReconciliation::Prepared &prepared,
                                                                      const RuntimeUiInstanceId instance, const UiCanvasId canvas) {
        using Admission = Result<UiAnimationPublisher>;
        if (const auto valid = CheckOwner(service); valid.HasError())
            return Admission::Failure(valid.ErrorValue());
        const auto candidate = prepared.storage_;
        if (!candidate || candidate->consumed || candidate->abandoned || !service->registry.storage_.Matches(candidate->owner) ||
            candidate->revision != candidate->owner->revision)
            return Admission::Failure(MakeError(UiErrors::RevisionStale));
        const auto entry = std::ranges::find(candidate->incoming, instance, [](const auto &value) {
            return value.descriptor.instance;
        });
        if (entry != candidate->incoming.end())
            return AttachPublisher(std::move(service), instance, canvas, entry->publisher, candidate);
        const auto replacement = std::ranges::find_if(candidate->rebinding, [&](const auto &row) {
            return candidate->owner->active[row.slot]->descriptor.instance == instance;
        });
        if (replacement == candidate->rebinding.end())
            return Admission::Failure(MakeError(UiErrors::HandleOwnerMismatch));
        auto &publisher = candidate->owner->active[replacement->slot]->publisher;
        auto source = publisher.AnimationReplacementLease(replacement->candidate);
        if (!source.Get() || std::ranges::find(source.Get()->Canvases(), canvas, &UiReloadCanvas::id) == source.Get()->Canvases().end())
            return Admission::Failure(MakeError(UiErrors::CapabilityUnavailable));
        return AttachPublisher(std::move(service), instance, canvas, publisher, candidate, std::move(source));
    }

    /** @copydoc UiAnimationPublisher::AttachPublisher */
    Result<UiAnimationPublisher> UiAnimationPublisher::AttachPublisher(std::shared_ptr<RuntimeUiServiceState> service,
                                                                       const RuntimeUiInstanceId instance, const UiCanvasId canvas,
                                                                       UiHotReload &publisher,
                                                                       std::shared_ptr<UiSceneReconciliation::Prepared::Storage> prepared,
                                                                       UiReloadLease source) {
        using Admission = Result<UiAnimationPublisher>;
        if (!source.Get()) {
            auto acquired = publisher.Acquire();
            if (acquired.HasError())
                return Admission::Failure(acquired.ErrorValue());
            source = std::move(acquired).Value();
        }
        const auto canvases = source.Get()->Canvases();
        if (std::ranges::find(canvases, canvas, &UiReloadCanvas::id) == canvases.end())
            return Admission::Failure(MakeError(UiErrors::CapabilityUnavailable));
        for (const auto &entry : service->animationSources) {
            const auto current = entry.lock();
            if (current && !current->retiring && current->instance == instance &&
                (!prepared || std::ranges::find(prepared->animationSources, current) == prepared->animationSources.end()))
                return Admission::Failure(MakeError(UiErrors::CapabilityBusy));
        }
        const auto slot = std::ranges::find_if(service->animationSources, [](const auto &entry) {
            return entry.expired();
        });
        if (slot == service->animationSources.end())
            return Admission::Failure(MakeError(UiErrors::CapacityExceeded));
        try {
            auto reservation =
                std::make_shared<UiAnimationSourceReservation>(UiAnimationSourceReservation{instance, canvas, std::move(source)});
            if (prepared) {
                prepared->preparedAnimations.push_back(reservation);
                reservation->unpublished = true;
            }
            *slot = reservation;
            UiAnimationPublisher result{std::move(service), std::move(reservation)};
            result.prepared_ = std::move(prepared);
            result.reservation_->preparation = result.prepared_;
            return Admission::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Admission::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiAnimationPublisher::CandidateGeneration */
    UiReloadGeneration *UiAnimationPublisher::CandidateGeneration() const noexcept {
        const auto prepared = prepared_.lock();
        if (!prepared || prepared->consumed || prepared->abandoned || prepared->cancellation.IsCancellationRequested() ||
            prepared->revision != prepared->owner->revision || !service_ || !service_->active || service_->shutdownRequested ||
            service_->scenePublication || service_->thread != std::this_thread::get_id() || !reservation_ || reservation_->retiring)
            return nullptr;
        const auto incoming = std::ranges::find(prepared->incoming, reservation_->instance, [](const auto &value) {
            return value.descriptor.instance;
        });
        if (incoming != prepared->incoming.end())
            return incoming->publisher.Current();
        const auto replacement = std::ranges::find_if(prepared->rebinding, [&](const auto &row) {
            return prepared->owner->active[row.slot]->descriptor.instance == reservation_->instance;
        });
        return replacement == prepared->rebinding.end()
                   ? nullptr
                   : prepared->owner->active[replacement->slot]->publisher.AnimationReplacement(replacement->candidate);
    }

    /** @copydoc UiAnimationPublisher::Resolve */
    UiHotReload *UiAnimationPublisher::Resolve() noexcept {
        if (standalone_)
            return &*standalone_;
        if (!service_ || !reservation_ || reservation_->unpublished || reservation_->retiring || !service_->active ||
            service_->shutdownRequested || service_->scenePublication || service_->thread != std::this_thread::get_id())
            return nullptr;
        auto *publisher = service_->registry.Publisher(reservation_->instance);
        return publisher && publisher->IsCurrent(reservation_->generation) ? publisher : nullptr;
    }

    /** @copydoc UiAnimationPublisher::Resolve */
    const UiHotReload *UiAnimationPublisher::Resolve() const noexcept {
        if (standalone_)
            return &*standalone_;
        if (!service_ || !reservation_ || reservation_->unpublished || reservation_->retiring || !service_->active ||
            service_->shutdownRequested || service_->scenePublication || service_->thread != std::this_thread::get_id())
            return nullptr;
        auto *publisher = service_->registry.Publisher(reservation_->instance);
        return publisher && publisher->IsCurrent(reservation_->generation) ? publisher : nullptr;
    }

    /** @copydoc UiAnimationPublisher::IsAvailable */
    bool UiAnimationPublisher::IsAvailable() const noexcept {
        if (reservation_ && reservation_->unpublished)
            return CandidateGeneration() != nullptr;
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
        if (reservation_ && reservation_->unpublished)
            return CandidateGeneration();
        auto *publisher = Resolve();
        return publisher ? publisher->Current() : nullptr;
    }

    /** @copydoc UiAnimationPublisher::Acquire */
    Result<UiReloadLease> UiAnimationPublisher::Acquire() const {
        if (reservation_ && reservation_->unpublished)
            return CandidateGeneration() ? Result<UiReloadLease>::Success(reservation_->generation)
                                         : Result<UiReloadLease>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto *publisher = Resolve();
        return publisher ? publisher->Acquire() : Result<UiReloadLease>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
    }

    /** @copydoc UiAnimationPublisher::IsCurrent */
    bool UiAnimationPublisher::IsCurrent(const UiReloadLease &lease) const noexcept {
        if (reservation_ && reservation_->unpublished)
            return CandidateGeneration() && lease.Get() == reservation_->generation.Get();
        const auto *publisher = Resolve();
        return publisher && publisher->IsCurrent(lease);
    }

    /** @copydoc UiAnimationPublisher::Prepare */
    Result<UiHotReload::Prepared> UiAnimationPublisher::Prepare(UiReloadGeneration replacement, const CancellationToken &cancellation) {
        if (reservation_ && reservation_->unpublished)
            return Result<UiHotReload::Prepared>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto *publisher = Resolve();
        return publisher ? publisher->Prepare(std::move(replacement), cancellation)
                         : Result<UiHotReload::Prepared>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
    }

    /** @copydoc UiAnimationPublisher::Commit */
    Result<UiReloadReconciliation> UiAnimationPublisher::Commit(UiHotReload::Prepared &prepared, const UiStructuralCommitPoint point) {
        if (reservation_ && reservation_->unpublished)
            return Result<UiReloadReconciliation>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
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
        if (reservation_ && reservation_->unpublished)
            return Result<bool>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
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
