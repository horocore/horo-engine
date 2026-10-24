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
}  // namespace Horo::Render
