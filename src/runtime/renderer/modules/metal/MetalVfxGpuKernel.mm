#include "MetalResourceRuntimeInternal.h"
#include "MetalUniformShaderAbi.h"
#include "MetalVfxGpuState.h"

#include <algorithm>
#include <format>

namespace Horo::Render::Detail {
    namespace {
        struct NativeFunction final {
            __strong id<MTLFunction> function{nil};
        };

        /** @brief Decodes the exact canonical stage and lets Metal validate the real offline library. */
        Result<NativeFunction> Function(id<MTLDevice> device, const CookedVfxGpuProgram &program, VfxGpuProgramRole role, ShaderStage stage,
                                        NSString *entry, const MTLFunctionType type) {
            const auto bytes = VfxGpuNativePayload(program.artifact, role, stage);
            if (bytes.HasError())
                return Result<NativeFunction>::Failure(bytes.ErrorValue());
            dispatch_data_t data = dispatch_data_create(bytes.Value().data(), bytes.Value().size(),
                                                        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            NSError *error = nil;
            id<MTLLibrary> library = data ? [device newLibraryWithData:data error:&error] : nil;
            id<MTLFunction> function = library ? [library newFunctionWithName:entry] : nil;
            if (!function || function.functionType != type)
                return Result<NativeFunction>::Failure(
                    MakeError(MetalBackendErrors::ResourceCreationFailed, "Metal rejected the exact cooked GPU VFX stage or entry."));
            return Result<NativeFunction>::Success({function});
        }

        /** @brief Resolves only an active first-class final-artifact buffer map, never source register guesses. */
        Result<NSUInteger> Binding(const NormalizedShaderReflection &reflection, const std::uint32_t id) {
            const auto found = std::ranges::find(reflection.targetBindings, ShaderBindingId{id}, &ShaderTargetBindingMapEntry::id);
            if (found == reflection.targetBindings.end() || !found->active || found->nativeSpace != 0 || found->generatedHelperIndex != 0 ||
                found->nativeBinding >= 30)
                return Result<NSUInteger>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            return Result<NSUInteger>::Success(found->nativeBinding);
        }

        /** @brief Finds exact actual buffer reflection without accepting inactive arguments or another namespace. */
        MTLArgument *Argument(NSArray<MTLArgument *> *arguments, const NSUInteger binding) {
            for (MTLArgument *argument in arguments)
                if (argument.active && argument.type == MTLArgumentTypeBuffer && argument.index == binding)
                    return argument;
            return nil;
        }

        /** @brief Native UInt words are either direct pointers or the translator's sole runtime-array member. */
        bool WordElement(MTLArgument *argument) {
            if (argument.bufferStructType == nil)
                return argument.bufferPointerType.elementType == MTLDataTypeUInt;
            const auto members = argument.bufferStructType.members;
            if (members.count != 1)
                return false;
            MTLStructMember *member = members[0];
            if (member.offset != 0)
                return false;
            if (member.dataType == MTLDataTypeUInt)
                return true;
            return member.dataType == MTLDataTypeArray && member.arrayType.elementType == MTLDataTypeUInt && member.arrayType.stride == 4 &&
                   member.arrayType.elementStructType == nil && member.arrayType.elementArrayType == nil;
        }

        Result<void> ValidateWords(MTLArgument *argument, const bool writes) {
            const bool access = writes ? argument.access == MTLArgumentAccessReadWrite || argument.access == MTLArgumentAccessWriteOnly
                                       : argument.access == MTLArgumentAccessReadOnly;
            if (!argument || !access || argument.bufferDataSize != 4 || !WordElement(argument))
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                              std::format("GPU VFX word argument index {} access {} bytes {} differs from actual UInt32 ABI (writes {}).",
                                          argument.index, static_cast<unsigned>(argument.access), argument.bufferDataSize, writes)));
            return Result<void>::Success();
        }

        bool ParameterMatches(const ShaderReflectedParameter &parameter, MTLStructType *type, const std::size_t bytes) {
            return MetalUniformParameterFits(parameter, bytes) && HasMetalUniformParameter(type, parameter);
        }

        Result<void> ValidateUniform(MTLArgument *argument, const NormalizedShaderReflection &reflection, const ShaderBindingId binding,
                                     const std::size_t bytes) {
            const bool extent = argument.bufferDataSize == bytes && argument.bufferStructType != nil;
            if (!argument || argument.access != MTLArgumentAccessReadOnly || !extent)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            std::size_t count = 0;
            for (const auto &parameter : reflection.parameters) {
                const bool selected = parameter.active && parameter.binding == binding;
                if (!selected)
                    continue;
                if (!ParameterMatches(parameter, argument.bufferStructType, bytes))
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                  std::format("GPU VFX uniform parameter {} differs from actual native packing.", parameter.id.value)));
                ++count;
            }
            if (argument.bufferStructType.members.count != count)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            return Result<void>::Success();
        }

        Result<void> ComputeBindings(MTLComputePipelineReflection *reflection, const NormalizedShaderReflection &normalized,
                                     MetalVfxGpuKernel &kernel) {
            for (std::uint32_t id = 1; id <= 5; ++id) {
                const auto mapped = Binding(normalized, id);
                if (mapped.HasError())
                    return Result<void>::Failure(mapped.ErrorValue());
                kernel.computeBindings[id - 1] = mapped.Value();
                const auto argument = Argument(reflection.arguments, mapped.Value());
                const auto valid = id == 5 ? ValidateUniform(argument, normalized, {5}, VfxGpuDispatchWords::Bytes)
                                           : ValidateWords(argument, id == 2 || id == 4);
                if (valid.HasError())
                    return valid;
            }
            auto sorted = kernel.computeBindings;
            std::ranges::sort(sorted);
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            return Result<void>::Success();
        }

        Result<void> PrepareCompute(id<MTLDevice> device, const CookedVfxGpuProgram &program, MetalVfxGpuKernel &kernel) {
            const auto normalized = NormalizeShaderReflection(program.manifest, program.target, program.reflection);
            if (normalized.HasError())
                return Result<void>::Failure(normalized.ErrorValue());
            const auto function =
                Function(device, program, VfxGpuProgramRole::Simulation, ShaderStage::Compute, @"SimulateParticles", MTLFunctionTypeKernel);
            if (function.HasError())
                return Result<void>::Failure(function.ErrorValue());
            NSError *error = nil;
            MTLComputePipelineReflection *reflection = nil;
            kernel.compute = [device newComputePipelineStateWithFunction:function.Value().function
                                                                 options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                              reflection:&reflection
                                                                   error:&error];
            if (!kernel.compute || !reflection || reflection.arguments.count != 5 || kernel.compute.maxTotalThreadsPerThreadgroup < 64)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
            return ComputeBindings(reflection, normalized.Value(), kernel);
        }

        MTLRenderPipelineDescriptor *DrawDescriptor(id<MTLFunction> vertex, id<MTLFunction> fragment, const bool depth) {
            auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
            descriptor.vertexFunction = vertex;
            descriptor.fragmentFunction = fragment;
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            auto *color = descriptor.colorAttachments[0];
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = MTLBlendFactorOne;
            color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationAlphaBlendFactor = MTLBlendFactorOne;
            if (depth)
                descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            return descriptor;
        }

        Result<void> DrawAbi(MTLRenderPipelineReflection *reflection, const NormalizedShaderReflection &metadata,
                             MetalVfxGpuKernel &kernel) {
            const auto particles = Binding(metadata, 1), view = Binding(metadata, 2);
            if (particles.HasError())
                return Result<void>::Failure(particles.ErrorValue());
            if (view.HasError())
                return Result<void>::Failure(view.ErrorValue());
            if (!reflection || reflection.vertexArguments.count != 2 || reflection.fragmentArguments.count != 0 ||
                particles.Value() == view.Value())
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            kernel.particlesBinding = particles.Value();
            kernel.viewBinding = view.Value();
            if (const auto valid = ValidateWords(Argument(reflection.vertexArguments, particles.Value()), false); valid.HasError())
                return valid;
            return ValidateUniform(Argument(reflection.vertexArguments, view.Value()), metadata, {2}, VfxGpuDrawWords::Bytes);
        }

        /** @brief Retains actual native depth state for both attachment variants; disabled depth never passes a nil object. */
        Result<void> PrepareDrawDepthStates(id<MTLDevice> device, MetalVfxGpuKernel &kernel) {
            auto *depth = [[MTLDepthStencilDescriptor alloc] init];
            depth.depthCompareFunction = MTLCompareFunctionLessEqual;
            depth.depthWriteEnabled = NO;
            kernel.depthRead = [device newDepthStencilStateWithDescriptor:depth];
            depth.depthCompareFunction = MTLCompareFunctionAlways;
            kernel.depthDisabled = [device newDepthStencilStateWithDescriptor:depth];
            if (!kernel.depthRead || !kernel.depthDisabled)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
            return Result<void>::Success();
        }

        Result<void> PrepareDraw(id<MTLDevice> device, const CookedVfxGpuProgram &program, MetalVfxGpuKernel &kernel) {
            const auto normalized = NormalizeShaderReflection(program.manifest, program.target, program.reflection);
            if (normalized.HasError())
                return Result<void>::Failure(normalized.ErrorValue());
            const auto vertex =
                Function(device, program, VfxGpuProgramRole::Draw, ShaderStage::Vertex, @"VfxGpuVSMain", MTLFunctionTypeVertex);
            const auto fragment =
                Function(device, program, VfxGpuProgramRole::Draw, ShaderStage::Fragment, @"VfxGpuPSMain", MTLFunctionTypeFragment);
            if (vertex.HasError())
                return Result<void>::Failure(vertex.ErrorValue());
            if (fragment.HasError())
                return Result<void>::Failure(fragment.ErrorValue());
            for (const bool depth : {false, true}) {
                MTLRenderPipelineReflection *reflection = nil;
                NSError *error = nil;
                const auto pipeline =
                    [device newRenderPipelineStateWithDescriptor:DrawDescriptor(vertex.Value().function, fragment.Value().function, depth)
                                                         options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                      reflection:&reflection
                                                           error:&error];
                if (!pipeline)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
                if (const auto valid = DrawAbi(reflection, normalized.Value(), kernel); valid.HasError())
                    return valid;
                if (depth)
                    kernel.drawDepth = pipeline;
                else
                    kernel.drawColor = pipeline;
            }
            return PrepareDrawDepthStates(device, kernel);
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::RealizeVfxGpuPrograms */
    Result<std::shared_ptr<IResidentVfxGpuKernel>> MetalResourceRuntime::RealizeVfxGpuPrograms(const CookedVfxGpuPrograms &programs) {
        DrainVfxGpuRetirements(64);
        using Outcome = Result<std::shared_ptr<IResidentVfxGpuKernel>>;
        if (!impl_->device || !impl_->deviceIncarnation || impl_->lightKernelIdentityExhausted)
            return Outcome::Failure(MakeError(MetalBackendErrors::NotInitialized));
        if (const auto valid = ValidateCookedVfxGpuPrograms(programs, ShaderTargetBackend::Metal); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            auto kernel = std::make_shared<MetalVfxGpuKernel>();
            if (const auto ready = PrepareCompute(impl_->device, programs.simulation, *kernel); ready.HasError())
                return Outcome::Failure(ready.ErrorValue());
            if (const auto ready = PrepareDraw(impl_->device, programs.draw, *kernel); ready.HasError())
                return Outcome::Failure(ready.ErrorValue());
            kernel->owner = impl_->vfxGpuOwner;
            kernel->incarnation = impl_->lightKernelIncarnation;
            return Outcome::Success(std::move(kernel));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
        }
    }
}  // namespace Horo::Render::Detail
