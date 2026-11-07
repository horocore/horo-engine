#include "VfxGpuInventoryLease.h"

#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <exception>
#include <new>

namespace Horo::Render::Detail {
    /** @copydoc VfxGpuInventoryLease::VfxGpuInventoryLease */
    VfxGpuInventoryLease::VfxGpuInventoryLease(RenderResourceRegistry &registry, std::weak_ptr<const VfxFrontendState> authority) noexcept
        : registry_(&registry), authority_(std::move(authority)), owner_(std::this_thread::get_id()) {}

    /** @copydoc VfxGpuInventoryLease::Create */
    Result<std::shared_ptr<VfxGpuInventoryLease>> VfxGpuInventoryLease::Create(RenderResourceRegistry &registry,
                                                                               const std::span<const RenderBufferHandle> buffers,
                                                                               std::weak_ptr<const VfxFrontendState> authority) {
        using Lease = Result<std::shared_ptr<VfxGpuInventoryLease>>;
        if (authority.expired() || buffers.empty() || buffers.size() > RenderGraphLimits::HardMaxResources)
            return Lease::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        try {
            auto lease = std::shared_ptr<VfxGpuInventoryLease>(new VfxGpuInventoryLease{registry, std::move(authority)});
            lease->buffers_.reserve(buffers.size());
            for (const auto buffer : buffers)
                if (const auto pinned = lease->Pin(buffer); pinned.HasError())
                    return Lease::Failure(pinned.ErrorValue());
            return Lease::Success(std::move(lease));
        } catch (const std::bad_alloc &) {
            return Lease::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        }
    }

    /** @copydoc VfxGpuInventoryLease::Pin */
    Result<void> VfxGpuInventoryLease::Pin(const RenderBufferHandle buffer) {
        if (!buffer.IsValid() || buffer.owner != registry_->Owner() || std::ranges::find(buffers_, buffer) != buffers_.end())
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        const RenderResourceIdentity identity{buffer.owner, buffer.slot, buffer.generation};
        if (const auto pinned = registry_->AddSubmissionPin(RenderResourceClass::Buffer, identity); pinned.HasError())
            return pinned;
        buffers_.push_back(buffer);
        return Result<void>::Success();
    }

    /** @copydoc VfxGpuInventoryLease::~VfxGpuInventoryLease */
    VfxGpuInventoryLease::~VfxGpuInventoryLease() {
        Release();
    }

    /** @copydoc VfxGpuInventoryLease::Release */
    void VfxGpuInventoryLease::Release() noexcept {
        if (!active_)
            return;
        active_ = false;
        if (!authority_.expired()) {
            if (owner_ != std::this_thread::get_id())
                std::terminate();
            for (const auto buffer : buffers_)
                static_cast<void>(
                    registry_->ReleaseSubmissionPin(RenderResourceClass::Buffer, {buffer.owner, buffer.slot, buffer.generation}));
        }
        buffers_.clear();
    }

    /** @copydoc VfxGpuInventoryLease::RetainedPayloadBytes */
    std::size_t VfxGpuInventoryLease::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + buffers_.capacity() * sizeof(RenderBufferHandle);
    }
}  // namespace Horo::Render::Detail
