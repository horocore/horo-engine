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

    /** @brief Describes the only admitted Vulkan stereo coverage reduction stage and descriptor ABI. */
    [[nodiscard]] ShaderManifest StereoCoverageReductionManifest();

    /** @brief Returns the exact compiler resource/member names for the closed reduction stage. */
    [[nodiscard]] std::vector<ShaderReflectionBindingName> StereoCoverageReductionReflectionNames();

    /** @brief Validates the packaged compute entry and its bound, exact reflected layout.
     * @param payload Canonical Vulkan stage package from the closed cooker.
     * @param metadata Exact same-operation sidecar bytes.
     * @return Bound reflection or a typed package, entry or layout failure. Performs no device work.
     */
    [[nodiscard]] Result<ReflectedShaderArtifact> ValidateStereoCoverageReductionCook(std::span<const std::uint8_t> payload,
                                                                                      std::span<const std::uint8_t> metadata);

    /** @brief Validates a bounded float-register raster interface before invoking native translators.
     * @details Supports one read-only uniform block of float4/float4x4 fields and literal arrays,
     * with at most one ordinary sampled texture/sampler pair on OpenGL; Metal admits up to eight independent
     * ordinary sampled images and eight independent samplers. Multi-resource Metal evidence establishes
     * exact independent native bindings, not inferred image/sampler usage pairs. This validates logical inputs only;
     * it provides no native offsets, slots, shader compatibility or execution evidence.
     */
    [[nodiscard]] Result<void> ValidateRegisterRasterReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                std::span<const ShaderReflectionBindingName> names);

    /** @brief Reconciles same-operation Cross JSON and native raster declarations with the exact authored IDs.
     * @details Only the bounded register profile is admitted. Placements and packing must be proven by
     * both reflection and actual native declarations before canonical target normalization.
     */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeRegisterRasterReflection(const ShaderManifest &manifest,
                                                                                       const ShaderTargetRequirement &target,
                                                                                       std::span<const SpirvReflectionStage> stages,
                                                                                       std::span<const std::string> translations,
                                                                                       std::span<const ShaderReflectionBindingName> names);

    /** @brief Reads sorted unique actual vertex-output or fragment-input locations for finite GL renaming.
     * @details Reads bounded exact-entry compiler JSON; never invents authored identities or placements.
     */
    [[nodiscard]] Result<std::vector<std::uint32_t>> RegisterRasterVaryingLocations(const SpirvReflectionStage &stage);

    /** @brief Normalizes bounded official SPIRV-Cross JSON against exact authored IDs and the manifest contract.
     * @details The named closed compute profiles require exact entry, descriptor ABI, native output representation,
     * uniform packing and fixed thread group dimensions.
     */
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
