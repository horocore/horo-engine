#pragma once
/** @file MultiviewShaderParameters.h
 * @brief Reflected view-indexed shader parameter admission and allocation-free matrix packing.
 */
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"

namespace Horo::Render {

    /** @brief Canonical logical IDs for the shipped runtime multiview mesh shader. */
    namespace MultiviewShaderParameterIds {
        inline constexpr ShaderBindingId Buffer{1};
        inline constexpr ShaderParameterId WorldToClip{1};
        inline constexpr ShaderParameterId LocalToWorld{2};
        inline constexpr ShaderParameterId ViewLayers{3};
    }  // namespace MultiviewShaderParameterIds

    /** @brief Validates the exact bounded final-artifact matrix/array layout.
     * @param layout Canonical reflected immutable material layout.
     * @param viewCount Complete active runtime view count.
     * @return Required uniform bytes including tail alignment, or typed invalid/unsupported/capacity failure. */
    [[nodiscard]] Result<std::size_t> MultiviewShaderParameterBytes(const MaterialBindingLayout &layout, std::size_t viewCount);

    /** @brief Adapts immutable multiview values to the canonical material parameter packer.
     * @param layout Exact final-artifact layout admitted for the selected pipeline.
     * @param views Complete dense ordered views from the canonical XR conversion.
     * @param localToWorld One canonical instance transform.
     * @param layers Exact target layers for instanced routing, or zero for single-layer multipass views.
     * @return Owned packed bytes or original typed packing failure. Run before active-frame admission;
     * upload bytes through the existing frontend buffer owner and bind that uniform generation.
     * @details Matrix order, strides, tails, overlaps and padding have one authority: PackMaterialParameterBinding.
     */
    [[nodiscard]] Result<MaterialParameterBinding> PackMultiviewShaderParameters(const MaterialBindingLayout &layout,
                                                                                 std::span<const MultiviewSceneView> views,
                                                                                 const Math::Mat4 &localToWorld,
                                                                                 std::span<const std::uint32_t> layers);
}  // namespace Horo::Render
