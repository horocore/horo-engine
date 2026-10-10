#include "NullGraphWorkloads.h"

#include "NullRenderBackendErrors.h"

#include <type_traits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Resolves one exact graph-local identity without a linear instance search. */
        [[nodiscard]] std::uint64_t Instance(const RenderGraphResourceId resource, const RenderGraphExecutionRequest &request) {
            if (resource.owner != request.graph.Owner() || resource.value == 0 || resource.value > request.resources.size())
                return 0;
            const auto &resolved = request.resources[resource.value - 1U];
            return resolved.resource == resource ? resolved.instance : 0;
        }

        /** @brief Checks buffer flags and overflow-safe byte ranges against the actual resolved instances. */
        [[nodiscard]] bool ValidCopy(const NullGraphResources &resources, const RenderGraphExecutionRequest &request,
                                     const RenderGraphBufferCopy &copy) {
            const auto sourceId = Instance(copy.source, request);
            const auto destinationId = Instance(copy.destination, request);
            const auto source = resources.buffers.find(sourceId);
            const auto destination = resources.buffers.find(destinationId);
            return source != resources.buffers.end() && destination != resources.buffers.end() && sourceId != destinationId &&
                   HasBufferUsage(source->second.usage, RenderBufferUsage::CopySource) &&
                   HasBufferUsage(destination->second.usage, RenderBufferUsage::CopyDestination) &&
                   copy.sourceOffset <= source->second.byteSize && copy.byteCount <= source->second.byteSize - copy.sourceOffset &&
                   copy.destinationOffset <= destination->second.byteSize &&
                   copy.byteCount <= destination->second.byteSize - copy.destinationOffset;
        }

        /** @brief Admits implemented whole-color attachment operations without pretending to render pixels. */
        [[nodiscard]] bool ValidColor(const NullGraphResources &resources, const RenderGraphExecutionRequest &request,
                                      const RenderGraphColorAttachment &color) {
            const auto texture = resources.textures.find(Instance(color.texture, request));
            if (texture == resources.textures.end())
                return false;
            const auto &descriptor = texture->second;
            return descriptor.dimension == RenderTextureDimension::TwoD && descriptor.depth == 1 && descriptor.mipCount == 1 &&
                   descriptor.layerCount == 1 && descriptor.sampleCount == 1 &&
                   HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment) &&
                   descriptor.format == RenderTextureFormat::Rgba8Unorm;
        }
    }  // namespace

    /** @copydoc ValidateNullGraphWorkloads */
    Result<void> ValidateNullGraphWorkloads(const NullGraphResources &resources, const RenderGraphExecutionRequest &request) {
        if (!request.resources.empty() && request.lease == nullptr)
            return Result<void>::Failure(MakeError(NullBackendErrors::InvalidExecutionPlan));
        if (const auto portable = ValidateRenderGraphExecutionRequest(request); portable.HasError())
            return portable;
        for (std::size_t index = 0; index < request.resources.size(); ++index) {
            const auto instance = request.resources[index].instance;
            if (instance == 0)
                continue;  // Portable admission permits only unused transient declarations here.
            const auto kind = request.graph.Resources()[index].kind;
            if ((kind == RenderGraphResourceKind::Buffer && !resources.buffers.contains(instance)) ||
                (kind == RenderGraphResourceKind::Texture && !resources.textures.contains(instance)))
                return Result<void>::Failure(MakeError(NullBackendErrors::InvalidExecutionPlan));
        }
        for (const auto &binding : request.workloads) {
            const auto valid = std::visit([&request, &resources]<typename Workload>(const Workload &workload) {
                if constexpr (std::is_same_v<Workload, RenderGraphLightCulling>) {
                    return Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
                } else if constexpr (std::is_same_v<Workload, RenderGraphBufferCopy>) {
                    if (!ValidCopy(resources, request, workload))
                        return Result<void>::Failure(MakeError(NullBackendErrors::InvalidExecutionPlan));
                } else if constexpr (std::is_same_v<Workload, RenderGraphColorAttachment>) {
                    if (!ValidColor(resources, request, workload))
                        return Result<void>::Failure(MakeError(NullBackendErrors::UnsupportedResourceOperation));
                }
                return Result<void>::Success();
            }, binding.workload);
            if (valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
