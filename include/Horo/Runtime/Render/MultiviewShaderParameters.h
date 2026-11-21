#pragma once
/** @file MultiviewShaderParameters.h
 * @brief Reflected view-indexed shader parameter admission and allocation-free matrix packing.
 */
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewMotionHistory.h"
#include "Horo/Runtime/Render/RenderScene.h"

#include <array>
#include <optional>

namespace Horo::Render {

    /** @brief Canonical logical IDs for the shipped runtime multiview mesh shader. */
    namespace MultiviewShaderParameterIds {
        inline constexpr ShaderBindingId Buffer{1};
        inline constexpr std::array PreviousEyeDepthBindings{ShaderBindingId{2}, ShaderBindingId{3}};
        inline constexpr ShaderParameterId WorldToClip{1};
        inline constexpr ShaderParameterId LocalToWorld{2};
        inline constexpr ShaderParameterId ViewLayers{3};
        inline constexpr ShaderParameterId CurrentLocalToClip{4};
        inline constexpr ShaderParameterId PreviousLocalToClip{5};
        inline constexpr ShaderParameterId NormalToWorld{6};
        inline constexpr ShaderParameterId EnvironmentAmbient{7};
        inline constexpr ShaderParameterId EnvironmentDirectional{8};
        inline constexpr ShaderParameterId EnvironmentDirection{9};
    }  // namespace MultiviewShaderParameterIds

    /** @brief Optional renderer-owned lighting for one multiview presentation, in scene-linear ACEScg and World axes. */
    struct MultiviewEnvironmentLighting final {
        std::optional<Math::Vec3> ambient;
        std::optional<RenderLight> directional;

        /** @brief Validates the supplied lobes, without inventing a fallback source. @return True for bounded valid values. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Writes actual view/object/routing parameters into an owner-retained frame slot without allocation.
     * @param layout Exact final-artifact reflected layout. @param views Complete ordered current runtime views.
     * @param localToWorld Canonical instance transform. @param layers Exact target layer per view.
     * @param destination Bounded scratch retained through the actual frontend upload/record operation.
     * @return Written prefix extent or original semantic/packing error; failure preserves all destination bytes.
     * @details Uses the SAME MaterialBinding writer as the allocating overload. No normal-frame compile, heap allocation,
     * native call, independent reflection interpretation or clock is introduced. Padding is zero; any suffix is untouched.
     */
    [[nodiscard]] Result<std::size_t> PackMultiviewShaderParameters(const MaterialBindingLayout &layout,
                                                                    std::span<const MultiviewSceneView> views,
                                                                    const Math::Mat4 &localToWorld, std::span<const std::uint32_t> layers,
                                                                    std::span<std::byte> destination);

    /** @brief Validates the exact bounded final-artifact matrix/array layout.
     * @param layout Canonical reflected immutable material layout.
     * @param viewCount Complete active runtime view count.
     * @return Required uniform bytes including tail alignment, or typed invalid/unsupported/capacity failure. */
    [[nodiscard]] Result<std::size_t> MultiviewShaderParameterBytes(const MaterialBindingLayout &layout, std::size_t viewCount);

    /** @brief Validates the separate environment-lit program's full reflected layout.
     * @param layout Final-artifact reflection. @param viewCount Number of exact active views.
     * @return Required bytes or a typed layout failure.
     */
    [[nodiscard]] Result<std::size_t> MultiviewEnvironmentParameterBytes(const MaterialBindingLayout &layout, std::size_t viewCount);

    /** @brief Writes the opt-in environment-lit variant, with an empty lighting value giving baseline unlit output.
     * @param layout Exact final environment program reflection. @param views Ordered active view matrices.
     * @param localToWorld Canonical instance affine transform. @param layers Ordered target layer routing.
     * @param lighting Current authority-scoped renderer inputs, or empty for neutral fallback.
     * @param destination Existing bounded frame scratch.
     * @return Written bytes or typed admission failure without partial writes. No allocation on success.
     */
    [[nodiscard]] Result<std::size_t> PackMultiviewEnvironmentParameters(
        const MaterialBindingLayout &layout, std::span<const MultiviewSceneView> views, const Math::Mat4 &localToWorld,
        std::span<const std::uint32_t> layers, const MultiviewEnvironmentLighting &lighting, std::span<std::byte> destination);

    /** @brief Checks the distinct multipass motion or stereo coverage program's uniform block.
     * @param layout Exact program layout. @param viewCount Active ordered view count.
     * @return Required bytes or typed invalid/unsupported layout.
     */
    [[nodiscard]] Result<std::size_t> MultiviewMotionParameterBytes(const MaterialBindingLayout &layout, std::size_t viewCount);

    /** @brief Packs only owner-validated exact current and previous clip transforms.
     * @param layout Final motion shader layout. @param history Selected existing render history owner.
     * @param frame Exact staged frame. @param instanceIndex Exact index in the staged scene.
     * @param destination Bounded upload buffer scratch.
     * @return Written prefix or a typed failure that leaves destination untouched when any visible eye lacks real history.
     */
    [[nodiscard]] Result<std::size_t> PackMultiviewMotionShaderParameters(const MaterialBindingLayout &layout,
                                                                          const MultiviewMotionHistory &history,
                                                                          const MultiviewMotionFrame &frame, std::uint32_t instanceIndex,
                                                                          std::span<std::byte> destination);

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
