#pragma once
/** @file ColorOutputShaderRecipe.h
 * @brief Non-installed product raster interfaces; native offsets/slots come only from real cooked reflection.
 */
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

namespace Horo::Render {
    /** @brief Separate scene-to-display-linear and final composition-to-sRGB operations. */
    enum class ColorOutputShaderStage : std::uint8_t {
        SceneToDisplayLinear,
        EncodeSdr,
        SceneToDisplayLinearWithLook
    };
    /** @brief Declares the exact Vulkan manual scene/SDR route or the existing Metal product family.
     * @param stage Product operation. @param target Explicit target descriptor.
     * @return Logical recipe or typed unsupported/invalid input. No compilation or realization is implied.
     */
    [[nodiscard]] Result<ShaderManifest> MakeColorOutputShaderManifest(ColorOutputShaderStage stage, const ShaderTargetRequirement &target);
    /** @brief Describes the exact canonical symbols used to reconcile genuine native compiler evidence.
     * @param stage Product operation. @param backend Explicit compiled target; Vulkan uses the manual-only recipe.
     * @return Names in logical binding order; empty for an unsupported operation or backend.
     */
    [[nodiscard]] std::vector<ShaderReflectionBindingName> ColorOutputShaderReflectionNames(
        ColorOutputShaderStage stage, ShaderTargetBackend backend = ShaderTargetBackend::Metal);
}  // namespace Horo::Render
