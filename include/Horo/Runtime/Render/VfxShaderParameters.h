#pragma once

/** @file VfxShaderParameters.h
 * @brief Bounded VFX shader semantics adapted to the canonical reflected material writer.
 */
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/VfxDrawData.h"

namespace Horo::Render {
    /** @brief Logical identities assigned when cooking the shipped VFX shader. */
    namespace VfxShaderParameterIds {
        inline constexpr ShaderBindingId Instances{1};
        inline constexpr ShaderBindingId Material{2};
        inline constexpr ShaderBindingId BaseColor{3};
        inline constexpr ShaderBindingId BaseColorSampler{4};
        inline constexpr ShaderParameterId ClipFromLocal{1};
        inline constexpr ShaderParameterId InstanceColor{2};
        inline constexpr ShaderParameterId Tint{3};
        inline constexpr ShaderParameterId AlphaCutoff{4};
    }  // namespace VfxShaderParameterIds

    /** @brief Defines the shipped VFX shader's logical interface independently of native placement.
     * @param target Explicit selected target requirement participating in cook identity.
     * @return Canonical manifest for VSMain/PSMain; optimization may deactivate declared resources.
     * @details Native slots, member offsets, array strides and matrix order are supplied only by validated
     * compiler/translator reflection. Callers migrate from handwritten fixture manifests to this authority.
     */
    [[nodiscard]] ShaderManifest MakeVfxShaderManifest(const ShaderTargetRequirement &target);

    /** @brief Admits the exact reflected instance schema before allocating native frame slots.
     * @param layout Immutable final-target material reflection, including the separate material resources.
     * @return Required instance-block prefix size or typed invalid schema/packing failure.
     * @details The selected adapter must additionally admit this size against its actual uniform-block limits.
     */
    [[nodiscard]] Result<std::size_t> VfxShaderParameterBytes(const MaterialBindingLayout &layout);

    /** @brief Writes one bounded draw's instance block without allocation or native work.
     * @param layout Exact reflected layout admitted for the retained material generation.
     * @param instances One through MaximumVfxDrawInstances canonical finite records in actual draw order.
     * @param destination Owner-retained frame storage, disjoint from all borrowed inputs.
     * @return Written prefix extent or typed failure; failure preserves every destination byte.
     * @details Converts canonical column-major matrices to MaterialParameterValue's logical row-major input.
     * The existing material writer alone applies native matrix order, offsets, strides and padding. Unused
     * instance lanes are zero. The owner must retain the resulting bytes through actual native upload/record.
     */
    [[nodiscard]] Result<std::size_t> PackVfxShaderParameters(const MaterialBindingLayout &layout,
                                                              std::span<const VfxPackedDrawInstance> instances,
                                                              std::span<std::byte> destination);
}  // namespace Horo::Render
