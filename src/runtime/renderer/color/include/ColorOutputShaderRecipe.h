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
    /** @brief Declares an exact product shader; currently only the implemented Metal target is admitted.
     * @param stage Product operation. @param target Explicit target descriptor.
     * @return Logical recipe or typed unsupported/invalid input. No compilation or realization is implied.
     */
    [[nodiscard]] Result<ShaderManifest> MakeColorOutputShaderManifest(ColorOutputShaderStage stage, const ShaderTargetRequirement &target);
    /** @brief Describes the exact canonical symbols used to reconcile genuine native compiler evidence.
     * @param stage Product operation. @return Names in logical binding order; empty for invalid stage.
     */
    [[nodiscard]] std::vector<ShaderReflectionBindingName> ColorOutputShaderReflectionNames(ColorOutputShaderStage stage);
}  // namespace Horo::Render
