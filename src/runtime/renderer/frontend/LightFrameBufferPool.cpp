#include "Horo/Runtime/Render/LightFrameBufferPool.h"

#include <limits>
#include <new>

namespace Horo::Render {
    /** @copydoc LightFrameBufferPool::LightFrameBufferPool */
    LightFrameBufferPool::LightFrameBufferPool(RenderFrontend &frontend, const LightCullingBudget &budget, const std::uint32_t slots,
                                               ConstructionKey)
        : frontend_(&frontend), budget_(budget), count_(slots), owner_(std::this_thread::get_id()) {}

    LightFrameBufferPool::~LightFrameBufferPool() {
        static_cast<void>(Shutdown());
    }

    /** @copydoc LightFrameBufferPool::Create */
    Result<std::unique_ptr<LightFrameBufferPool>> LightFrameBufferPool::Create(RenderFrontend &frontend, const LightCullingBudget &budget,
                                                                               const std::uint32_t slots) {
        using Creation = Result<std::unique_ptr<LightFrameBufferPool>>;
        if (!budget.IsValid() || slots == 0 || slots > 8 || slots > frontend.Capabilities().support.limits.maxFramesInFlight)
            return Creation::Failure(MakeError(LightCullingErrors::Capacity));
        if (!frontend.Capabilities().support.features.Supports(RenderCapability::LightCulling))
            return Creation::Failure(MakeError(LightCullingErrors::Unsupported));
        try {
            auto pool = std::make_unique<LightFrameBufferPool>(frontend, budget, slots, ConstructionKey{});
            const std::array<std::size_t, 4> bytes{std::size_t{budget.maximumLights} * sizeof(PackedRenderLight),
                                                   std::size_t{budget.maximumClusters} * sizeof(PackedLightCluster),
                                                   std::size_t{budget.maximumClusters} * sizeof(PackedLightMembership),
                                                   std::size_t{budget.maximumClusters} * budget.referencesPerCluster *
                                                       sizeof(std::uint32_t)};
            for (std::size_t slot = 0; slot < slots; ++slot) {
                auto &buffers = pool->slots_[slot];
                const std::array destinations{&buffers.lights, &buffers.clusters, &buffers.membership, &buffers.references};
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    const auto buffer = frontend.CreateBuffer({.byteSize = bytes[index],
                                                               .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopySource,
                                                               .access = RenderBufferAccess::HostVisible},
                                                              {});
                    if (buffer.HasError())
                        return Creation::Failure(buffer.ErrorValue());
                    *destinations[index] = buffer.Value().handle;
                    pool->operations_[slot][index] = buffer.Value().operation;
                }
            }
            return Creation::Success(std::move(pool));
        } catch (const std::bad_alloc &) {
            return Creation::Failure(MakeError(LightCullingErrors::Capacity, "Light pool preparation allocation failed."));
        }
    }

    /** @copydoc LightFrameBufferPool::Update */
    Result<UploadedLightFrame> LightFrameBufferPool::Update(const std::uint32_t slot, const std::span<const PackedRenderLight> lights,
                                                            const std::span<const PackedLightCluster> clusters,
                                                            const CancellationToken &cancellation) {
        using Upload = Result<UploadedLightFrame>;
        if (closed_ || std::this_thread::get_id() != owner_ || slot >= count_)
            return Upload::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (revisions_[slot] == std::numeric_limits<std::uint64_t>::max())
            return Upload::Failure(MakeError(LightCullingErrors::Capacity));
        for (const auto operation : operations_[slot])
            if (const auto ready = frontend_->ResourceOperationResult(operation); ready.HasError())
                return Upload::Failure(ready.ErrorValue());
        const std::uint64_t revision = revisions_[slot] + 1;
        const LightFrameUpdate update{.budget = budget_,
                                      .revision = revision,
                                      .lights = lights,
                                      .clusters = clusters,
                                      .cancellation = cancellation};
        if (const auto uploaded = frontend_->UpdateLightFrame(slots_[slot], update); uploaded.HasError())
            return Upload::Failure(uploaded.ErrorValue());
        revisions_[slot] = revision;
        return Upload::Success(
            {slots_[slot],
             {static_cast<std::uint32_t>(lights.size()), static_cast<std::uint32_t>(clusters.size()), budget_.referencesPerCluster, 0},
             revision});
    }

    /** @copydoc LightFrameBufferPool::Shutdown */
    Result<void> LightFrameBufferPool::Shutdown() {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        closed_ = true;
        for (auto &buffers : slots_)
            for (auto *handle : {&buffers.lights, &buffers.clusters, &buffers.membership, &buffers.references}) {
                if (!handle->IsValid())
                    continue;
                if (const auto released = frontend_->ReleaseBuffer(*handle); released.HasError())
                    return released;
                *handle = {};
            }
        return Result<void>::Success();
    }
}  // namespace Horo::Render
