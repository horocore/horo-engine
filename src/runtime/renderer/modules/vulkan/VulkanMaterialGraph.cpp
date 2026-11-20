#include "VulkanGraphRuntimeInternal.h"
#include "VulkanResourceConversions.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Resolves an exact graph texture view using the existing native resource registry. */
        Result<VulkanDrawViewBinding> ResolveGraphView(const RenderGraphExecutionRequest &request, const VulkanMaterialCapture &capture,
                                                       RenderTextureViewHandle view) {
            const auto found = std::ranges::find(request.views, view, &RenderGraphTextureViewInstance::view);
            if (found == request.views.end() || !found->instance)
                return Result<VulkanDrawViewBinding>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
            return capture.ResolveDrawView(found->instance);
        }

        /** @brief The material renderer uses single-sample 2D images and tracks contents at whole-image granularity. */
        bool SupportsAttachmentImage(const RenderTextureDescriptor &image, FramebufferExtent extent, bool depth,
                                     bool loadsExistingContents) noexcept {
            return image.extent == extent && VulkanResourceConversion::IsDepthFormat(image.format) == depth &&
                   image.dimension == RenderTextureDimension::TwoD && (image.layerCount == 1 || loadsExistingContents) &&
                   image.mipCount == 1 && image.sampleCount == 1 && HasTextureUsage(image.usage, RenderTextureUsage::RenderAttachment);
        }

        /** @brief A material pass accesses exactly one aspect-qualified, single-mip 2D layer. */
        bool MatchesAttachmentView(const VulkanGraphResource &texture, const VulkanDrawViewBinding &view, bool depth) noexcept {
            using enum RenderTextureFormat;
            using enum RenderTextureAspect;
            const auto &image = texture.textureDescriptor;
            const auto &subresource = view.descriptor;
            const bool stencil = image.format == Depth24Stencil8 || image.format == Depth32FloatStencil8;
            const auto depthAspect = stencil ? DepthStencil : Depth;
            // The native view registry already validates mip ranges. The admitted image has only one mip.
            return view.view && view.image == texture.image && subresource.aspect == (depth ? depthAspect : Color) &&
                   subresource.dimension == RenderTextureViewDimension::TwoD && subresource.format == image.format &&
                   subresource.baseLayer < image.layerCount && subresource.layerCount == 1;
        }

        /** @brief Captures the same graph attachment whose state is transitioned by compiled uses. */
        Result<VulkanMaterialTarget> CaptureGraphTarget(const RenderGraphExecutionRequest &request, const RenderGraphMaterialRaster &raster,
                                                        const VulkanMaterialCapture &capture,
                                                        std::span<const VulkanGraphResource> bindings) {
            using Outcome = Result<VulkanMaterialTarget>;
            VulkanMaterialTarget target{.extent = raster.extent};
            for (const bool depth : {false, true}) {
                const auto id = depth ? raster.depth : raster.color;
                if (!id.IsValid())
                    continue;
                const auto &texture = bindings[id.value - 1];
                const auto view = ResolveGraphView(request, capture, depth ? raster.depthView : raster.colorView);
                if (view.HasError())
                    return Outcome::Failure(view.ErrorValue());
                if (const bool loadsExistingContents =
                        depth ? !raster.depthClear.has_value() : raster.colorOperations.loadOperation == AttachmentLoadOperation::Load;
                    !SupportsAttachmentImage(texture.textureDescriptor, raster.extent, depth, loadsExistingContents) ||
                    !MatchesAttachmentView(texture, view.Value(), depth))
                    return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
                if (depth) {
                    target.depth = texture;
                    target.depthView = view.Value().view;
                } else {
                    target.color = texture;
                    target.colorView = view.Value().view;
                }
            }
            return Outcome::Success(target);
        }

        /** @brief Finds the exact graph read authorized by the public material's logical binding. */
        const RenderGraphMaterialRead *MatchingRead(const MaterialBindingDescriptor &material, const RenderGraphMaterialRaster &raster,
                                                    ShaderBindingId binding) {
            const auto entry = std::ranges::find(material.resources, binding, &MaterialResourceBinding::binding);
            if (entry == material.resources.end())
                return nullptr;
            const auto read = std::ranges::find_if(raster.reads, [&](const auto &input) {
                if (const auto *buffer = std::get_if<MaterialBufferBinding>(&entry->value))
                    return input.buffer == buffer->buffer;
                if (const auto *texture = std::get_if<RenderTextureViewHandle>(&entry->value))
                    return input.texture == *texture;
                return false;
            });
            return read == raster.reads.end() ? nullptr : std::addressof(*read);
        }

        /** @brief Verifies the retained descriptor backing has the same native generation as the graph transition. */
        bool MatchesRead(const RenderGraphMaterialRead &read, const RenderGraphExecutionRequest &request,
                         const VulkanMaterialCapture &capture, const VulkanMaterialResource &backing, const VulkanGraphResource &graph) {
            if (!backing.backing || !backing.state || backing.state != graph.state)
                return false;
            if (read.buffer.IsValid())
                return backing.buffer != VK_NULL_HANDLE && backing.buffer == graph.buffer;
            const auto view = ResolveGraphView(request, capture, read.texture);
            return view.HasValue() && backing.view && backing.view == view.Value().view && backing.image == graph.image &&
                   graph.image == view.Value().image;
        }
    }  // namespace

    /** @copydoc VulkanGraphRuntime::Impl::PrepareGraphMaterial */
    Result<void> VulkanGraphRuntime::Impl::PrepareGraphMaterial(const RenderGraphExecutionRequest &request, std::size_t pass,
                                                                const RenderGraphMaterialRaster &raster, VulkanCommandRetention &pins) {
        const auto *queue = Queue(request.graph.Passes()[pass].queue);
        if (!queue || !queue->supportsGraphics || !native.bindPipeline || !native.bindSets || !native.bindVertices || !native.bindIndex ||
            !native.viewport || !native.scissor || !native.drawIndexed)
            return Result<void>::Failure(Unsupported("Material graph raster requires the selected native graphics command family."));
        if (materialPassCount == materialPasses.size() || raster.draws.size() > materialDraws.size() - materialDrawCount)
            return Result<void>::Failure(Invalid("Material graph raster exceeds the native batch capacity."));
        const auto capture = resources->MaterialCapture();
        const auto target = CaptureGraphTarget(request, raster, capture, bindings);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        const auto firstDraw = materialDrawCount;
        for (const auto &entry : raster.draws) {
            auto &draw = materialDraws[materialDrawCount];
            const auto mesh = std::ranges::find(request.meshes, entry.mesh, &RenderGraphMeshInstance::mesh);
            if (mesh == request.meshes.end() || !mesh->instance)
                return Result<void>::Failure(Invalid("Material graph raster requires the exact resolved native mesh."));
            if (const auto captured = CaptureMaterialDraw({mesh->instance, &entry.material->BackendBinding().NativeBinding()}, draw,
                                                          target.Value(), raster.depthOnly, pins);
                captured.HasError())
                return captured;
            if (draw.mesh.vertex.state != bindings[entry.vertices.value - 1].state ||
                draw.mesh.index.state != bindings[entry.indices.value - 1].state)
                return Result<void>::Failure(Invalid("Material graph mesh inputs differ from the retained native mesh."));
            for (const auto &backing : draw.material->resources) {
                const auto *read = MatchingRead(entry.material->Descriptor(), raster, backing.binding);
                if (!read || !MatchesRead(*read, request, capture, backing, bindings[read->resource.value - 1]))
                    return Result<void>::Failure(Invalid("Material graph read differs from the retained native descriptor."));
            }
            ++materialDrawCount;
        }
        const RenderMaterialExecutionRequest recording{.extent = raster.extent,
                                                       .color = raster.colorOperations,
                                                       .depthClear = raster.depthClear,
                                                       .depthOnly = raster.depthOnly,
                                                       .region = raster.region};
        materialPasses[materialPassCount] = {target.Value(), recording, {firstDraw, raster.draws.size()}, {}};
        passes[pass].materialRaster = materialPassCount++;
        if (raster.color.IsValid())
            states[raster.color.value - 1].contentsDefined = true;
        if (raster.depth.IsValid())
            states[raster.depth.value - 1].contentsDefined = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
