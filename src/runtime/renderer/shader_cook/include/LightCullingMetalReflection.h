#pragma once
/** @file LightCullingMetalReflection.h @brief Exact single-entry Metal compute evidence for the canonical light-culling ABI. */
#include "ShaderReflectionCook.h"

namespace Horo::Render {
    [[nodiscard]] std::vector<ShaderReflectionBindingName> LightCullingMetalReflectionNames();
    /** @brief Admits only the canonical MakeLightCullingShaderManifest interface and authored names. */
    [[nodiscard]] Result<void> ValidateLightCullingMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                   std::span<const ShaderReflectionBindingName> names);
    /** @brief Qualifies only selected kernel input arguments after independent validated-SPIRV readonly and exact native ABI proof.
     * @details Preserves every other source byte, including the shader body and writable output declarations. No tool execution occurs. */
    [[nodiscard]] Result<std::string> QualifyLightCullingMetalInputs(std::string_view source, std::span<const std::uint8_t> validatedSpirv);
    /** @brief Proves exact actual Cross JSON declarations and the selected translated MSL kernel together.
     * @details Parser output is layout evidence only, never native compilation or execution proof. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeLightCullingMetalReflection(
        const ShaderManifest &manifest, const ShaderTargetRequirement &target, std::span<const SpirvReflectionStage> stages,
        std::span<const std::string> translations, std::span<const ShaderReflectionBindingName> names,
        std::span<const std::uint8_t> validatedSpirv = {});
}  // namespace Horo::Render
