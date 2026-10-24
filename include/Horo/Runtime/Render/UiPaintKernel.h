#pragma once

/** @file UiPaintKernel.h
 * @brief Exact cooked portable UI raster interface and render-owner native pipeline ownership.
 */
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

#include <memory>

namespace Horo::Render {
    /** @brief Exact two-stage artifact with its same-boundary immutable reflection, prepared through the existing locked cook route. */
    struct CookedUiPaintKernel final {
        ShaderManifest manifest;
        ShaderTargetRequirement target;
        CompiledShaderArtifact artifact;
        RenderTextureFormat colorFormat{RenderTextureFormat::Rgba8Unorm};
        RenderTextureFormat stencilFormat{RenderTextureFormat::Depth24Stencil8};
    };

    /** @brief Selected-backend pipeline owner; submitted commands independently retain its exact native objects.
     * @details Release preparation leases on the render owner thread after detaching users. Native completion or unsent
     * abandonment releases queued references. Creation and destruction never imply a normal-frame GPU wait. */
    class IResidentUiPaintKernel {
    public:
        virtual ~IResidentUiPaintKernel() = default;
    };

    /** @brief Creates the fixed raster interface without compiling or selecting a backend.
     * @param target Explicit owning cook target. @return Inert exact vertex/fragment interface manifest. */
    [[nodiscard]] ShaderManifest MakeUiPaintShaderManifest(const ShaderTargetRequirement &target);

    /** @brief Validates the exact cooked interface, target, artifact envelope and normalized reflection.
     * @param kernel Owned cook evidence and target attachment formats. @param backend Selected native backend family.
     * @return Normalized binding map or original typed cook/interface failure; performs no native realization. */
    [[nodiscard]] Result<NormalizedShaderReflection> ValidateCookedUiPaintKernel(const CookedUiPaintKernel &kernel,
                                                                                 ShaderTargetBackend backend);

    /** @brief Borrows one exact entry payload from the strict two-stage HOROSHDR envelope.
     * @param artifact Complete caller-owned artifact. @param stage Vertex or Fragment.
     * @return Exact native bytes or typed malformed/stage/order/trailing-data failure. */
    [[nodiscard]] Result<std::span<const std::uint8_t>> UiPaintNativePayload(const CompiledShaderArtifact &artifact, ShaderStage stage);
}  // namespace Horo::Render
