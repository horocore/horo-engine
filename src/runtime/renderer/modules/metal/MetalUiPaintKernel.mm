#include "MetalUiPaintKernel.h"

#include "MetalResourceFormats.h"
#include "MetalResourceRuntimeInternal.h"

namespace Horo::Render::Detail {
    namespace {
        using Preparation = Result<std::shared_ptr<IResidentUiPaintKernel>>;

        /** @brief Loads only the exact offline entry payload; no source compilation is performed at runtime. */
        id<MTLFunction> LoadEntry(id<MTLDevice> device, const CompiledShaderArtifact &artifact, ShaderStage stage, NSString *name) {
            const auto payload = UiPaintNativePayload(artifact, stage);
            if (payload.HasError())
                return nil;
            dispatch_data_t data = dispatch_data_create(payload.Value().data(), payload.Value().size(),
                                                        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            if (!data)
                return nil;
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
            return library ? [library newFunctionWithName:name] : nil;
        }

        /** @brief Fixes the public eight-float vertex ABI independently of reflected uniform/texture bindings. */
        MTLVertexDescriptor *VertexLayout() {
            MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
            const std::array<MTLVertexFormat, 3> formats{MTLVertexFormatFloat2, MTLVertexFormatFloat2, MTLVertexFormatFloat4};
            constexpr std::array<NSUInteger, 3> offsets{0, 8, 16};
            for (std::size_t index = 0; index < formats.size(); ++index) {
                layout.attributes[index].format = formats[index];
                layout.attributes[index].offset = offsets[index];
                layout.attributes[index].bufferIndex = MetalUiPaintKernel::VertexBufferBinding;
            }
            auto *buffer = layout.layouts[MetalUiPaintKernel::VertexBufferBinding];
            buffer.stride = sizeof(UiPaintVertex);
            buffer.stepFunction = MTLVertexStepFunctionPerVertex;
            buffer.stepRate = 1;
            return layout;
        }

        /** @brief Checks the native compiler's active argument kind and uniform byte layout rather than trusting host metadata. */
        bool NativeArgument(NSArray<MTLArgument *> *arguments, NSUInteger binding, MTLArgumentType type, const bool uniform = false) {
            for (MTLArgument *argument in arguments)
                if (argument.active && argument.index == binding && argument.type == type)
                    return !uniform ||
                           (argument.access == MTLArgumentAccessReadOnly && argument.bufferDataSize == sizeof(UiPaintParameters));
            return false;
        }

        /** @brief Verifies all four canonical resources in their actual stages after native pipeline linking. */
        bool NativeInterface(MTLRenderPipelineReflection *reflection, const MetalUiPaintKernel &resident) {
            return reflection && NativeArgument(reflection.vertexArguments, resident.bindings[0], MTLArgumentTypeBuffer, true) &&
                   NativeArgument(reflection.fragmentArguments, resident.bindings[0], MTLArgumentTypeBuffer, true) &&
                   NativeArgument(reflection.fragmentArguments, resident.bindings[1], MTLArgumentTypeTexture) &&
                   NativeArgument(reflection.fragmentArguments, resident.bindings[2], MTLArgumentTypeTexture) &&
                   NativeArgument(reflection.fragmentArguments, resident.bindings[3], MTLArgumentTypeSampler);
        }

        /** @brief Creates fixed premultiplied-over color and color-disabled stencil variants. */
        bool Pipelines(id<MTLDevice> device, const CookedUiPaintKernel &cooked, MetalUiPaintKernel &resident) {
            id<MTLFunction> vertex = LoadEntry(device, cooked.artifact, ShaderStage::Vertex, @"PaintVertex");
            id<MTLFunction> fragment = LoadEntry(device, cooked.artifact, ShaderStage::Fragment, @"PaintFragment");
            if (!vertex || !fragment || vertex.functionType != MTLFunctionTypeVertex || fragment.functionType != MTLFunctionTypeFragment)
                return false;
            MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
            descriptor.vertexFunction = vertex;
            descriptor.fragmentFunction = fragment;
            descriptor.vertexDescriptor = VertexLayout();
            descriptor.colorAttachments[0].pixelFormat = resident.colorFormat;
            descriptor.depthAttachmentPixelFormat = resident.stencilFormat;
            descriptor.stencilAttachmentPixelFormat = resident.stencilFormat;
            auto *color = descriptor.colorAttachments[0];
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = MTLBlendFactorOne;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            NSError *error = nil;
            MTLRenderPipelineReflection *reflection = nil;
            resident.color = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                  options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                               reflection:&reflection
                                                                    error:&error];
            if (!NativeInterface(reflection, resident))
                return false;
            resident.maskedColor = resident.color;
            color.writeMask = MTLColorWriteMaskNone;
            resident.stencil = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            color.writeMask = MTLColorWriteMaskAll;
            color.sourceRGBBlendFactor = MTLBlendFactorZero;
            color.sourceAlphaBlendFactor = MTLBlendFactorZero;
            color.destinationRGBBlendFactor = MTLBlendFactorSourceAlpha;
            color.destinationAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            resident.maskedMultiplyCoverage = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            descriptor.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
            descriptor.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
            resident.multiplyCoverage = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            color.sourceRGBBlendFactor = MTLBlendFactorOne;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            descriptor.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
            descriptor.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
            resident.color = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            return resident.color && resident.maskedColor && resident.stencil && resident.multiplyCoverage &&
                   resident.maskedMultiplyCoverage;
        }

        /** @brief Configures equality-tested bounded nesting without depth writes or normal-frame synchronization. */
        bool StencilStates(id<MTLDevice> device, MetalUiPaintKernel &resident) {
            MTLDepthStencilDescriptor *descriptor = [[MTLDepthStencilDescriptor alloc] init];
            descriptor.depthCompareFunction = MTLCompareFunctionAlways;
            descriptor.depthWriteEnabled = NO;
            resident.unmasked = [device newDepthStencilStateWithDescriptor:descriptor];
            constexpr std::array<MTLStencilOperation, 3> operations{MTLStencilOperationKeep, MTLStencilOperationIncrementClamp,
                                                                    MTLStencilOperationDecrementClamp};
            for (std::size_t index = 0; index < operations.size(); ++index) {
                MTLStencilDescriptor *stencil = [[MTLStencilDescriptor alloc] init];
                stencil.stencilCompareFunction = MTLCompareFunctionEqual;
                stencil.stencilFailureOperation = MTLStencilOperationKeep;
                stencil.depthFailureOperation = MTLStencilOperationKeep;
                stencil.depthStencilPassOperation = operations[index];
                stencil.readMask = 0xff;
                stencil.writeMask = index == 0 ? 0 : 0xff;
                descriptor.frontFaceStencil = stencil;
                descriptor.backFaceStencil = stencil;
                resident.stencilStates[index] = [device newDepthStencilStateWithDescriptor:descriptor];
                if (!resident.stencilStates[index])
                    return false;
            }
            return resident.unmasked != nil;
        }

        /** @brief Owns deterministic white fallback sampling and every closed minification/magnification/mipmap combination before any
         * frame. */
        bool Sampling(id<MTLDevice> device, MetalUiPaintKernel &resident) {
            MTLSamplerDescriptor *sampler = [[MTLSamplerDescriptor alloc] init];
            sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
            for (std::size_t index = 0; index < resident.samplers.size(); ++index) {
                sampler.minFilter = index / 6 == 0 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
                sampler.magFilter = (index / 3) % 2 == 0 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
                sampler.mipFilter = index % 3 == 0   ? MTLSamplerMipFilterNotMipmapped
                                    : index % 3 == 1 ? MTLSamplerMipFilterNearest
                                                     : MTLSamplerMipFilterLinear;
                resident.samplers[index] = [device newSamplerStateWithDescriptor:sampler];
                if (!resident.samplers[index])
                    return false;
            }
            MTLTextureDescriptor *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                               width:1
                                                                                              height:1
                                                                                           mipmapped:NO];
            texture.usage = MTLTextureUsageShaderRead;
            texture.storageMode = MTLStorageModeShared;
            resident.white = [device newTextureWithDescriptor:texture];
            if (!resident.white)
                return false;
            constexpr std::uint32_t white = 0xffffffff;
            [resident.white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
            return true;
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::RealizeUiPaintKernel */
    Result<std::shared_ptr<IResidentUiPaintKernel>> MetalResourceRuntime::RealizeUiPaintKernel(const CookedUiPaintKernel &kernel) {
        if (!impl_->device || impl_->lightKernelIdentityExhausted || impl_->lightKernelIncarnation == 0)
            return Preparation::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution));
        const auto reflection = ValidateCookedUiPaintKernel(kernel, ShaderTargetBackend::Metal);
        if (reflection.HasError())
            return Preparation::Failure(reflection.ErrorValue());
        try {
            auto resident = std::make_shared<MetalUiPaintKernel>();
            for (const auto &binding : reflection.Value().targetBindings) {
                if (binding.id.value == 0 || binding.id.value > 4 || binding.nativeSpace != 0 || binding.generatedHelperIndex != 0)
                    return Preparation::Failure(MakeError(MetalBackendErrors::InvalidConfig));
                resident->bindings[binding.id.value - 1] = binding.nativeBinding;
            }
            if (resident->bindings[0] == MetalUiPaintKernel::VertexBufferBinding)
                return Preparation::Failure(MakeError(MetalBackendErrors::InvalidConfig));
            resident->colorFormat = static_cast<MTLPixelFormat>(MetalPixelFormatValue(kernel.colorFormat));
            resident->stencilFormat = static_cast<MTLPixelFormat>(MetalPixelFormatValue(kernel.stencilFormat));
            if (!Pipelines(impl_->device, kernel, *resident) || !StencilStates(impl_->device, *resident) ||
                !Sampling(impl_->device, *resident))
                return Preparation::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed, "Cooked UI raster preparation failed."));
            resident->owner = impl_->uiPaintKernelOwner;
            resident->incarnation = impl_->lightKernelIncarnation;
            return Preparation::Success(std::move(resident));
        } catch (const std::bad_alloc &) {
            return Preparation::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
        }
    }
}  // namespace Horo::Render::Detail
