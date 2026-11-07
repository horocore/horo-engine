#pragma once
/** @file ExposureGpuReflection.h @brief Exact offline evidence for the closed Metal exposure family. */
#include "Horo/Runtime/Render/ExposureGpuKernel.h"
#include "ShaderReflectionCook.h"

namespace Horo::Render {
    /** @brief Returns the exact authored Cross names for one canonical stage. */
    [[nodiscard]] std::vector<ShaderReflectionBindingName> ExposureGpuMetalReflectionNames(ExposureGpuStage stage);
    /** @brief Admits only canonical exposure interfaces and names on the Metal compute target. */
    [[nodiscard]] Result<void> ValidateExposureGpuMetalReflection(const ShaderManifest &, const ShaderTargetRequirement &,
                                                                  std::span<const ShaderReflectionBindingName>);
    /** @brief Qualifies only independently proven readonly native exposure inputs; preserves all other source bytes. */
    [[nodiscard]] Result<std::string> QualifyExposureGpuMetalInputs(const ShaderManifest &, std::string_view,
                                                                    std::span<const std::uint8_t> validatedSpirv);
    /** @brief Proves actual Cross JSON, selected native entry and validated SPIR-V before normalization. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeExposureGpuMetalReflection(
        const ShaderManifest &, const ShaderTargetRequirement &, std::span<const SpirvReflectionStage>, std::span<const std::string>,
        std::span<const ShaderReflectionBindingName>, std::span<const std::uint8_t> validatedSpirv);
}  // namespace Horo::Render
