#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Render {
    namespace {
        Result<void> InvalidRaster(const char *message) {
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph, message));
        }

        const RenderGraphResource *RasterResource(const RenderGraphExecutionRequest &request, RenderGraphResourceId id,
                                                  RenderGraphResourceKind kind) {
            if (id.owner != request.graph.Owner() || !id.value || id.value > request.graph.Resources().size())
                return nullptr;
            const auto &resource = request.graph.Resources()[id.value - 1];
            return resource.kind == kind ? &resource : nullptr;
        }

        bool RasterRegionFits(const RenderGraphMaterialRaster &raster) {
            if (!raster.region)
                return true;
            const auto &region = *raster.region;
            return region.width && region.height && region.x <= raster.extent.width && region.y <= raster.extent.height &&
                   region.width <= raster.extent.width - region.x && region.height <= raster.extent.height - region.y;
        }

        bool RasterEnvelopeValid(const RenderGraphExecutionPass &pass, const RenderGraphMaterialRaster &raster) {
            const bool clear =
                !raster.depthClear || (std::isfinite(*raster.depthClear) && *raster.depthClear >= 0 && *raster.depthClear <= 1);
            const std::array valid{pass.kind == RenderPassKind::Graphics,
                                   raster.extent.IsValid(),
                                   raster.draws.size() <= RenderGraphMaterialRaster::MaximumDraws,
                                   raster.reads.size() <= RenderGraphMaterialRaster::MaximumReads,
                                   pass.usages.count <= RenderGraphLimits::HardMaxResources,
                                   raster.colorOperations.IsValid(),
                                   clear,
                                   RasterRegionFits(raster)};
            return std::ranges::all_of(valid, [](bool value) {
                return value;
            });
        }

        struct RasterAdmission {
            const RenderGraphExecutionRequest &request;
            const RenderGraphMaterialRaster &raster;
            std::span<const RenderGraphResourceUsage> usages;
            std::array<bool, RenderGraphLimits::HardMaxResources> matched{}, readUsed{};

            Result<void> Use(RenderGraphResourceId id, RenderGraphUsageKind kind, RenderGraphAccess access) {
                const auto found = std::ranges::find(usages, id, &RenderGraphResourceUsage::resource);
                if (found == usages.end() || found->kind != kind || found->access != access)
                    return InvalidRaster("Material raster has a missing or incompatible declared graph use.");
                matched[static_cast<std::size_t>(found - usages.begin())] = true;
                return Result<void>::Success();
            }

            Result<void> Attachment(RenderGraphResourceId id, RenderTextureViewHandle view, RenderGraphUsageKind kind,
                                    RenderGraphAccess access) {
                if (!RasterResource(request, id, RenderGraphResourceKind::Texture) || !view.IsValid())
                    return InvalidRaster("Material raster requires exact graph texture and prepared view identities.");
                if (std::ranges::find(request.views, view, &RenderGraphTextureViewInstance::view) == request.views.end())
                    return InvalidRaster("Material raster attachment view was not resolved by the frontend.");
                return Use(id, kind, access);
            }

            Result<void> ColorOutput() {
                if (raster.depthOnly) {
                    if (raster.color.IsValid() || raster.colorView.IsValid() || !raster.depth.IsValid())
                        return InvalidRaster("Depth-only material raster cannot name a color attachment.");
                    return Result<void>::Success();
                }
                if (raster.colorOperations.outputColor || raster.colorOperations.loadOperation == AttachmentLoadOperation::DontCare ||
                    raster.colorOperations.storeOperation != AttachmentStoreOperation::Store)
                    return InvalidRaster("Material raster color must clear/load and store.");
                return Attachment(raster.color, raster.colorView, RenderGraphUsageKind::ColorAttachment,
                                  raster.colorOperations.loadOperation == AttachmentLoadOperation::Load ? RenderGraphAccess::ReadWrite
                                                                                                        : RenderGraphAccess::Write);
            }

            Result<void> Outputs() {
                if (auto color = ColorOutput(); color.HasError())
                    return color;
                if (raster.depth.IsValid())
                    return Attachment(raster.depth, raster.depthView, RenderGraphUsageKind::DepthStencilAttachment,
                                      raster.depthClear ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite);
                return raster.depthView.IsValid() ? InvalidRaster("An absent depth texture cannot have a view.") : Result<void>::Success();
            }

            Result<void> ReadShape(const RenderGraphMaterialRead &read) {
                if (read.buffer.IsValid() == read.texture.IsValid())
                    return InvalidRaster("A material graph input maps exactly one buffer or sampled view.");
                if (read.buffer.IsValid()) {
                    const auto *resource = RasterResource(request, read.resource, RenderGraphResourceKind::Buffer);
                    if (!resource)
                        return InvalidRaster("Material graph input buffer is foreign.");
                    const auto *buffer = std::get_if<RenderBufferHandle>(&resource->binding);
                    if (!buffer || *buffer != read.buffer)
                        return InvalidRaster("Material graph buffer mapping differs from its resident generation.");
                } else {
                    if (!RasterResource(request, read.resource, RenderGraphResourceKind::Texture) ||
                        std::ranges::find(request.views, read.texture, &RenderGraphTextureViewInstance::view) == request.views.end())
                        return InvalidRaster("Material sampled graph view was not resolved by the frontend.");
                }
                return Result<void>::Success();
            }

            Result<void> Input(const MaterialResourceBinding &logical, const ShaderReflectedBinding &binding) {
                if (binding.kind == ShaderResourceKind::Sampler)
                    return Result<void>::Success();
                if (binding.access != ShaderResourceAccess::ReadOnly)
                    return InvalidRaster("Material graph raster permits readonly inputs only.");
                const auto found = std::ranges::find_if(raster.reads, [&logical](const auto &read) {
                    if (const auto *buffer = std::get_if<MaterialBufferBinding>(&logical.value))
                        return read.buffer == buffer->buffer;
                    if (const auto *view = std::get_if<RenderTextureViewHandle>(&logical.value))
                        return read.texture == *view;
                    return false;
                });
                if (found == raster.reads.end())
                    return InvalidRaster(
                        "Every material input must name an exact graph generation; implicit parameter buffers are unsupported.");
                const auto index = static_cast<std::size_t>(found - raster.reads.begin());
                readUsed[index] = true;
                if (binding.kind == ShaderResourceKind::SampledTexture)
                    return Use(found->resource, RenderGraphUsageKind::Sampled, RenderGraphAccess::Read);
                if (binding.kind == ShaderResourceKind::UniformBuffer)
                    return Use(found->resource, RenderGraphUsageKind::UniformBuffer, RenderGraphAccess::Read);
                if (binding.kind == ShaderResourceKind::StorageBuffer)
                    return Use(found->resource, RenderGraphUsageKind::Storage, RenderGraphAccess::Read);
                return InvalidRaster("Material graph raster resource class is unsupported.");
            }

            Result<void> Draw(const RenderGraphMaterialDraw &draw) {
                if (!draw.material || !draw.mesh.IsValid() || draw.mesh.owner != draw.material->Id().owner)
                    return InvalidRaster("Material raster mesh/material owner identity is invalid.");
                if (std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh) == request.meshes.end())
                    return InvalidRaster("Material graph mesh was not resolved by the frontend.");
                if (!RasterResource(request, draw.vertices, RenderGraphResourceKind::Buffer) ||
                    !RasterResource(request, draw.indices, RenderGraphResourceKind::Buffer))
                    return InvalidRaster("Material graph geometry must declare its exact vertex/index buffers.");
                if (auto vertex = Use(draw.vertices, RenderGraphUsageKind::VertexBuffer, RenderGraphAccess::Read); vertex.HasError())
                    return vertex;
                if (auto index = Use(draw.indices, RenderGraphUsageKind::IndexBuffer, RenderGraphAccess::Read); index.HasError())
                    return index;
                return MaterialInputs(*draw.material);
            }

            Result<void> MaterialInputs(const ResidentMaterialBinding &material) {
                const auto &descriptor = material.Descriptor();
                for (const auto &logical : descriptor.resources) {
                    const auto binding = std::ranges::find(descriptor.layout.resources, logical.binding, &ShaderReflectedBinding::id);
                    if (binding == descriptor.layout.resources.end())
                        return InvalidRaster("Material descriptor has an unknown logical binding.");
                    if (auto read = Input(logical, *binding); read.HasError())
                        return read;
                }
                return Result<void>::Success();
            }
        };
    }  // namespace

    Result<void> ValidateMaterialRasterWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                const RenderGraphMaterialRaster &raster) {
        if (!RasterEnvelopeValid(pass, raster) || !pass.usages.IsValidFor(request.graph.Usages().size()))
            return InvalidRaster("Material raster pass or finite input envelope is invalid.");
        RasterAdmission admission{request, raster, request.graph.Usages().subspan(pass.usages.offset, pass.usages.count)};
        if (auto outputs = admission.Outputs(); outputs.HasError())
            return outputs;
        for (const auto &read : raster.reads)
            if (auto valid = admission.ReadShape(read); valid.HasError())
                return valid;
        for (const auto &draw : raster.draws)
            if (auto valid = admission.Draw(draw); valid.HasError())
                return valid;
        if (!std::ranges::all_of(std::span{admission.matched}.first(admission.usages.size()), [](bool value) {
            return value;
        }))
            return InvalidRaster("Material raster declares unconsumed graph uses.");
        if (!std::ranges::all_of(std::span{admission.readUsed}.first(raster.reads.size()), [](bool value) {
            return value;
        }))
            return InvalidRaster("Material raster declares unused graph input mappings.");
        return Result<void>::Success();
    }
}  // namespace Horo::Render
