#include "RenderGraphResourceLeasePool.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderResourceOperations.h"

#include <algorithm>
#include <memory>
#include <type_traits>

namespace Horo::Render::Detail {
    /** @copydoc ResolveGraphResidentIdentity */
    Result<RenderGraphResidentIdentity> ResolveGraphResidentIdentity(const RenderGraphResourceBinding &binding) {
        return std::visit([]<typename Handle>(const Handle &handle) -> Result<RenderGraphResidentIdentity> {
            if constexpr (std::is_same_v<Handle, RenderBufferHandle>) {
                return Result<RenderGraphResidentIdentity>::Success({RenderResourceClass::Buffer, Identity(handle)});
            } else if constexpr (std::is_same_v<Handle, RenderTextureHandle>) {
                return Result<RenderGraphResidentIdentity>::Success({RenderResourceClass::Texture, Identity(handle)});
            } else {
                return Result<RenderGraphResidentIdentity>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
            }
        }, binding);
    }

    /** @copydoc RenderGraphResourceLeasePool::RenderGraphResourceLeasePool */
    RenderGraphResourceLeasePool::RenderGraphResourceLeasePool(RenderResourceRegistry &registry, const std::size_t maximumPins)
        : registry_(&registry), maximumPins_(maximumPins) {
        for (auto &lease : leases_) {
            lease.pool = this;
            lease.pins.reserve(std::min(maximumPins, RenderGraphLimits::HardMaxResources));
        }
    }

    /** @copydoc IRenderGraphResourceLease::Release */
    void RenderGraphResourceLeasePool::Lease::Release() noexcept {
        if (!active) {
            return;
        }
        for (const auto &pin : pins) {
            static_cast<void>(pool->registry_->ReleaseSubmissionPin(pin.resourceClass, pin.identity));
        }
        pool->activePins_ -= pins.size();
        pins.clear();
        ui.Release();
        if (transient != nullptr) {
            transient->inFlight = false;
            transient = nullptr;
        }
        active = false;
    }

    /** @copydoc RenderGraphResourceLeasePool::Lease::PinResource */
    Result<void> RenderGraphResourceLeasePool::Lease::PinResource(const RenderGraphResource &resource) {
        const auto *binding = &resource.binding;
        if (resource.resourceClass == RenderGraphResourceClass::Transient) {
            if (transient == nullptr || resource.id.value == 0 || resource.id.value > transient->bindings.size()) {
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
            }
            const auto index = resource.id.value - 1U;
            if (transient->lifetimes[index].disposition == RenderGraphLifetimeDisposition::Unused)
                return Result<void>::Success();
            binding = &transient->bindings[index];
        }
        const auto resolved = ResolveGraphResidentIdentity(*binding);
        if (resolved.HasError()) {
            return Result<void>::Failure(resolved.ErrorValue());
        }
        const auto &pin = resolved.Value();
        if (std::any_of(pins.begin(), pins.end(), [&pin](const Pin &existing) {
            return existing.resourceClass == pin.resourceClass && existing.identity == pin.identity;
        }))
            return Result<void>::Success();
        if (pool->activePins_ == pool->maximumPins_) {
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        }
        if (const auto pinned = pool->registry_->AddSubmissionPin(pin.resourceClass, pin.identity); pinned.HasError()) {
            return Result<void>::Failure(pinned.ErrorValue());
        }
        pins.push_back(pin);
        ++pool->activePins_;
        return Result<void>::Success();
    }

    /** @copydoc RenderGraphResourceLeasePool::Acquire */
    Result<IRenderGraphResourceLease *> RenderGraphResourceLeasePool::Acquire(const std::span<const RenderGraphResource> resources,
                                                                              UiRenderSubmission *ui,
                                                                              RenderGraphTransientResourceSet *transient) {
        if (transient != nullptr && (transient->inFlight || transient->released))
            return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceNotReady));
        if (resources.size() > RenderGraphLimits::HardMaxResources) {
            return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        }
        for (auto &lease : leases_) {
            if (lease.active) {
                continue;
            }
            lease.active = true;
            const auto rollback = [](Lease *pending) {
                pending->Release();
            };
            std::unique_ptr<Lease, decltype(rollback)> transaction{&lease, rollback};
            lease.transient = transient;
            if (transient != nullptr)
                transient->inFlight = true;
            for (const auto &resource : resources) {
                if (const auto pinned = lease.PinResource(resource); pinned.HasError()) {
                    return Result<IRenderGraphResourceLease *>::Failure(pinned.ErrorValue());
                }
            }
            if (ui != nullptr) {
                if (const auto captured = lease.ui.Capture(*ui, resources); captured.HasError()) {
                    return Result<IRenderGraphResourceLease *>::Failure(captured.ErrorValue());
                }
            }
            return Result<IRenderGraphResourceLease *>::Success(transaction.release());
        }
        return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
    }
}  // namespace Horo::Render::Detail
