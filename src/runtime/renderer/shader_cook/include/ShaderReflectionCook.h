#pragma once
/** @file ShaderReflectionCook.h
 * @brief Non-installed normalization of actual translator evidence into the canonical RenderApi reflected record. */
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

#include <span>
#include <string_view>

namespace Horo::Render {
    /** @brief Exact stage output from the separately admitted SPIRV-Cross process. */
    struct SpirvReflectionStage {
        ShaderStage stage;
        std::string entryPoint;
        std::string json;
    };

    /** @brief Normalizes bounded official SPIRV-Cross JSON against exact authored IDs and the existing manifest contract. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeVulkanReflection(const ShaderManifest &manifest,
                                                                               const ShaderTargetRequirement &target,
                                                                               std::span<const SpirvReflectionStage> stages,
                                                                               std::span<const ShaderReflectionBindingName> names);
    /** @brief Validates the closed canonical UI interface against actual Cross JSON and exact generated MSL.
     * @param manifest Exact canonical UI manifest. @param target Selected Metal target.
     * @param stages Official reflection documents for the same validated SPIR-V stages.
     * @param translations Actual generated MSL, in the same stage order; never authored native maps.
     * @return Normalized actual target placements or typed malformed, unsupported or ambiguous evidence failure. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeUiPaintMetalReflection(const ShaderManifest &manifest,
                                                                                     const ShaderTargetRequirement &target,
                                                                                     std::span<const SpirvReflectionStage> stages,
                                                                                     std::span<const std::string> translations);
    /** @brief Validates the canonical VFX interface against same-operation Cross JSON and generated MSL.
     * @param manifest Exact canonical VFX logical manifest. @param target Selected Metal requirement.
     * @param stages Actual intermediate reflection for the selected stages.
     * @param translations Actual MSL from those same translations, in vertex/fragment order.
     * @return Normalized proven native layout or typed rejection of unsupported or ambiguous evidence.
     */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeVfxMetalReflection(const ShaderManifest &manifest,
                                                                                 const ShaderTargetRequirement &target,
                                                                                 std::span<const SpirvReflectionStage> stages,
                                                                                 std::span<const std::string> translations);
    /** @brief Validates canonical VFX semantics against same-operation Cross JSON and generated GLSL410.
     * @param manifest Exact canonical VFX interface. @param target Selected OpenGL requirement.
     * @param stages Actual intermediate reflection documents. @param translations Their actual GLSL410 translations.
     * @return Proven std140 packing and actual link names with finite Horo-assigned slots; native realization
     * independently validates linked members and actual device limits before installing those bindings.
     */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeVfxOpenGLReflection(const ShaderManifest &manifest,
                                                                                  const ShaderTargetRequirement &target,
                                                                                  std::span<const SpirvReflectionStage> stages,
                                                                                  std::span<const std::string> translations);
}  // namespace Horo::Render
