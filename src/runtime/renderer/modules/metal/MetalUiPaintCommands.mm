#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "MetalResourceRuntimeInternal.h"
#include "MetalUiPaintKernel.h"

namespace Horo::Render::Detail {
    namespace {
        struct PaintBindings final {
            const MetalUiPaintKernel *kernel{};
            std::array<MetalBufferInstance *, 3> buffers{};
            std::array<MetalTextureInstance *, 2> textures{};  // color, stencil
            std::array<MetalTextureInstance *, UiPaintLimits::MaximumSampledResources> sampled{};
            __strong id<MTLTexture> target{nil};
        };

        template <class Instance>
        Instance *Resolve(RenderGraphResourceId id, std::span<const RenderGraphResourceInstance> resources,
                          const std::unordered_set<Instance *> &residents) {
            for (const auto &resource : resources)
                if (resource.resource == id) {
                    auto *instance = Decode<Instance>(resource.instance);
                    return instance && residents.contains(instance) && !instance->use.retired ? instance : nullptr;
                }
            return nullptr;
        }

        /** @brief Admits only the exact complete reusable publication, never merely a large enough buffer. */
        bool BuffersMatch(const RenderGraphUiPaint &paint, const std::array<MetalBufferInstance *, 3> &buffers) {
            constexpr std::array usages{RenderBufferUsage::Vertex, RenderBufferUsage::Index, RenderBufferUsage::Uniform};
            const std::array<std::size_t, 3> bytes{std::size_t{paint.vertexCount} * sizeof(UiPaintVertex),
                                                   std::size_t{paint.indexCount} * sizeof(std::uint32_t),
                                                   (paint.firstParameter + paint.draws.size()) * UiPaintFrameUpdate::ParameterStride};
            for (std::size_t index = 0; index < buffers.size(); ++index) {
                const auto *buffer = buffers[index];
                if (!buffer || !HasBufferUsage(buffer->usage, usages[index]) || bytes[index] > buffer->buffer.length ||
                    buffer->uiPaintRevision != paint.frameRevision || buffer->uiPaintVertices != paint.vertexCount ||
                    buffer->uiPaintIndices != paint.indexCount || paint.firstParameter > buffer->uiPaintParameters ||
                    paint.draws.size() > buffer->uiPaintParameters - paint.firstParameter)
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (buffer == buffers[previous])
                        return false;
            }
            return true;
        }

        /** @brief Checks uploaded uniform bytes against this exact admitted draw program before native encoding. */
        bool ParametersMatch(const RenderGraphUiPaint &paint, const MetalBufferInstance &buffer) {
            const auto *bytes = static_cast<const std::byte *>(buffer.buffer.contents);
            if (!bytes)
                return false;
            for (std::size_t index = 0; index < paint.draws.size(); ++index) {
                const auto offset = (paint.firstParameter + index) * UiPaintFrameUpdate::ParameterStride;
                UiPaintParameters parameters;
                std::memcpy(&parameters, bytes + offset, sizeof(parameters));
                const auto &draw = paint.draws[index];
                if (parameters.extent != std::array{static_cast<float>(paint.extent.width), static_cast<float>(paint.extent.height)} ||
                    parameters.opacity != draw.opacity || parameters.sampling != static_cast<std::uint32_t>(draw.sampling) ||
                    parameters.flags != (draw.coverage != NoUiPaintTexture ? 1U : 0U) || parameters.maskExtent != draw.maskExtent ||
                    parameters.cornerRadii != draw.cornerRadii)
                    return false;
            }
            return true;
        }

        /** @brief Resolves native residency, attachment compatibility and source reads before any encoder is opened. */
        Result<PaintBindings> Bindings(const RenderGraphUiPaint &paint, std::span<const RenderGraphResourceInstance> resources,
                                       const std::unordered_set<MetalBufferInstance *> &buffers,
                                       const std::unordered_set<MetalTextureInstance *> &textures, id<MTLTexture> primary,
                                       const std::shared_ptr<const MetalUiPaintOwnerIdentity> &owner, std::uint64_t incarnation) {
            using Outcome = Result<PaintBindings>;
            PaintBindings result;
            result.kernel = dynamic_cast<const MetalUiPaintKernel *>(paint.kernel.get());
            if (!result.kernel || result.kernel->owner != owner || result.kernel->incarnation != incarnation || incarnation == 0)
                return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            const std::array bufferIds{paint.vertices, paint.indices, paint.parameters};
            for (std::size_t index = 0; index < bufferIds.size(); ++index)
                result.buffers[index] = Resolve(bufferIds[index], resources, buffers);
            if (!BuffersMatch(paint, result.buffers) || !ParametersMatch(paint, *result.buffers[2]))
                return Outcome::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            const std::array textureIds{paint.color, paint.stencil};
            for (std::size_t index = 0; index < textureIds.size(); ++index) {
                if (!textureIds[index].IsValid())
                    continue;
                result.textures[index] = Resolve(textureIds[index], resources, textures);
                if (!result.textures[index])
                    return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            }
            if (paint.sampledResources.size() > result.sampled.size())
                return Outcome::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            for (std::size_t index = 0; index < paint.sampledResources.size(); ++index) {
                result.sampled[index] = Resolve(paint.sampledResources[index], resources, textures);
                if (!result.sampled[index])
                    return Outcome::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            }
            for (const auto &draw : paint.draws) {
                const bool texture = draw.sampling == UiPaintSampling::Color || draw.sampling == UiPaintSampling::Alpha ||
                                     draw.sampling == UiPaintSampling::Premultiplied;
                if ((texture && draw.sampled >= paint.sampledResources.size()) ||
                    (draw.coverage != NoUiPaintTexture && draw.coverage >= paint.sampledResources.size()) ||
                    draw.operation > UiPaintOperation::MultiplyCoverage || draw.filter > RenderSamplerFilter::Linear ||
                    draw.magFilter > RenderSamplerFilter::Linear || draw.mipmap > UiPaintMipmap::Linear)
                    return Outcome::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                if (draw.coverage != NoUiPaintTexture) {
                    const auto coverage = result.sampled[draw.coverage]->texture;
                    if (coverage.width != paint.extent.width || coverage.height != paint.extent.height)
                        return Outcome::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                }
            }
            result.target = paint.color.IsValid() ? result.textures[0]->texture : primary;
            if (!result.target || result.target.pixelFormat != result.kernel->colorFormat || result.target.width != paint.extent.width ||
                result.target.height != paint.extent.height)
                return Outcome::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
            return Outcome::Success(std::move(result));
        }

        /** @brief Checks every declared possible read, including currently hidden sources, before native encoding. */
        bool SamplePolicies(const RenderGraphUiPaint &paint, const PaintBindings &bindings) {
            for (std::size_t index = 0; index < paint.sampledResources.size(); ++index) {
                const auto *texture = bindings.sampled[index];
                if (!texture || !HasTextureUsage(texture->descriptor.usage, RenderTextureUsage::Sampled) ||
                    texture->descriptor.sampleCount != 1 || texture->descriptor.mipCount != 1 || texture->descriptor.layerCount != 1 ||
                    !MetalTextureAspectMatches(texture->descriptor.format, RenderTextureAspect::Color) ||
                    texture->texture == bindings.target || texture == bindings.textures[1])
                    return false;
            }
            return true;
        }

        /** @brief Requires exact single-sample render roles and disallows read/write aliasing. */
        bool TexturePolicies(const RenderGraphUiPaint &paint, const PaintBindings &bindings) {
            for (const auto *texture : bindings.textures) {
                if (!texture)
                    continue;
                if (!HasTextureUsage(texture->descriptor.usage, RenderTextureUsage::RenderAttachment) ||
                    texture->descriptor.sampleCount != 1 || texture->descriptor.mipCount != 1 || texture->descriptor.layerCount != 1)
                    return false;
            }
            const auto *stencil = bindings.textures[1];
            return SamplePolicies(paint, bindings) &&
                   (!stencil || (stencil->texture != bindings.target && stencil->texture.pixelFormat == bindings.kernel->stencilFormat &&
                                 stencil->texture.width == paint.extent.width && stencil->texture.height == paint.extent.height));
        }

        MTLRenderPassDescriptor *RenderPass(const RenderGraphUiPaint &paint, const PaintBindings &bindings) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = bindings.target;
            pass.colorAttachments[0].loadAction = paint.clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            if (bindings.textures[1]) {
                pass.depthAttachment.texture = bindings.textures[1]->texture;
                pass.depthAttachment.loadAction = MTLLoadActionDontCare;
                pass.depthAttachment.storeAction = MTLStoreActionDontCare;
                pass.stencilAttachment.texture = bindings.textures[1]->texture;
                pass.stencilAttachment.loadAction = paint.clearStencil ? MTLLoadActionClear : MTLLoadActionLoad;
                pass.stencilAttachment.storeAction = MTLStoreActionStore;
                pass.stencilAttachment.clearStencil = 0;
            }
            return pass;
        }

        /** @brief Encodes one already validated operation, including exact top-left scissor and stencil equality. */
        void Draw(id<MTLRenderCommandEncoder> encoder, const RenderGraphUiPaint &paint, const PaintBindings &bindings,
                  const UiPaintDraw &draw, std::size_t index) {
            if (!draw.scissor.width || !draw.scissor.height)
                return;
            const auto &kernel = *bindings.kernel;
            const bool stencil = draw.operation == UiPaintOperation::PushStencil || draw.operation == UiPaintOperation::PopStencil;
            const bool multiply = draw.operation == UiPaintOperation::MultiplyCoverage;
            id<MTLRenderPipelineState> pipeline = multiply
                                                      ? (bindings.textures[1] ? kernel.maskedMultiplyCoverage : kernel.multiplyCoverage)
                                                      : (bindings.textures[1] ? kernel.maskedColor : kernel.color);
            [encoder setRenderPipelineState:stencil ? kernel.stencil : pipeline];
            const auto operation = stencil ? static_cast<std::size_t>(draw.operation) : 0;
            [encoder setDepthStencilState:stencil || draw.stencilDepth ? kernel.stencilStates[operation] : kernel.unmasked];
            [encoder setStencilReferenceValue:draw.stencilDepth];
            [encoder setScissorRect:MTLScissorRect{draw.scissor.x, draw.scissor.y, draw.scissor.width, draw.scissor.height}];
            const auto offset = (paint.firstParameter + index) * UiPaintFrameUpdate::ParameterStride;
            [encoder setVertexBuffer:bindings.buffers[2]->buffer offset:offset atIndex:kernel.bindings[0]];
            [encoder setFragmentBuffer:bindings.buffers[2]->buffer offset:offset atIndex:kernel.bindings[0]];
            [encoder
                setFragmentSamplerState:kernel
                                            .samplers[6 * static_cast<std::size_t>(draw.filter) +
                                                      3 * static_cast<std::size_t>(draw.magFilter) + static_cast<std::size_t>(draw.mipmap)]
                                atIndex:kernel.bindings[3]];
            const auto *sampled = draw.sampled < paint.sampledResources.size() ? bindings.sampled[draw.sampled] : nullptr;
            const auto *coverage = draw.coverage < paint.sampledResources.size() ? bindings.sampled[draw.coverage] : nullptr;
            [encoder setFragmentTexture:sampled ? sampled->texture : kernel.white atIndex:kernel.bindings[1]];
            [encoder setFragmentTexture:coverage ? coverage->texture : kernel.white atIndex:kernel.bindings[2]];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:draw.indexCount
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:bindings.buffers[1]->buffer
                         indexBufferOffset:std::size_t{draw.firstIndex} * sizeof(std::uint32_t)];
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateUiPaint */
    Result<void> MetalResourceRuntime::ValidateUiPaint(const RenderGraphUiPaint &paint,
                                                       std::span<const RenderGraphResourceInstance> resources, void *primary) const {
        const auto resolved = Bindings(paint, resources, impl_->buffers, impl_->textures, (__bridge id<MTLTexture>)primary,
                                       impl_->uiPaintKernelOwner, impl_->lightKernelIdentityExhausted ? 0 : impl_->lightKernelIncarnation);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        return TexturePolicies(paint, resolved.Value()) ? Result<void>::Success()
                                                        : Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
    }

    /** @copydoc MetalResourceRuntime::ExecuteUiPaint */
    Result<void> MetalResourceRuntime::ExecuteUiPaint(void *commands, const RenderGraphUiPaint &paint,
                                                      std::span<const RenderGraphResourceInstance> resources, void *primary) {
        if (const auto valid = ValidateUiPaint(paint, resources, primary); valid.HasError())
            return valid;
        auto resolved = Bindings(paint, resources, impl_->buffers, impl_->textures, (__bridge id<MTLTexture>)primary,
                                 impl_->uiPaintKernelOwner, impl_->lightKernelIncarnation);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        const auto &bindings = resolved.Value();
        id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)commands;
        if (!buffer)
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:RenderPass(paint, bindings)];
        if (!encoder)
            return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        [encoder setViewport:MTLViewport{0, 0, static_cast<double>(paint.extent.width), static_cast<double>(paint.extent.height), 0, 1}];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setVertexBuffer:bindings.buffers[0]->buffer offset:0 atIndex:MetalUiPaintKernel::VertexBufferBinding];
        for (std::size_t index = 0; index < paint.draws.size(); ++index)
            Draw(encoder, paint, bindings, paint.draws[index], index);
        [encoder endEncoding];
        for (auto *resident : bindings.buffers)
            Impl::TrackUse(resident->use, buffer);
        for (auto *texture : bindings.textures)
            if (texture)
                Impl::TrackUse(texture->use, buffer);
        for (std::size_t index = 0; index < paint.sampledResources.size(); ++index)
            Impl::TrackUse(bindings.sampled[index]->use, buffer);
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
