#include "RenderGraphResourceLeasePool.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderResourceOperations.h"

#include <algorithm>
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
            lease.transient = transient;
            if (transient != nullptr)
                transient->inFlight = true;
            for (const auto &resource : resources) {
                const auto *binding = &resource.binding;
                if (resource.resourceClass == RenderGraphResourceClass::Transient) {
                    if (transient == nullptr || resource.id.value == 0 || resource.id.value > transient->bindings.size()) {
                        lease.Release();
                        return Result<IRenderGraphResourceLease *>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                    }
                    const auto index = resource.id.value - 1U;
                    if (transient->lifetimes[index].disposition == RenderGraphLifetimeDisposition::Unused)
                        continue;
                    binding = &transient->bindings[index];
                }
                const auto resolved = ResolveGraphResidentIdentity(*binding);
                if (resolved.HasError()) {
                    lease.Release();
                    return Result<IRenderGraphResourceLease *>::Failure(resolved.ErrorValue());
                }
                const auto &pin = resolved.Value();
                if (std::any_of(lease.pins.begin(), lease.pins.end(), [&pin](const Pin &existing) {
                    return existing.resourceClass == pin.resourceClass && existing.identity == pin.identity;
                }))
                    continue;
                if (activePins_ == maximumPins_) {
                    lease.Release();
                    return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
                }
                if (const auto pinned = registry_->AddSubmissionPin(pin.resourceClass, pin.identity); pinned.HasError()) {
                    lease.Release();
                    return Result<IRenderGraphResourceLease *>::Failure(pinned.ErrorValue());
                }
                lease.pins.push_back(pin);
                ++activePins_;
            }
            if (ui != nullptr) {
                if (const auto captured = lease.ui.Capture(*ui, resources); captured.HasError()) {
                    lease.Release();
                    return Result<IRenderGraphResourceLease *>::Failure(captured.ErrorValue());
                }
            }
            return Result<IRenderGraphResourceLease *>::Success(&lease);
        }
        return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
    }
}  // namespace Horo::Render::Detail
