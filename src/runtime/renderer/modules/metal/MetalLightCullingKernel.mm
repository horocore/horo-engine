#include "MetalLightCullingKernel.h"

#include "MetalResourceRuntimeInternal.h"

#include <new>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Verifies native reflection independently of the host's cooked mapping assertions. */
        bool NativeArgumentsMatch(MTLComputePipelineReflection *reflection, const std::array<NSUInteger, 5> &bindings) {
            if (reflection == nil || reflection.arguments.count != bindings.size())
                return false;
            constexpr std::array<NSUInteger, 5> elementBytes{sizeof(PackedRenderLight), sizeof(PackedLightCluster),
                                                             sizeof(PackedLightMembership), sizeof(std::uint32_t),
                                                             sizeof(LightCullingDispatch)};
            for (std::size_t logical = 0; logical < bindings.size(); ++logical) {
                bool found = false;
                for (MTLArgument *argument in reflection.arguments) {
                    if (argument.index != bindings[logical])
                        continue;
                    const bool writes = logical == 2 || logical == 3;
                    const bool accessValid =
                        writes ? argument.access == MTLArgumentAccessReadWrite || argument.access == MTLArgumentAccessWriteOnly
                               : argument.access == MTLArgumentAccessReadOnly;
                    if (!argument.active || argument.type != MTLArgumentTypeBuffer || !accessValid)
                        return false;
                    if (argument.bufferDataSize != elementBytes[logical])
                        return false;
                    found = true;
                }
                if (!found)
                    return false;
            }
            return true;
        }

        /** @brief Loads an owned offline library and verifies the resulting native pipeline ABI. */
        Result<void> InitializeNativePipeline(id<MTLDevice> device, const std::span<const std::uint8_t> payload,
                                              MetalLightCullingKernel &resident) {
            // DEFAULT copies into system-owned storage; the library never borrows the host package bytes.
            dispatch_data_t data = dispatch_data_create(payload.data(), payload.size(), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            if (data == nullptr)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::ResourceCreationFailed, "Cooked library data ownership failed."));
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
            if (library == nil)
                return Result<void>::Failure(
                    MakeError(LightCullingErrors::InvalidInput, "Metal rejected the cooked light-culling library."));
            id<MTLFunction> function = [library newFunctionWithName:@"CullLights"];
            if (function == nil || function.functionType != MTLFunctionTypeKernel)
                return Result<void>::Failure(
                    MakeError(LightCullingErrors::InvalidInput, "Cooked entry is absent or is not a compute kernel."));
            MTLComputePipelineReflection *reflection = nil;
            resident.pipeline = [device newComputePipelineStateWithFunction:function
                                                                    options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                                 reflection:&reflection
                                                                      error:&error];
            if (resident.pipeline == nil || !NativeArgumentsMatch(reflection, resident.bindings) ||
                resident.pipeline.maxTotalThreadsPerThreadgroup < 64)
                return Result<void>::Failure(
                    MakeError(LightCullingErrors::InvalidInput, "Native light-culling pipeline ABI is incompatible."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::RealizeLightCullingKernel */
    Result<std::shared_ptr<IResidentLightCullingKernel>> MetalResourceRuntime::RealizeLightCullingKernel(
        const CookedLightCullingKernel &kernel) {
        using Preparation = Result<std::shared_ptr<IResidentLightCullingKernel>>;
        if (impl_->device == nil || impl_->lightKernelIncarnation == 0 || impl_->lightKernelIdentityExhausted)
            return Preparation::Failure(MakeError(LightCullingErrors::Unsupported));
        try {
            const auto validated = ValidateCookedLightCullingKernel(kernel, ShaderTargetBackend::Metal);
            if (validated.HasError())
                return Preparation::Failure(validated.ErrorValue());
            auto resident = std::make_shared<MetalLightCullingKernel>();
            for (const auto &binding : validated.Value().targetBindings) {
                if (binding.generatedHelperIndex != 0 || binding.nativeSpace != 0 || !binding.active || binding.id.value == 0 ||
                    binding.id.value > resident->bindings.size())
                    return Preparation::Failure(MakeError(LightCullingErrors::InvalidInput));
                resident->bindings[binding.id.value - 1] = binding.nativeBinding;
            }
            const auto payload = LightCullingNativePayload(kernel.artifact);
            if (payload.HasError())
                return Preparation::Failure(payload.ErrorValue());
            const auto initialized = InitializeNativePipeline(impl_->device, payload.Value(), *resident);
            if (initialized.HasError())
                return Preparation::Failure(initialized.ErrorValue());
            resident->owner = impl_->lightKernelOwner;
            resident->incarnation = impl_->lightKernelIncarnation;
            return Preparation::Success(std::move(resident));
        } catch (const std::bad_alloc &) {
            return Preparation::Failure(
                MakeError(MetalBackendErrors::ResourceCreationFailed, "Light kernel preparation allocation failed."));
        }
    }
}  // namespace Horo::Render::Detail
