#include "VulkanUiPaintCommands.h"

#include "VulkanResourceConversions.h"
#include "VulkanShaderRuntimeInternal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        constexpr std::size_t DrawsPerFrame = UiPaintLimits::MaximumDraws;
        constexpr std::size_t FilterCount = static_cast<std::size_t>(RenderSamplerFilter::Linear) + 1;
        constexpr std::size_t MipmapCount = static_cast<std::size_t>(UiPaintMipmap::Linear) + 1;

        /** @brief Translates an actual Vulkan construction error without dropping its cause. */
        Result<void> Made(const VkResult result, const char *operation) {
            return result == VK_SUCCESS ? Result<void>::Success()
                                        : Result<void>::Failure(VulkanShaderDetail::NativeShaderFailure(operation, result));
        }

        /** @brief Samples and attachments all refer to exact single-subresource two-dimensional texture views. */
        bool WholeImage(const VulkanGraphResource &image, const RenderTextureUsage usage) noexcept {
            const auto &descriptor = image.textureDescriptor;
            return image.kind == RenderGraphResourceKind::Texture && image.image && descriptor.dimension == RenderTextureDimension::TwoD &&
                   descriptor.depth == 1 && descriptor.layerCount == 1 && descriptor.mipCount == 1 && descriptor.sampleCount == 1 &&
                   HasTextureUsage(descriptor.usage, usage);
        }

        /** @brief Checks the portable packet against the accepted mapped uniform bytes before any native command. */
        bool UniformsMatch(const RenderGraphUiPaint &paint, const VulkanUiPaintBufferView &uniforms) {
            const auto size = paint.draws.size();
            const auto first = paint.firstParameter;
            constexpr auto stride = UiPaintFrameUpdate::ParameterStride;
            if (first > uniforms.publication.parameters || size > uniforms.publication.parameters - first ||
                (first + size) * stride > uniforms.bytes.size())
                return false;
            for (std::size_t index = 0; index < size; ++index) {
                UiPaintParameters values;
                std::memcpy(&values, uniforms.bytes.data() + (first + index) * stride, sizeof(values));
                const auto &draw = paint.draws[index];
                if (values.extent != std::array{static_cast<float>(paint.extent.width), static_cast<float>(paint.extent.height)} ||
                    values.opacity != draw.opacity || values.sampling != static_cast<std::uint32_t>(draw.sampling) ||
                    values.flags != (draw.coverage != NoUiPaintTexture ? 1U : 0U) || values.maskExtent != draw.maskExtent ||
                    values.cornerRadii != draw.cornerRadii)
                    return false;
            }
            return true;
        }

        /** @brief Resolves one complete immutable publication for all three graph buffers. */
        bool ExactUpload(const RenderGraphUiPaint &paint, const VulkanUiPaintResources &bindings) {
            const std::array ids{paint.vertices.value - 1, paint.indices.value - 1, paint.parameters.value - 1};
            const std::array instances{bindings.identities[ids[0]].instance, bindings.identities[ids[1]].instance,
                                       bindings.identities[ids[2]].instance};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto id = ids[index];
                if (!bindings.resources[id].buffer || !bindings.states[id].contentsDefined)
                    return false;
                const auto buffer = bindings.owner.ReadUiPaintBuffer(instances[index]);
                if (buffer.HasError())
                    return false;
                const auto &publication = buffer.Value().publication;
                if (publication.members != instances || publication.revision != paint.frameRevision ||
                    publication.vertices != paint.vertexCount || publication.indices != paint.indexCount)
                    return false;
                if (index == 2 && !UniformsMatch(paint, buffer.Value()))
                    return false;
            }
            return true;
        }

        /** @brief Captures exact native targets and only defined, graph-declared, non-aliased color samples. */
        bool ExactImages(const RenderGraphUiPaint &paint, const VulkanUiPaintResources &bindings, const VulkanUiPaintKernel &kernel,
                         VulkanPreparedUiPaint &prepared) {
            using enum RenderTextureUsage;
            if (!paint.color.IsValid() || !paint.neutralSample.IsValid())
                return false;
            const auto &color = bindings.resources[paint.color.value - 1];
            if (!WholeImage(color, RenderAttachment) || !color.colorView || color.textureDescriptor.extent != paint.extent ||
                VulkanResourceConversion::TextureFormat(color.textureDescriptor.format) != kernel.ColorFormat() ||
                (!paint.clearColor && !bindings.states[paint.color.value - 1].contentsDefined))
                return false;
            prepared.color = color.colorView;
            VkImage stencilImage{};
            if (paint.stencil.IsValid()) {
                const auto &stencil = bindings.resources[paint.stencil.value - 1];
                if (!WholeImage(stencil, RenderAttachment) || !stencil.stencilView || stencil.textureDescriptor.extent != paint.extent ||
                    stencil.image == color.image ||
                    VulkanResourceConversion::TextureFormat(stencil.textureDescriptor.format) != kernel.StencilFormat() ||
                    (!paint.clearStencil && !bindings.states[paint.stencil.value - 1].contentsDefined))
                    return false;
                prepared.stencil = stencil.stencilView;
                stencilImage = stencil.image;
            }
            if (const auto &neutral = bindings.resources[paint.neutralSample.value - 1].textureDescriptor;
                neutral.extent != FramebufferExtent{1, 1} || neutral.format != RenderTextureFormat::Rgba8Unorm)
                return false;
            return std::ranges::all_of(paint.sampledResources, [&](const auto id) {
                const auto &sample = bindings.resources[id.value - 1];
                return WholeImage(sample, Sampled) && sample.colorView && sample.image != color.image && sample.image != stencilImage &&
                       !VulkanResourceConversion::IsDepthFormat(sample.textureDescriptor.format) &&
                       bindings.states[id.value - 1].contentsDefined;
            });
        }

        /** @brief Resolves an admitted sample table index or the explicit graph-owned neutral input. */
        const VulkanGraphResource &Sample(const RenderGraphUiPaint &paint, const VulkanUiPaintResources &resources,
                                          const std::uint32_t slot) noexcept {
            const auto graph = slot < paint.sampledResources.size() ? paint.sampledResources[slot] : paint.neutralSample;
            return resources.resources[graph.value - 1];
        }

        std::size_t SamplerIndex(const UiPaintDraw &draw) noexcept {
            return (static_cast<std::size_t>(draw.filter) * FilterCount + static_cast<std::size_t>(draw.magFilter)) * MipmapCount +
                   static_cast<std::size_t>(draw.mipmap);
        }
    }  // namespace

    VulkanUiPaintCommands::VulkanUiPaintCommands(const VulkanUiPaintDevice &device, const Dispatch native, FormatFeatures formats,
                                                 std::shared_ptr<VulkanUiPaintKernel> kernel) noexcept
        : device_(device.device), maximumExtent_{std::min(device.limits.maxFramebufferWidth, device.limits.maxViewportDimensions[0]),
                                                 std::min(device.limits.maxFramebufferHeight, device.limits.maxViewportDimensions[1])},
          native_(native), formats_(std::move(formats)), kernel_(std::move(kernel)) {}

    VulkanUiPaintCommands::~VulkanUiPaintCommands() {
        Revoke();
    }

    /** @copydoc VulkanUiPaintCommands::LoadDispatch */
    std::optional<VulkanUiPaintCommands::Dispatch> VulkanUiPaintCommands::LoadDispatch(const VkDevice device,
                                                                                       const VulkanShaderRuntime::Resolver &resolve) {
        Dispatch native;
        const auto load = [&](auto &member, const char *name) {
            using Proc = std::remove_reference_t<decltype(member)>;
            member = std::bit_cast<Proc>(resolve(device, name));
            return member != nullptr;
        };
        if (!(load(native.createPool, "vkCreateDescriptorPool") && load(native.destroyPool, "vkDestroyDescriptorPool") &&
              load(native.allocateSets, "vkAllocateDescriptorSets") && load(native.updateSets, "vkUpdateDescriptorSets") &&
              load(native.createSampler, "vkCreateSampler") && load(native.destroySampler, "vkDestroySampler") &&
              load(native.begin, "vkCmdBeginRendering") && load(native.end, "vkCmdEndRendering") &&
              load(native.bindPipeline, "vkCmdBindPipeline") && load(native.bindSet, "vkCmdBindDescriptorSets") &&
              load(native.bindVertices, "vkCmdBindVertexBuffers") && load(native.bindIndices, "vkCmdBindIndexBuffer") &&
              load(native.viewport, "vkCmdSetViewport") && load(native.scissor, "vkCmdSetScissor") &&
              load(native.reference, "vkCmdSetStencilReference") && load(native.draw, "vkCmdDrawIndexed")))
            return std::nullopt;
        return native;
    }

    /** @copydoc VulkanUiPaintCommands::Create */
    Result<std::shared_ptr<VulkanUiPaintCommands>> VulkanUiPaintCommands::Create(const VulkanUiPaintDevice &device,
                                                                                 const VulkanShaderRuntime::Resolver &resolve,
                                                                                 const CookedUiPaintKernel &cooked,
                                                                                 const FormatFeatures &formats, const std::size_t frames) {
        using Outcome = Result<std::shared_ptr<VulkanUiPaintCommands>>;
        if (!resolve || !device.device || frames == 0 || frames > 8)
            return Outcome::Failure(MakeError(UiPaintFrameErrors::Unavailable, "Vulkan UI command owner has no valid native frame slots."));
        const auto native = LoadDispatch(device.device, resolve);
        if (!native)
            return Outcome::Failure(
                MakeError(UiPaintFrameErrors::Unavailable, "Selected Vulkan UI descriptor, sampler or graphics dispatch is incomplete."));
        auto kernel = VulkanUiPaintKernel::Create(device, resolve, cooked);
        if (kernel.HasError())
            return Outcome::Failure(kernel.ErrorValue());
        try {
            auto owner =
                std::shared_ptr<VulkanUiPaintCommands>{new VulkanUiPaintCommands{device, *native, formats, std::move(kernel).Value()}};
            if (const auto descriptors = owner->PrepareDescriptors(frames); descriptors.HasError())
                return Outcome::Failure(descriptors.ErrorValue());
            if (const auto samplers = owner->PrepareSamplers(); samplers.HasError())
                return Outcome::Failure(samplers.ErrorValue());
            return Outcome::Success(std::move(owner));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCapacityExceeded));
        }
    }

    /** @copydoc VulkanUiPaintCommands::PrepareDescriptors */
    Result<void> VulkanUiPaintCommands::PrepareDescriptors(const std::size_t frames) {
        const auto count = static_cast<std::uint32_t>(frames * DrawsPerFrame);
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, count},
                               VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * count},
                               VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, count}};
        sets_.resize(count);
        const std::vector layouts(count, kernel_->SetLayout());
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = count,
                                              .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                                              .pPoolSizes = sizes.data()};
        if (const auto created = Made(native_.createPool(device_, &pool, nullptr, &pool_), "vkCreateDescriptorPool"); created.HasError())
            return created;
        const VkDescriptorSetAllocateInfo request{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                  .descriptorPool = pool_,
                                                  .descriptorSetCount = count,
                                                  .pSetLayouts = layouts.data()};
        return Made(native_.allocateSets(device_, &request, sets_.data()), "vkAllocateDescriptorSets");
    }

    /** @copydoc VulkanUiPaintCommands::PrepareSamplers */
    Result<void> VulkanUiPaintCommands::PrepareSamplers() {
        static_assert(FilterCount * FilterCount * MipmapCount == std::tuple_size_v<decltype(samplers_)>);
        constexpr std::array filters{VK_FILTER_NEAREST, VK_FILTER_LINEAR};
        for (std::size_t min = 0; min < filters.size(); ++min)
            for (std::size_t mag = 0; mag < filters.size(); ++mag)
                for (std::size_t mip = 0; mip < MipmapCount; ++mip) {
                    const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                                   .magFilter = filters[mag],
                                                   .minFilter = filters[min],
                                                   .mipmapMode = mip == static_cast<std::size_t>(UiPaintMipmap::Linear)
                                                                     ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                   .maxLod =
                                                       mip == static_cast<std::size_t>(UiPaintMipmap::None) ? 0.0F : VK_LOD_CLAMP_NONE};
                    auto &sampler = samplers_[(min * FilterCount + mag) * MipmapCount + mip];
                    if (const auto created = Made(native_.createSampler(device_, &info, nullptr, &sampler), "vkCreateSampler");
                        created.HasError())
                        return created;
                }
        return Result<void>::Success();
    }

    /** @copydoc VulkanUiPaintCommands::Matches */
    bool VulkanUiPaintCommands::Matches(const VkDevice device, const std::uint64_t incarnation) const noexcept {
        return device_ && device_ == device && kernel_->Matches(device, incarnation);
    }

    /** @copydoc VulkanUiPaintCommands::CanFilter */
    bool VulkanUiPaintCommands::CanFilter(const VulkanGraphResource &texture, const UiPaintDraw &draw) const noexcept {
        const auto index = static_cast<std::size_t>(texture.textureDescriptor.format);
        if (index >= formats_.size() || (formats_[index] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
            return false;
        const bool linear = draw.filter == RenderSamplerFilter::Linear || draw.magFilter == RenderSamplerFilter::Linear ||
                            draw.mipmap == UiPaintMipmap::Linear;
        return !linear || (formats_[index] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    }

    /** @copydoc VulkanUiPaintCommands::BindDraw */
    void VulkanUiPaintCommands::BindDraw(const VkDescriptorSet set, const VkBuffer uniforms, const VkImageView source,
                                         const VkImageView coverage, const VkSampler sampler) const noexcept {
        const auto &bind = kernel_->Bindings();
        const VkDescriptorBufferInfo buffer{uniforms, 0, sizeof(UiPaintParameters)};
        const VkDescriptorImageInfo paintInfo{VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo coverageInfo{VK_NULL_HANDLE, coverage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo samplerInfo{.sampler = sampler};
        const std::array writes{VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, bind[0], 0, 1,
                                                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &buffer, nullptr},
                                VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, bind[1], 0, 1,
                                                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &paintInfo, nullptr, nullptr},
                                VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, bind[2], 0, 1,
                                                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &coverageInfo, nullptr, nullptr},
                                VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, bind[3], 0, 1,
                                                     VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr, nullptr}};
        native_.updateSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    /** @copydoc VulkanUiPaintCommands::Prepare */
    Result<VulkanPreparedUiPaint> VulkanUiPaintCommands::Prepare(const RenderGraphUiPaint &paint, const VulkanUiPaintResources &bindings,
                                                                 const std::size_t frame, const std::size_t firstDraw) const {
        using Outcome = Result<VulkanPreparedUiPaint>;
        VulkanPreparedUiPaint prepared;
        if (!device_ || paint.extent.width > maximumExtent_.width || paint.extent.height > maximumExtent_.height ||
            frame >= sets_.size() / DrawsPerFrame || firstDraw > DrawsPerFrame || paint.draws.size() > DrawsPerFrame - firstDraw)
            return Outcome::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (!ExactUpload(paint, bindings) || !ExactImages(paint, bindings, *kernel_, prepared))
            return Outcome::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto validDraw = [&](const UiPaintDraw &draw) {
            const auto &source = Sample(paint, bindings, draw.sampled);
            const auto &coverage = Sample(paint, bindings, draw.coverage);
            return kernel_->Pipeline(draw.operation, prepared.stencil != VK_NULL_HANDLE) && CanFilter(source, draw) &&
                   (draw.coverage == NoUiPaintTexture || coverage.textureDescriptor.extent == paint.extent);
        };
        if (!std::ranges::all_of(paint.draws, validDraw))
            return Outcome::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        prepared.vertices = bindings.resources[paint.vertices.value - 1].buffer;
        prepared.indices = bindings.resources[paint.indices.value - 1].buffer;
        prepared.sets = std::span{sets_}.subspan(frame * DrawsPerFrame + firstDraw, paint.draws.size());
        const auto uniforms = bindings.resources[paint.parameters.value - 1].buffer;
        for (std::size_t index = 0; index < paint.draws.size(); ++index) {
            const auto &draw = paint.draws[index];
            BindDraw(prepared.sets[index], uniforms, Sample(paint, bindings, draw.sampled).colorView,
                     Sample(paint, bindings, draw.coverage).colorView, samplers_[SamplerIndex(draw)]);
        }
        return Outcome::Success(prepared);
    }

    /** @copydoc VulkanUiPaintCommands::EncodeDraw */
    void VulkanUiPaintCommands::EncodeDraw(const VkCommandBuffer command, const UiPaintDraw &draw, const VkDescriptorSet set,
                                           const std::uint32_t parameter, const bool hasStencil) const noexcept {
        if (!draw.scissor.width || !draw.scissor.height)
            return;
        const VkRect2D scissor{{static_cast<std::int32_t>(draw.scissor.x), static_cast<std::int32_t>(draw.scissor.y)},
                               {draw.scissor.width, draw.scissor.height}};
        const auto offset = parameter * static_cast<std::uint32_t>(UiPaintFrameUpdate::ParameterStride);
        native_.bindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, kernel_->Pipeline(draw.operation, hasStencil));
        native_.reference(command, VK_STENCIL_FACE_FRONT_AND_BACK, draw.stencilDepth);
        native_.scissor(command, 0, 1, &scissor);
        native_.bindSet(command, VK_PIPELINE_BIND_POINT_GRAPHICS, kernel_->Layout(), 0, 1, &set, 1, &offset);
        native_.draw(command, draw.indexCount, 1, draw.firstIndex, 0, 0);
    }

    /** @copydoc VulkanUiPaintCommands::Encode */
    void VulkanUiPaintCommands::Encode(const VkCommandBuffer command, const RenderGraphUiPaint &paint,
                                       const VulkanPreparedUiPaint &prepared) const noexcept {
        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = prepared.color,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = paint.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingAttachmentInfo stencil{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                .imageView = prepared.stencil,
                                                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                .loadOp = paint.clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingInfo info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                   .renderArea = {{0, 0}, {paint.extent.width, paint.extent.height}},
                                   .layerCount = 1,
                                   .colorAttachmentCount = 1,
                                   .pColorAttachments = &color,
                                   .pStencilAttachment = prepared.stencil ? &stencil : nullptr};
        native_.begin(command, &info);
        if (!paint.draws.empty()) {
            const auto height = static_cast<float>(paint.extent.height);
            const VkViewport viewport{0.0F, height, static_cast<float>(paint.extent.width), -height, 0.0F, 1.0F};
            const VkDeviceSize zero = 0;
            native_.viewport(command, 0, 1, &viewport);
            native_.bindVertices(command, 0, 1, &prepared.vertices, &zero);
            native_.bindIndices(command, prepared.indices, 0, VK_INDEX_TYPE_UINT32);
            for (std::uint32_t index = 0; index < paint.draws.size(); ++index)
                EncodeDraw(command, paint.draws[index], prepared.sets[index], paint.firstParameter + index,
                           prepared.stencil != VK_NULL_HANDLE);
        }
        native_.end(command);
    }

    /** @copydoc VulkanUiPaintCommands::Revoke */
    void VulkanUiPaintCommands::Revoke() noexcept {
        if (!device_)
            return;
        if (pool_)
            native_.destroyPool(device_, std::exchange(pool_, VK_NULL_HANDLE), nullptr);
        for (auto &sampler : samplers_)
            if (sampler)
                native_.destroySampler(device_, std::exchange(sampler, VK_NULL_HANDLE), nullptr);
        kernel_->Revoke();
        device_ = VK_NULL_HANDLE;
    }
}  // namespace Horo::Render::Detail
