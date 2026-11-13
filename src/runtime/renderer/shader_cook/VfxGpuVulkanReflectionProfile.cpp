#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "VfxGpuVulkanReflection.h"

namespace Horo::Render {
    Result<void> ValidateVfxGpuVulkanReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                std::span<const ShaderReflectionBindingName> names) {
        try {
            if (target.backend != ShaderTargetBackend::Vulkan || target.payloadFormat != ShaderPayloadFormat::SpirV16 ||
                !target.supportsCompute || !target.supportsStorageResources ||
                !VfxGpuCookDetail::CanonicalInterface(manifest, target, names))
                throw VfxGpuCookDetail::InvalidEvidence{};
            return Result<void>::Success();
        } catch (const VfxGpuCookDetail::InvalidEvidence &) {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::InvalidReflection));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ShaderReflectionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
