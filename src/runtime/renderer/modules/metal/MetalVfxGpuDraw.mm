#include "MetalColorAttachmentEncoding.h"
#include "MetalResourceRuntimeInternal.h"
#include "MetalVfxGpuGraphInternal.h"

namespace Horo::Render::Detail {
    namespace {
        Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid, message));
        }

        struct Attachments final {
            MetalTextureInstance *color{}, *depth{};
        };

        Result<MetalTextureInstance *> Texture(const RenderGraphResourceId id, const RenderGraphExecutionRequest &request,
                                               const std::unordered_set<MetalTextureInstance *> &textures, const FramebufferExtent extent,
                                               const RenderTextureFormat format) {
            auto *texture = Decode<MetalTextureInstance>(MetalVfxGraphInstance(id, request));
            if (!textures.contains(texture))
                return Result<MetalTextureInstance *>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            const auto &descriptor = texture->descriptor;
            if (texture->use.retired || descriptor.extent != extent || descriptor.format != format || descriptor.sampleCount != 1 ||
                descriptor.layerCount != 1 || descriptor.mipCount != 1 ||
                !HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment))
                return Result<MetalTextureInstance *>::Failure(
                    MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                              "GPU VFX draw requires an exact whole RGBA8 color and optional Depth32 attachment."));
            return Result<MetalTextureInstance *>::Success(texture);
        }

        /** @brief A same-graph destination may be drawn before native submission, but never without its exact earlier compute producer. */
        bool CandidateProducer(const MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuDraw &draw,
                               const RenderGraphExecutionRequest &request) {
            for (const auto &entry : request.workloads) {
                if (std::get_if<RenderGraphVfxGpuDraw>(&entry.workload) == &draw)
                    return false;
                const auto *compute = std::get_if<RenderGraphVfxGpuCompute>(&entry.workload);
                if (!compute || compute->simulationLease.get() != &simulation)
                    continue;
                if (compute->work.step.targetGeneration == draw.generation &&
                    MetalVfxGraphInstance(compute->destination, request) == MetalVfxGraphInstance(draw.particles, request))
                    return true;
            }
            return false;
        }

        Result<void> DrawGeneration(const MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuDraw &draw,
                                    const RenderGraphExecutionRequest &request) {
            if (simulation.stopped || draw.simulation != simulation.simulationId || draw.lifetimeCancellation.IsCancellationRequested() ||
                !SameVfxGpuDescriptor(simulation.descriptor, draw.descriptor))
                return Invalid("GPU VFX draw metadata is stale or revoked.");
            const auto index = MetalVfxStateIndex(simulation, draw.particles, request);
            if (index >= simulation.states.size())
                return Invalid("GPU VFX draw source is outside its exact issued ring.");
            if (simulation.states[index].generation == draw.generation && draw.generation <= simulation.currentGeneration)
                return Result<void>::Success();
            return CandidateProducer(simulation, draw, request)
                       ? Result<void>::Success()
                       : Invalid("GPU VFX draw has neither an accepted generation nor an exact earlier same-graph compute producer.");
        }

        Result<Attachments> DrawAttachments(const RenderGraphVfxGpuDraw &draw, const RenderGraphExecutionRequest &request,
                                            const std::unordered_set<MetalTextureInstance *> &textures) {
            const auto color = Texture(draw.color, request, textures, draw.view.extent, RenderTextureFormat::Rgba8Unorm);
            if (color.HasError())
                return Result<Attachments>::Failure(color.ErrorValue());
            Attachments result{color.Value(), nullptr};
            if (draw.depth.IsValid()) {
                const auto depth = Texture(draw.depth, request, textures, draw.view.extent, RenderTextureFormat::Depth32Float);
                if (depth.HasError())
                    return Result<Attachments>::Failure(depth.ErrorValue());
                result.depth = depth.Value();
            }
            return Result<Attachments>::Success(result);
        }

        struct Encoder final {
            __strong MTLRenderPassDescriptor *pass;
            __strong id<MTLRenderCommandEncoder> encoder{nil};

            ~Encoder() {
                [encoder endEncoding];
                pass.colorAttachments[0].texture = nil;
                pass.depthAttachment.texture = nil;
                pass.stencilAttachment.texture = nil;
            }
        };

        void ConfigureAttachments(MTLRenderPassDescriptor *pass, const RenderGraphVfxGpuDraw &draw, const Attachments attachments) {
            pass.colorAttachments[0].texture = attachments.color->texture;
            ConfigureMetalColorAttachment(pass.colorAttachments[0], draw.colorOperations);
            if (attachments.depth) {
                pass.depthAttachment.texture = attachments.depth->texture;
                pass.depthAttachment.loadAction = MTLLoadActionLoad;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateVfxGpuDraw */
    Result<void> MetalResourceRuntime::ValidateVfxGpuDraw(const RenderGraphVfxGpuDraw &draw,
                                                          const RenderGraphExecutionRequest &request) const {
        const auto simulation = std::dynamic_pointer_cast<MetalVfxGpuSimulation>(draw.simulationLease);
        if (!simulation || simulation->owner != impl_->vfxGpuOwner || simulation->incarnation != impl_->lightKernelIncarnation ||
            draw.deviceIncarnation != impl_->deviceIncarnation || draw.kernel.get() != simulation->kernel.get())
            return Invalid("GPU VFX draw complete-family/simulation lease belongs to another native owner.");
        if (const auto generation = DrawGeneration(*simulation, draw, request); generation.HasError())
            return generation;
        const auto attachments = DrawAttachments(draw, request, impl_->textures);
        if (attachments.HasError())
            return Result<void>::Failure(attachments.ErrorValue());
        std::array<std::uint32_t, VfxGpuDrawWords::Count> parameters;
        if (!PackVfxGpuDrawParameters(simulation->descriptor, draw.view, parameters))
            return Invalid("GPU VFX view packing failed.");
        return Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::ExecuteVfxGpuDraw */
    Result<void> MetalResourceRuntime::ExecuteVfxGpuDraw(void *nativeCommands, const RenderGraphVfxGpuDraw &draw,
                                                         const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateVfxGpuDraw(draw, request); valid.HasError())
            return valid;
        if (!nativeCommands)
            return Invalid("GPU VFX draw has no actual command buffer.");
        const auto simulation = std::static_pointer_cast<MetalVfxGpuSimulation>(draw.simulationLease);
        const auto index = MetalVfxStateIndex(*simulation, draw.particles, request);
        const auto attachments = DrawAttachments(draw, request, impl_->textures).Value();
        std::array<std::uint32_t, VfxGpuDrawWords::Count> parameters;
        if (!PackVfxGpuDrawParameters(simulation->descriptor, draw.view, parameters))
            return Invalid("GPU VFX view packing failed.");
        TrackParallelGraphUse(nativeCommands, request.resources);
        Encoder scope{impl_->vfxPass};
        ConfigureAttachments(scope.pass, draw, attachments);
        scope.encoder = [(__bridge id<MTLCommandBuffer>)nativeCommands renderCommandEncoderWithDescriptor:scope.pass];
        if (!scope.encoder)
            return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        [scope.encoder setRenderPipelineState:attachments.depth ? simulation->kernel->drawDepth : simulation->kernel->drawColor];
        [scope.encoder setDepthStencilState:attachments.depth ? simulation->kernel->depthRead : simulation->kernel->depthDisabled];
        [scope.encoder setCullMode:MTLCullModeNone];
        [scope.encoder
            setViewport:MTLViewport{0, 0, static_cast<double>(draw.view.extent.width), static_cast<double>(draw.view.extent.height), 0, 1}];
        [scope.encoder setVertexBuffer:simulation->states[index].native->buffer offset:0 atIndex:simulation->kernel->particlesBinding];
        [scope.encoder setVertexBytes:parameters.data() length:VfxGpuDrawWords::Bytes atIndex:simulation->kernel->viewBinding];
        [scope.encoder drawPrimitives:MTLPrimitiveTypeTriangle
                          vertexStart:0
                          vertexCount:6
                        instanceCount:simulation->descriptor.layout.capacity];
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
