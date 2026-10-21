#include "RenderMultiviewResources.h"

#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "RenderResourceRegistry.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Resolves one unique exact registry identity without claiming any native synchronization. */
        template <typename Handle, typename Entry, std::size_t Capacity>
        [[nodiscard]] Result<void> Resolve(const Handle handle, const RenderResourceClass kind, std::array<Entry, Capacity> &entries,
                                           std::size_t &count, const RenderResourceRegistry &registry) {
            const auto used = std::span{entries}.first(count);
            if (std::ranges::any_of(used, [handle](const Entry &entry) {
                if constexpr (requires { entry.mesh; })
                    return entry.mesh == handle;
                else
                    return entry.view == handle;
            }))
                return Result<void>::Success();
            if (count == entries.size())
                return Result<void>::Failure(MakeError(MultiviewExtractionErrors::CapacityExceeded));
            const auto instance = registry.BackendInstance(kind, {handle.owner, handle.slot, handle.generation});
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            entries[count++] = {handle, instance.Value()};
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ResolveScene(const RenderGraphMultiview &scene, ResolvedMultiviewResources &output,
                                                const RenderResourceRegistry &registry) {
            if (!scene.scene || scene.geometry.size() > MultiviewExtractionLimits::HardMaximumInstances ||
                scene.targets.size() > MultiviewExtractionLimits::HardMaximumViews)
                return Result<void>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
            if (const auto live = scene.scene->Validate(scene.active); live.HasError())
                return live;
            for (const auto &geometry : scene.geometry) {
                if (geometry.instanceIndex >= scene.scene->Instances().size())
                    return Result<void>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
                const auto mesh = scene.scene->Instances()[geometry.instanceIndex].mesh;
                if (const auto resolved = Resolve(mesh, RenderResourceClass::Mesh, output.meshes, output.meshCount, registry);
                    resolved.HasError())
                    return resolved;
            }
            for (const auto &target : scene.targets) {
                if (const auto resolved =
                        Resolve(target.colorView, RenderResourceClass::TextureView, output.views, output.viewCount, registry);
                    resolved.HasError())
                    return resolved;
                if (target.depth.IsValid()) {
                    if (const auto resolved =
                            Resolve(target.depthView, RenderResourceClass::TextureView, output.views, output.viewCount, registry);
                        resolved.HasError())
                        return resolved;
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ResolveMultiviewResources */
    Result<ResolvedMultiviewResources> ResolveMultiviewResources(const std::span<const RenderGraphPassWorkload> workloads,
                                                                 const RenderResourceRegistry &registry) {
        ResolvedMultiviewResources output;
        std::size_t scenes = 0;
        for (const auto &entry : workloads) {
            if (const auto *scene = std::get_if<RenderGraphMultiview>(&entry.workload)) {
                if (++scenes > 1)
                    return Result<ResolvedMultiviewResources>::Failure(MakeError(MultiviewExtractionErrors::CapacityExceeded));
                if (const auto resolved = ResolveScene(*scene, output, registry); resolved.HasError())
                    return Result<ResolvedMultiviewResources>::Failure(resolved.ErrorValue());
            }
        }
        return Result<ResolvedMultiviewResources>::Success(std::move(output));
    }
}  // namespace Horo::Render::Detail
