#pragma once
/** @file VfxGpuVulkanReflection.h @brief Closed actual-SPIR-V reflection for the two canonical VFX roles. */
#include "ShaderReflectionCook.h"
#include "VfxGpuSpirvEvidence.h"

namespace Horo::Render {
    [[nodiscard]] Result<void> ValidateVfxGpuVulkanReflection(const ShaderManifest &, const ShaderTargetRequirement &,
                                                              std::span<const ShaderReflectionBindingName>);
    /** @brief Reconciles actual Cross JSON with the exact immutable stage bytes packaged by this Vulkan compilation.
     * @details No MSL, native binding guesses, Apple tools, source rewrite or capability grant is involved. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeVfxGpuVulkanReflection(const ShaderManifest &,
                                                                                     const ShaderTargetRequirement &,
                                                                                     std::span<const SpirvReflectionStage>,
                                                                                     std::span<const ShaderReflectionBindingName>,
                                                                                     const CompiledShaderArtifact &);
}  // namespace Horo::Render
