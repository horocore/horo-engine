#include "RenderGraphResourceLeasePool.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceOperations.h"

#include <algorithm>

namespace Horo::Render::Detail {
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
        active = false;
    }

    /** @copydoc RenderGraphResourceLeasePool::Acquire */
    Result<IRenderGraphResourceLease *> RenderGraphResourceLeasePool::Acquire(const std::span<const RenderGraphResource> resources) {
        if (resources.size() > RenderGraphLimits::HardMaxResources || resources.size() > maximumPins_ - activePins_) {
            return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        }
        for (auto &lease : leases_) {
            if (lease.active) {
                continue;
            }
            lease.active = true;
            for (const auto &resource : resources) {
                Pin pin{};
                if (const auto *buffer = std::get_if<RenderBufferHandle>(&resource.binding)) {
                    pin = {RenderResourceClass::Buffer, Identity(*buffer)};
                } else if (const auto *texture = std::get_if<RenderTextureHandle>(&resource.binding)) {
                    pin = {RenderResourceClass::Texture, Identity(*texture)};
                } else {
                    lease.Release();
                    return Result<IRenderGraphResourceLease *>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                }
                if (const auto pinned = registry_->AddSubmissionPin(pin.resourceClass, pin.identity); pinned.HasError()) {
                    lease.Release();
                    return Result<IRenderGraphResourceLease *>::Failure(pinned.ErrorValue());
                }
                lease.pins.push_back(pin);
                ++activePins_;
            }
            return Result<IRenderGraphResourceLease *>::Success(&lease);
        }
        return Result<IRenderGraphResourceLease *>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
    }
}  // namespace Horo::Render::Detail
