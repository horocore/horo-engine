#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "VfxSchedulingStorage.h"

#include <algorithm>

namespace Horo::Render {
    /** @copydoc RenderFrontend::ResolveVfxGraphResources */
    Result<void> RenderFrontend::ResolveVfxGraphResources(const std::span<const RenderGraphPassWorkload> workloads,
                                                          const std::span<const RenderGraphMeshInstance> existingMeshes,
                                                          std::span<const RenderGraphMeshInstance> &meshes,
                                                          std::span<const RenderGraphTargetInstance> &targets) {
        if (!vfxScheduling_ || !vfxScheduling_->resolvingGraph || existingMeshes.size() > vfxScheduling_->graphMeshes.capacity())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        auto &resolved = *vfxScheduling_;
        resolved.graphMeshes.clear();
        resolved.graphTargets.clear();
        resolved.graphMeshes.insert(resolved.graphMeshes.end(), existingMeshes.begin(), existingMeshes.end());
        for (const auto &entry : workloads) {
            const auto *vfx = std::get_if<RenderGraphVfx>(&entry.workload);
            if (!vfx)
                continue;
            if (!vfx->order || !vfx->order->nativePrepared)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            if (std::ranges::find(resolved.graphTargets, vfx->target, &RenderGraphTargetInstance::target) == resolved.graphTargets.end()) {
                if (resolved.graphTargets.size() == resolved.graphTargets.capacity())
                    return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
                const auto instance = BackendInstance(vfx->target);
                if (instance.HasError())
                    return Result<void>::Failure(instance.ErrorValue());
                resolved.graphTargets.push_back({vfx->target, instance.Value()});
            }
            for (const auto &draw : vfx->order->draws) {
                if (!draw.slot || draw.slot->ValidateDraw(*vfx, draw).HasError())
                    return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                if (std::ranges::find(resolved.graphMeshes, draw.mesh, &RenderGraphMeshInstance::mesh) != resolved.graphMeshes.end())
                    continue;
                if (resolved.graphMeshes.size() == resolved.graphMeshes.capacity())
                    return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
                const auto instance = BackendInstance(draw.mesh);
                if (instance.HasError())
                    return Result<void>::Failure(instance.ErrorValue());
                resolved.graphMeshes.push_back({draw.mesh, instance.Value()});
            }
        }
        meshes = resolved.graphMeshes;
        targets = resolved.graphTargets;
        return Result<void>::Success();
    }
}  // namespace Horo::Render
