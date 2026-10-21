#include "Horo/Runtime/Render/MultiviewGraphWorkload.h"

#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Render {
    namespace {
        /** @brief Fixed scratch describing each exact graph resource's single semantic role in this pass. */
        class Uses final {
        public:
            explicit Uses(const CompiledRenderGraphExecution &graph) noexcept : graph_(graph) {}

            [[nodiscard]] bool Add(const RenderGraphResourceId id, const RenderGraphResourceKind kind,
                                   const RenderGraphUsageKind role) noexcept {
                if (id.owner != graph_.Owner() || id.value == 0 || id.value > graph_.Resources().size() ||
                    graph_.Resources()[id.value - 1].kind != kind)
                    return false;
                auto &entry = entries_[id.value - 1];
                if (entry.present)
                    return entry.role == role;
                entry = {true, role};
                ++count_;
                return true;
            }

            [[nodiscard]] bool Matches(const std::span<const RenderGraphResourceUsage> usages) const noexcept {
                if (usages.size() != count_)
                    return false;
                for (const auto &use : usages) {
                    if (use.resource.owner != graph_.Owner() || use.resource.value == 0 || use.resource.value > graph_.Resources().size())
                        return false;
                    const auto &entry = entries_[use.resource.value - 1];
                    const bool attachment =
                        entry.role == RenderGraphUsageKind::ColorAttachment || entry.role == RenderGraphUsageKind::DepthStencilAttachment;
                    if (!entry.present || entry.role != use.kind ||
                        use.access != (attachment ? RenderGraphAccess::Write : RenderGraphAccess::Read))
                        return false;
                }
                return true;
            }

        private:
            struct Entry {
                bool present{};
                RenderGraphUsageKind role{};
            };

            const CompiledRenderGraphExecution &graph_;
            std::array<Entry, RenderGraphLimits::HardMaxResources> entries_{};
            std::size_t count_{};
        };

        [[nodiscard]] Result<void> Invalid() {
            return Result<void>::Failure(MakeError(MultiviewExtractionErrors::InvalidInput));
        }

        /** @brief Requires one complete ordered target set with no aliased view subresources. */
        [[nodiscard]] bool AddTargets(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &workload, Uses &uses) {
            for (std::size_t index = 0; index < workload.targets.size(); ++index) {
                const auto &target = workload.targets[index];
                if (!target.depth.IsValid() && target.depth != RenderGraphResourceId{})
                    return false;
                const auto colorView = std::ranges::find(request.views, target.colorView, &RenderGraphTextureViewInstance::view);
                if (!target.colorView.IsValid() || colorView == request.views.end() || colorView->instance == 0 ||
                    !uses.Add(target.color, RenderGraphResourceKind::Texture, RenderGraphUsageKind::ColorAttachment) ||
                    target.rectangle.width == 0 || target.rectangle.height == 0)
                    return false;
                if (target.depth.IsValid() &&
                    !uses.Add(target.depth, RenderGraphResourceKind::Texture, RenderGraphUsageKind::DepthStencilAttachment))
                    return false;
                if (target.depth.IsValid()) {
                    const auto depthView = std::ranges::find(request.views, target.depthView, &RenderGraphTextureViewInstance::view);
                    if (!target.depthView.IsValid() || depthView == request.views.end() || depthView->instance == 0)
                        return false;
                } else if (target.depthView.IsValid()) {
                    return false;
                }
                for (const auto &prior : workload.targets.first(index))
                    if ((prior.color == target.color && prior.arrayLayer == target.arrayLayer) ||
                        (target.depth.IsValid() && prior.depth == target.depth && prior.depthArrayLayer == target.depthArrayLayer))
                        return false;
            }
            return true;
        }

        /** @brief Matches the explicit uniform dependency to the material's actual logical buffer generation. */
        [[nodiscard]] bool ParameterBindingMatches(const CompiledRenderGraphExecution &graph, const MultiviewGraphGeometry &geometry,
                                                   const std::size_t viewCount) {
            const auto &descriptor = geometry.material->Descriptor();
            if (descriptor.resources.size() != 1 || MultiviewShaderParameterBytes(descriptor.layout, viewCount).HasError())
                return false;
            const auto &resource = descriptor.resources.front();
            const auto *uniform = std::get_if<MaterialBufferBinding>(&resource.value);
            if (!uniform || resource.binding != MultiviewShaderParameterIds::Buffer || resource.arrayElement != 0)
                return false;
            const auto &binding = graph.Resources()[geometry.parameters.value - 1].binding;
            const auto *handle = std::get_if<RenderBufferHandle>(&binding);
            return handle && *handle == uniform->buffer;
        }

        /** @brief Resolves each visible source exactly once, preserving canonical draw order and exact resident generations. */
        [[nodiscard]] bool AddGeometry(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &workload, Uses &uses) {
            for (std::size_t index = 0; index < workload.geometry.size(); ++index) {
                const auto &geometry = workload.geometry[index];
                if (geometry.instanceIndex != workload.scene->Draws()[index].instanceIndex || !geometry.material ||
                    !uses.Add(geometry.vertices, RenderGraphResourceKind::Buffer, RenderGraphUsageKind::VertexBuffer) ||
                    !uses.Add(geometry.indices, RenderGraphResourceKind::Buffer, RenderGraphUsageKind::IndexBuffer) ||
                    !uses.Add(geometry.parameters, RenderGraphResourceKind::Buffer, RenderGraphUsageKind::UniformBuffer))
                    return false;
                const auto &source = workload.scene->Instances()[geometry.instanceIndex];
                if (geometry.material->Id() != source.material ||
                    !ParameterBindingMatches(request.graph, geometry, workload.targets.size()))
                    return false;
                const auto resolved = std::ranges::find(request.meshes, source.mesh, &RenderGraphMeshInstance::mesh);
                if (resolved == request.meshes.end() || resolved->instance == 0)
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc MultiviewRenderRectangle::Fits */
    bool MultiviewRenderRectangle::Fits(const FramebufferExtent allocation) const noexcept {
        return allocation.IsValid() && width != 0 && height != 0 && x <= allocation.width && y <= allocation.height &&
               width <= allocation.width - x && height <= allocation.height - y;
    }

    /** @copydoc ValidateMultiviewGraphWorkload */
    Result<void> ValidateMultiviewGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                const RenderGraphMultiview &workload) {
        if (!workload.scene || pass.kind != RenderPassKind::Graphics || !workload.clearColor.IsFinite() ||
            !std::isfinite(workload.depthClear) || workload.depthClear < 0.0F || workload.depthClear > 1.0F ||
            workload.targets.size() != workload.scene->Views().size() || workload.geometry.size() != workload.scene->Draws().size() ||
            request.graph.Resources().size() > RenderGraphLimits::HardMaxResources ||
            request.meshes.size() > MultiviewExtractionLimits::HardMaximumInstances ||
            request.views.size() > MultiviewExtractionLimits::HardMaximumViews * 2 || !workload.limits.IsValid())
            return Invalid();
        if (const auto live = workload.scene->Validate(workload.active); live.HasError())
            return live;
        if (!pass.usages.IsValidFor(request.graph.Usages().size()))
            return Invalid();
        Uses uses{request.graph};
        if (!AddTargets(request, workload, uses) || !AddGeometry(request, workload, uses) ||
            !uses.Matches(request.graph.Usages().subspan(pass.usages.offset, pass.usages.count)))
            return Invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
