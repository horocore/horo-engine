#pragma once

/** @file LightCullingKernel.h
 * @brief Cooked-kernel admission and owned native-compute preparation lease.
 */
#include "Horo/Runtime/Render/LightFrameLayout.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

#include <memory>

namespace Horo::Render {
    /** @brief Logical bindings of the fixed portable light-culling interface. */
    enum class LightCullingBinding : std::uint32_t {
        Lights = 1,
        Clusters = 2,
        Membership = 3,
        References = 4,
        Dispatch = 5,
    };

    /** @brief Actual cooked artifact and final-target reflection submitted at a non-frame-hot preparation boundary. */
    struct CookedLightCullingKernel final {
        ShaderManifest manifest;
        ShaderTargetRequirement target;
        CompiledShaderArtifact artifact;
        ShaderReflectionCandidate reflection;
    };

    /**
     * @brief Backend-owned immutable compute pipeline lease; contains no public native handles.
     * @details The host releases preparation leases on the render owner thread. Submitted commands
     * independently retain native pipeline objects through terminal completion or unsent abandonment.
     * The host detaches and drains users before backend teardown; no normal-frame wait is permitted.
     */
    class IResidentLightCullingKernel {
    public:
        virtual ~IResidentLightCullingKernel() = default;
    };

    /**
     * @brief Creates the fixed light-culling manifest for an explicitly admitted target.
     * @param target Complete target descriptor with compute/storage requirements and exact payload format.
     * @return HLSL2021 logical interface consumed by the existing locked cook route; no compilation occurs.
     */
    [[nodiscard]] ShaderManifest MakeLightCullingShaderManifest(const ShaderTargetRequirement &target);

    /**
     * @brief Validates cooked stage, entry, logical ABI, exact target, payload envelope and final reflection.
     * @details Does not compile, discover tools, select a backend or realize native resources. The native
     * adapter must subsequently load this exact artifact and reject mismatching native argument layouts.
     * @param kernel Host-owned cooked artifact and reflection evidence.
     * @param expectedBackend Backend selected by host composition.
     * @return Normalized target binding map or original validation/typed light failure.
     */
    [[nodiscard]] Result<NormalizedShaderReflection> ValidateCookedLightCullingKernel(const CookedLightCullingKernel &kernel,
                                                                                      ShaderTargetBackend expectedBackend);

    /**
     * @brief Borrows the exact single compute-entry payload from the existing HOROSHDR v1 toolchain envelope.
     * @param artifact Bounded cooked artifact owned by the caller for the entire use of the returned span.
     * @return Exact CullLights compute bytes, or typed malformed/version/stage/entry/envelope failure.
     * @details Does not reinterpret the package as native library bytes or accept trailing/duplicate records.
     */
    [[nodiscard]] Result<std::span<const std::uint8_t>> LightCullingNativePayload(const CompiledShaderArtifact &artifact);
}  // namespace Horo::Render
