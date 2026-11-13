#include "ExposureGpuKernel.h"

#include "MetalResourceRuntimeInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Matches one exact active native argument against the closed ABI. */
        bool ArgumentValid(MTLArgument *argument, std::size_t logical) noexcept {
            if (!argument.active)
                return false;
            if (logical == 0)
                return argument.type == MTLArgumentTypeTexture && argument.textureType == MTLTextureType2D &&
                       argument.access == MTLArgumentAccessReadOnly;
            if (argument.type != MTLArgumentTypeBuffer)
                return false;
            constexpr std::array<NSUInteger, 5> sizes{0, 4, 32, 32, 96};
            if (argument.bufferDataSize != sizes[logical])
                return false;
            if (logical == 1 || logical == 3)
                return argument.access == MTLArgumentAccessReadWrite || argument.access == MTLArgumentAccessWriteOnly;
            return argument.access == MTLArgumentAccessReadOnly;
        }

        /** @brief Independently checks native pipeline arguments, not only cook-provided reflection. */
        bool ReflectionValid(MTLComputePipelineReflection *reflection, const std::array<NSUInteger, 5> &bindings) noexcept {
            if (reflection == nil)
                return false;
            std::size_t count{};
            for (std::size_t logical = 0; logical < bindings.size(); ++logical) {
                if (bindings[logical] == NSNotFound)
                    continue;
                ++count;
                bool found{};
                for (MTLArgument *argument in reflection.arguments)
                    if (argument.index == bindings[logical] && ArgumentValid(argument, logical))
                        found = true;
                if (!found)
                    return false;
            }
            return reflection.arguments.count == count;
        }

        /** @brief Maps the normalized logical interface into one exact native binding table. */
        Result<void> Bindings(const NormalizedShaderReflection &reflection, std::array<NSUInteger, 5> &bindings) {
            bindings.fill(NSNotFound);
            for (const auto &binding : reflection.targetBindings) {
                if (!binding.active || binding.generatedHelperIndex != 0 || binding.nativeSpace != 0 || binding.id.value == 0 ||
                    binding.id.value > 5)
                    return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
                bindings[binding.id.value - 1] = binding.nativeBinding;
            }
            return Result<void>::Success();
        }

        /** @brief Creates one actual Metal pipeline from its standard offline library package. */
        Result<void> Pipeline(id<MTLDevice> device, const CookedExposureGpuStage &stage, ExposureGpuStage index,
                              ExposureGpuKernel &resident) {
            auto payload = ExposureGpuNativePayload(index, stage.artifact);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            dispatch_data_t data = dispatch_data_create(payload.Value().data(), payload.Value().size(),
                                                        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            if (data == nullptr)
                return Result<void>::Failure(MakeError(ExposureGpuErrors::Capacity));
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
            const std::array<NSString *, 3> names{@"ClearExposureHistogram", @"BuildExposureHistogram", @"AdaptExposure"};
            const auto slot = static_cast<std::size_t>(index);
            id<MTLFunction> function = [library newFunctionWithName:names[slot]];
            if (function == nil || function.functionType != MTLFunctionTypeKernel)
                return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            MTLComputePipelineReflection *reflection = nil;
            resident.pipelines[slot] =
                [device newComputePipelineStateWithFunction:function
                                                    options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                 reflection:&reflection
                                                      error:&error];
            const NSUInteger requiredThreads = index == ExposureGpuStage::BuildHistogram ? 64 : 256;
            if (resident.pipelines[slot] == nil || resident.pipelines[slot].maxTotalThreadsPerThreadgroup < requiredThreads ||
                !ReflectionValid(reflection, resident.bindings[slot]))
                return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::RealizeExposureGpuKernel */
    Result<std::shared_ptr<IResidentExposureGpuKernel>> MetalResourceRuntime::RealizeExposureGpuKernel(
        const CookedExposureGpuKernel &kernel) {
        using Return = Result<std::shared_ptr<IResidentExposureGpuKernel>>;
        if (impl_->device == nil || impl_->deviceIncarnation == 0)
            return Return::Failure(MakeError(ExposureGpuErrors::Unsupported));
        auto slot = std::ranges::find_if(impl_->exposureGpuKernels, [](const auto &entry) {
            return entry.expired();
        });
        if (slot == impl_->exposureGpuKernels.end())
            return Return::Failure(MakeError(ExposureGpuErrors::Capacity));
        auto validated = ValidateCookedExposureGpuKernel(kernel, ShaderTargetBackend::Metal);
        if (validated.HasError())
            return Return::Failure(validated.ErrorValue());
        try {
            auto resident = std::make_shared<ExposureGpuKernel>();
            for (std::size_t index = 0; index < 3; ++index) {
                if (kernel.stages[index].artifact.payloadFormat != ShaderPayloadFormat::MetalLibrary24)
                    return Return::Failure(MakeError(ExposureGpuErrors::Unsupported));
                if (auto mapped = Bindings(validated.Value()[index], resident->bindings[index]); mapped.HasError())
                    return Return::Failure(mapped.ErrorValue());
                if (auto native = Pipeline(impl_->device, kernel.stages[index], static_cast<ExposureGpuStage>(index), *resident);
                    native.HasError())
                    return Return::Failure(native.ErrorValue());
            }
            resident->owner = impl_->exposureGpuOwner;
            resident->incarnation = impl_->deviceIncarnation;
            *slot = resident;
            return Return::Success(std::move(resident));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(ExposureGpuErrors::Capacity));
        }
    }

    /** @copydoc MetalResourceRuntime::FinishExposureGpuCommands */
    void MetalResourceRuntime::FinishExposureGpuCommands(void *commands, bool committed) noexcept {
        for (auto &weak : impl_->exposureGpuKernels)
            if (auto kernel = weak.lock())
                kernel->Finish((__bridge id<MTLCommandBuffer>)commands, committed);
    }
}  // namespace Horo::Render::Detail
