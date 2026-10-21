#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "VulkanMultiviewRuntime.h"
#include "VulkanRenderBackendErrors.h"
#include "VulkanShaderRuntime.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Horo::Render::Detail {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Invalid(const char *reason) {
            return Result<T>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid, reason));
        }

        [[nodiscard]] Result<void> Unsupported(const char *reason) {
            return Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported, reason));
        }

        /** @brief Returns only a structurally in-range exact native graph binding. */
        [[nodiscard]] const VulkanGraphResource *Resource(const RenderGraphExecutionRequest &request,
                                                          const std::span<const VulkanGraphResource> resources,
                                                          const RenderGraphResourceId id) noexcept {
            return id.owner == request.graph.Owner() && id.value != 0 && id.value <= request.resources.size() &&
                           id.value <= resources.size()
                       ? &resources[id.value - 1]
                       : nullptr;
        }
    }  // namespace

    /** @copydoc VulkanMultiviewRecorder::Attachment */
    Result<VulkanDrawViewBinding> VulkanMultiviewRecorder::Attachment(const RenderGraphExecutionRequest &request,
                                                                      const VulkanResourceRuntime &registry,
                                                                      const VulkanGraphResource &texture,
                                                                      const RenderTextureViewHandle view, const std::uint32_t layer,
                                                                      const RenderTextureAspect aspect) const {
        const auto found = std::ranges::find(request.views, view, &RenderGraphTextureViewInstance::view);
        if (found == request.views.end() || found->instance == 0)
            return Invalid<VulkanDrawViewBinding>("Exact frontend-resolved multiview attachment view is missing.");
        const auto actual = registry.ResolveDrawView(found->instance);
        if (actual.HasError())
            return actual;
        const auto &descriptor = actual.Value().descriptor;
        if (actual.Value().image != texture.image || actual.Value().view == VK_NULL_HANDLE || descriptor.baseLayer != layer ||
            descriptor.layerCount != 1 || descriptor.baseMip != 0 || descriptor.mipCount != 1 ||
            descriptor.dimension != RenderTextureViewDimension::TwoD || descriptor.format != texture.textureDescriptor.format ||
            descriptor.aspect != aspect)
            return Invalid<VulkanDrawViewBinding>("Attachment view differs from the exact graph image/layer/format/aspect.");
        return actual;
    }

    /** @copydoc VulkanMultiviewRecorder::PrepareTargets */
    Result<void> VulkanMultiviewRecorder::PrepareTargets(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &workload,
                                                         const std::span<const VulkanGraphResource> resources,
                                                         const VulkanResourceRuntime &registry) {
        for (std::size_t index = 0; index < workload.targets.size(); ++index) {
            const auto &input = workload.targets[index];
            const auto *color = Resource(request, resources, input.color);
            if (!color || color->kind != RenderGraphResourceKind::Texture || color->colorView == VK_NULL_HANDLE ||
                color->textureDescriptor.sampleCount != 1 || !input.rectangle.Fits(color->textureDescriptor.extent) ||
                input.arrayLayer >= color->textureDescriptor.layerCount ||
                input.rectangle.x > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
                input.rectangle.y > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
                return Invalid<void>("Multiview color image, active rectangle, sample count or array layer is invalid.");
            const auto colorView = Attachment(request, registry, *color, input.colorView, input.arrayLayer, RenderTextureAspect::Color);
            if (colorView.HasError())
                return Result<void>::Failure(colorView.ErrorValue());
            auto &target = targets_[index];
            target = {};
            target.color = colorView.Value().view;
            target.colorArray = color->colorView;
            target.colorImage = color->image;
            target.colorDescriptor = color->textureDescriptor;
            target.rectangle = input.rectangle;
            target.layer = input.arrayLayer;
            if (input.depth.IsValid()) {
                const auto *depth = Resource(request, resources, input.depth);
                if (!depth || depth->kind != RenderGraphResourceKind::Texture || depth->colorView == VK_NULL_HANDLE ||
                    depth->textureDescriptor.extent != color->textureDescriptor.extent || depth->textureDescriptor.sampleCount != 1 ||
                    input.depthArrayLayer >= depth->textureDescriptor.layerCount ||
                    (depth->textureDescriptor.format != RenderTextureFormat::Depth16Unorm &&
                     depth->textureDescriptor.format != RenderTextureFormat::Depth32Float))
                    return Unsupported("Multiview depth requires matching single-sample extent/layers and an admitted depth-only format.");
                const auto depthView =
                    Attachment(request, registry, *depth, input.depthView, input.depthArrayLayer, RenderTextureAspect::Depth);
                if (depthView.HasError())
                    return Result<void>::Failure(depthView.ErrorValue());
                target.depth = depthView.Value().view;
                target.depthArray = depth->colorView;
                target.depthImage = depth->image;
                target.depthDescriptor = depth->textureDescriptor;
                target.depthLayer = input.depthArrayLayer;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanMultiviewRecorder::CanRouteLayers */
    bool VulkanMultiviewRecorder::CanRouteLayers() const noexcept {
        const auto &first = targets_[0];
        if (!vertexLayerEnabled_ || first.colorDescriptor.layerCount != targetCount_)
            return false;
        std::uint32_t layers = 0;
        for (std::uint32_t index = 0; index < targetCount_; ++index) {
            const auto &target = targets_[index];
            if (target.colorImage != first.colorImage || target.colorArray != first.colorArray || target.depthImage != first.depthImage ||
                target.depthArray != first.depthArray || target.rectangle != first.rectangle || target.layer >= targetCount_ ||
                (target.depth && (target.depthLayer != target.layer || target.depthDescriptor.layerCount != targetCount_)) ||
                (layers & (std::uint32_t{1} << target.layer)) != 0)
                return false;
            layers |= std::uint32_t{1} << target.layer;
        }
        return true;
    }

    /** @copydoc VulkanMultiviewRecorder::PrepareDraws */
    Result<void> VulkanMultiviewRecorder::PrepareDraws(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &workload,
                                                       const std::span<const VulkanGraphResource> resources,
                                                       VulkanResourceRuntime &registry, VulkanCommandRetention &retention) {
        auto remaining = workload.limits.maximumExpandedIndices;
        for (std::size_t index = 0; index < workload.geometry.size(); ++index) {
            const auto &input = workload.geometry[index];
            if (input.instanceIndex >= workload.scene->Instances().size() ||
                input.instanceIndex != workload.scene->Draws()[index].instanceIndex)
                return Invalid<void>("Multiview geometry source index differs from its canonical visible draw.");
            const auto mesh = workload.scene->Instances()[input.instanceIndex].mesh;
            const auto mapped = std::ranges::find(request.meshes, mesh, &RenderGraphMeshInstance::mesh);
            if (mapped == request.meshes.end() || !input.material)
                return Invalid<void>("Exact multiview mesh/material generation mapping is missing.");
            const auto actual = registry.CaptureMaterialMesh(mapped->instance, retention);
            if (actual.HasError())
                return Result<void>::Failure(actual.ErrorValue());
            const auto *vertex = Resource(request, resources, input.vertices);
            const auto *indices = Resource(request, resources, input.indices);
            if (!vertex || !indices || vertex->buffer != actual.Value().vertex.buffer || indices->buffer != actual.Value().index.buffer)
                return Invalid<void>("Canonical graph vertex/index dependencies differ from the exact resident mesh.");
            const auto &descriptor = actual.Value().descriptor;
            const auto indexBytes = descriptor.indexFormat == RenderIndexFormat::UInt16 ? 2U : 4U;
            if (!descriptor.IsValid() || descriptor.vertexStride != sizeof(MeshVertex) ||
                static_cast<std::uint64_t>(descriptor.vertexCount) * descriptor.vertexStride >
                    actual.Value().vertex.bufferDescriptor.byteSize ||
                static_cast<std::uint64_t>(descriptor.indexCount) * indexBytes > actual.Value().index.bufferDescriptor.byteSize)
                return Invalid<void>("Multiview mesh exceeds its exact immutable vertex/index backing.");
            const auto *material = dynamic_cast<const VulkanPreparedMaterialBinding *>(&input.material->BackendBinding().NativeBinding());
            if (!material || material->device != device_ || material->deviceIncarnation != deviceIncarnation_ || !material->owner ||
                !material->pipeline || !material->layout || material->sets.empty() || material->sets.size() > 4)
                return Invalid<void>("Multiview draw requires a live same-device Vulkan material descriptor lease.");
            auto &draw = draws_[index];
            draw = {actual.Value(), material->pipeline, material->layout,
                    material->sets, material->state,    workload.scene->Draws()[index].visibleViews};
            const auto required =
                mode_ == MultiviewExecutionMode::Instanced ? RenderViewRouting::VertexLayer : RenderViewRouting::SingleView;
            if (draw.state.viewRouting != required || draw.state.sampleCount != 1)
                return Unsupported("Cooked shader routing or samples differ from the admitted multiview execution mode.");
            for (std::uint32_t view = 0; view < targetCount_; ++view) {
                const auto &target = targets_[view];
                const auto depth = target.depth ? std::optional{target.depthDescriptor.format} : std::nullopt;
                if (draw.state.colorFormat != target.colorDescriptor.format || draw.state.depthFormat != depth)
                    return Invalid<void>("Cooked native pipeline attachment formats differ from the exact XR target tuple.");
            }
            const auto expanded = static_cast<std::uint64_t>(std::popcount(draw.views));
            const auto count = static_cast<std::uint64_t>(draw.mesh.descriptor.indexCount);
            if (count == 0 || count > workload.limits.maximumIndicesPerDraw || expanded > remaining / count)
                return Result<void>::Failure(MakeError(MultiviewExtractionErrors::CapacityExceeded));
            remaining -= count * expanded;
        }
        return Result<void>::Success();
    }

    /** @copydoc VulkanMultiviewRecorder::Prepare */
    Result<void> VulkanMultiviewRecorder::Prepare(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &workload,
                                                  const std::span<const VulkanGraphResource> resources, VulkanResourceRuntime &registry,
                                                  VulkanCommandRetention &retention) {
        Reset();
        if (!available_)
            return Unsupported("Selected Vulkan device has no complete indexed multiview command dispatch.");
        if (!workload.scene || !workload.limits.IsValid() || workload.targets.empty() || workload.targets.size() > targets_.size() ||
            workload.geometry.size() > draws_.size() || workload.targets.size() != workload.scene->Views().size() ||
            workload.geometry.size() != workload.scene->Draws().size())
            return Invalid<void>("Multiview native workload or work capacities are malformed.");
        if (workload.active.deviceIncarnation != deviceIncarnation_)
            return Result<void>::Failure(MakeError(MultiviewExtractionErrors::StalePublication));
        if (const auto current = workload.scene->Validate(workload.active); current.HasError())
            return current;
        if (std::ranges::any_of(workload.scene->Views(), [](const auto &view) {
            return view.depthRange != Math::ClipDepthRange::ZeroToOne;
        }))
            return Unsupported("Vulkan multiview requires the canonical zero-to-one clip-depth projection.");
        mode_ = workload.scene->Mode();
        targetCount_ = static_cast<std::uint32_t>(workload.targets.size());
        drawCount_ = static_cast<std::uint32_t>(workload.geometry.size());
        clearColor_ = workload.clearColor;
        depthClear_ = workload.depthClear;
        if (const auto targets = PrepareTargets(request, workload, resources, registry); targets.HasError())
            return targets;
        if (mode_ == MultiviewExecutionMode::Instanced && !CanRouteLayers())
            return Unsupported("Instanced stereo needs enabled vertex-layer output and one complete compatible layered color/depth tuple.");
        if (const auto draws = PrepareDraws(request, workload, resources, registry, retention); draws.HasError())
            return draws;
        prepared_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
