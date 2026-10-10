#include "UiSceneReconciliationInternal.h"

#include <new>

namespace Horo::Runtime::Ui {
    using UiSceneDetail::Failure;

    /** @copydoc UiSceneReconciliation::Prepared::Storage::PrepareRetirement */
    Horo::Result<void> UiSceneReconciliation::Prepared::Storage::PrepareRetirement(const std::size_t slot) {
        auto source = owner->active[slot]->publisher.Acquire();
        if (source.HasError())
            return Horo::Result<void>::Failure(source.ErrorValue());
        auto stamps = UiReloadDetail::CaptureSource(*source.Value().Get());
        if (stamps.HasError())
            return Horo::Result<void>::Failure(stamps.ErrorValue());
        retiring.emplace_back(slot, std::move(source).Value(), std::move(stamps).Value());
        return Horo::Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Prepared::Storage::PrepareRebind */
    Horo::Result<void> UiSceneReconciliation::Prepared::Storage::PrepareRebind(const std::size_t slot,
                                                                               UiSceneBindingReplacement &replacement) {
        for (const auto &canvas : replacement.generation.Canvases())
            if (canvas.bindings)
                result.requiredUnavailable += canvas.bindings->Current().requiredUnavailable;
        auto candidate = owner->active[slot]->publisher.PrepareSceneRebind(std::move(replacement.generation), !request.next, cancellation);
        if (candidate.HasError())
            return Horo::Result<void>::Failure(candidate.ErrorValue());
        rebinding.push_back({slot, std::move(candidate).Value()});
        return Horo::Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Prepared::Storage::PrepareActive */
    Horo::Result<void> UiSceneReconciliation::Prepared::Storage::PrepareActive(std::vector<UiSceneBindingReplacement> &bindings) {
        for (std::size_t slot = 0; slot < owner->active.size(); ++slot) {
            if (!owner->active[slot])
                continue;
            auto &entry = *owner->active[slot];
            const auto *current = entry.publisher.Current();
            if (!current)
                return Failure(UiErrors::InstanceStateInvalid);
            if (entry.descriptor.owner.scene == request.previous) {
                if (const auto valid = PrepareRetirement(slot); valid.HasError())
                    return valid;
                continue;
            }
            const auto replacement = std::ranges::find(bindings, entry.descriptor.instance, &UiSceneBindingReplacement::instance);
            // Reattachment after unload is explicitly named by the host, never inferred from a current scene.
            const bool detachedSources =
                !entry.descriptor.providerScene && std::ranges::any_of(current->Canvases(), [](const UiReloadCanvas &canvas) {
                return canvas.bindings && canvas.bindings->HasSceneProviders();
            });
            if (const bool detachedReattach = detachedSources && request.next && replacement != bindings.end();
                entry.descriptor.providerScene != request.previous && !detachedReattach)
                continue;
            if (replacement == bindings.end())
                return Failure(UiErrors::BindingProviderUnknown);
            if (const auto valid = PrepareRebind(slot, *replacement); valid.HasError())
                return valid;
            bindings.erase(replacement);
        }
        return bindings.empty() ? Horo::Result<void>::Success() : Failure(UiErrors::BindingDescriptorConflict);
    }

    /** @copydoc UiSceneReconciliation::Prepared::Storage::ValidateCapacity */
    Horo::Result<void> UiSceneReconciliation::Prepared::Storage::ValidateCapacity(const std::size_t incomingCount) const {
        if (const auto freeActive = std::ranges::count_if(owner->active,
                                                          [](const auto &entry) {
            return !entry;
        });
            incomingCount > static_cast<std::size_t>(freeActive) + retiring.size() ||
            retiring.size() + owner->limits.maximumInstances > owner->FreeRetired())
            return Failure(UiErrors::CapacityExceeded);
        return Horo::Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Prepared::Storage::PrepareIncoming */
    Horo::Result<void> UiSceneReconciliation::Prepared::Storage::PrepareIncoming(UiSceneActivation &activation) {
        if (!request.next || activation.descriptor.owner.kind != UiOwnerScopeKind::Scene ||
            activation.descriptor.owner.scene != request.next)
            return Failure(UiErrors::HandleOwnerMismatch);
        if (const auto issued = owner->Issue(activation.descriptor); issued.HasError())
            return issued;
        auto publisher = UiHotReload::Create(std::move(activation.generation));
        if (publisher.HasError())
            return Horo::Result<void>::Failure(publisher.ErrorValue());
        const auto lease = publisher.Value().Acquire();
        if (lease.HasError())
            return Horo::Result<void>::Failure(lease.ErrorValue());
        if (const auto valid = owner->Validate(activation.descriptor, *lease.Value().Get()); valid.HasError())
            return valid;
        incoming.emplace_back(activation.descriptor, std::move(publisher).Value());
        return Horo::Result<void>::Success();
    }

    /** @copydoc UiSceneReconciliation::Prepare */
    Result<UiSceneReconciliation::Prepared> UiSceneReconciliation::Prepare(const UiSceneTransitionRequest &request,
                                                                           std::vector<UiSceneBindingReplacement> bindings,
                                                                           std::vector<UiSceneActivation> incoming,
                                                                           const CancellationToken &cancellation) {
        if (!storage_ || storage_.Get()->stopped || storage_.Get()->collecting)
            return Failure<Prepared>(UiErrors::InstanceStateInvalid);
        if (!request.previous.IsValid() || (request.next && (!request.next->IsValid() || request.next == request.previous)))
            return Failure<Prepared>(UiErrors::IdentityInvalid);
        if (bindings.size() > storage_.Get()->limits.maximumInstances || incoming.size() > storage_.Get()->limits.maximumInstances ||
            storage_.Get()->preparedCount == storage_.Get()->limits.maximumPreparedTransitions)
            return Failure<Prepared>(UiErrors::CapacityExceeded);
        if (cancellation.IsCancellationRequested())
            return Failure<Prepared>(UiErrors::AssetLoadCancelled);
        try {
            auto prepared = std::make_unique<Prepared::Storage>();
            prepared->owner = storage_.OwnerPin();
            prepared->request = request;
            prepared->revision = storage_.Get()->revision;
            prepared->cancellation = cancellation;
            prepared->rebinding.reserve(bindings.size());
            prepared->incoming.reserve(incoming.size());
            prepared->retiring.reserve(storage_.Get()->limits.maximumInstances);
            if (const auto valid = prepared->PrepareActive(bindings); valid.HasError())
                return Result<Prepared>::Failure(valid.ErrorValue());
            if (const auto valid = prepared->ValidateCapacity(incoming.size()); valid.HasError())
                return Result<Prepared>::Failure(valid.ErrorValue());
            for (auto &activation : incoming)
                if (const auto valid = prepared->PrepareIncoming(activation); valid.HasError())
                    return Result<Prepared>::Failure(valid.ErrorValue());
            prepared->result.retiredSceneInstances = static_cast<std::uint32_t>(prepared->retiring.size());
            prepared->result.reboundPersistentInstances = static_cast<std::uint32_t>(prepared->rebinding.size());
            prepared->result.activatedSceneInstances = static_cast<std::uint32_t>(prepared->incoming.size());
            ++storage_.Get()->preparedCount;
            prepared->admitted = true;
            return Result<Prepared>::Success(Prepared{std::move(prepared)});
        } catch (const std::bad_alloc &) {
            return Failure<Prepared>(UiErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Runtime::Ui
