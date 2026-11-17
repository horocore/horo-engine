#pragma once
/** @file EditorViewportShaderParameters.h @brief Canonical HLSL viewport parameter packing without frame allocation. */
#include "Horo/Runtime/Render/ClusteredLightGrid.h"
#include "Horo/Runtime/Render/DirectionalShadow.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"

#include <array>

namespace Horo::Editor {
    /** @brief Admits one exact canonical role and its same-boundary artifact before native preparation. */
    [[nodiscard]] Result<const Render::CompiledShaderArtifact *> ViewportReloadArtifact(const Render::ShaderHotReloadProgramView &view,
                                                                                        Render::ShaderTargetBackend backend,
                                                                                        std::size_t role);

    /** @brief Returns the explicitly interpreted fixed Editor material in canonical scene-linear AP1.
     * @return Immutable prepared float4 RGB/1; source legacy-linearRec709D65V1 is converted once before lighting.
     */
    [[nodiscard]] std::array<float, 4> EditorViewportFixedMaterialBaseColor() noexcept;

    struct EditorViewportShaderParameters final {
        Math::Mat4 mvp{Math::Mat4::Identity()};
        Math::Mat4 model{Math::Mat4::Identity()};
        std::array<float, 64> shadow{};
        std::array<float, 16> cascadeSplits{};
        std::array<float, 16> atlasRects{};
        std::array<float, 4> cameraForward{};
        std::array<float, 4> shadowBiasFilter{};
        std::array<float, 4> camera{};
        std::array<float, 4> selection{};
        std::array<float, 4> shadowSettings{};
        std::array<float, 4> shadowCoordinates{};
        std::array<float, 4> baseColor = EditorViewportFixedMaterialBaseColor();
        bool unexposedAcesCg{}; /**< Explicit host recipe: true emits unexposed AP1; false retains legacy display shading. */
        std::array<std::array<float, 64>, 4> lights{};
        /** @brief Captures the bounded canonical 16-light scene and current backend shadow coordinates. */
        void SetScene(const Render::RenderSceneView &scene, const std::optional<Render::DirectionalShadowFrame> &shadowFrame,
                      Render::ShaderTargetBackend backend) noexcept;
        /** @brief Packs complete lit parameters through the exact accepted reflected layout. */
        [[nodiscard]] Result<std::size_t> PackLit(const Render::MaterialBindingLayout &layout, std::span<std::byte> destination) const;
    };

    /** @brief Fixed, reflected uniform input for the real storage-backed clustered lit consumer. */
    struct EditorViewportClusteredShaderParameters final {
        EditorViewportShaderParameters common;
        std::array<float, 4> dimensions{};
        std::array<float, 4> depth{};
        std::array<float, 4> viewport{};
        std::array<float, 4> shadowIdentity{};
        /** @brief Captures exact producer/grid identities after complete-table admission; retains no borrowed data. */
        [[nodiscard]] Result<void> SetScene(const Render::RenderSceneView &scene, const Render::ClusteredLightGrid &grid,
                                            const std::optional<Render::DirectionalShadowFrame> &shadowFrame,
                                            std::uint32_t referencesPerCluster);
        /** @brief Packs only uniform binding 1 through its exact accepted driver/compiler-reflected layout. */
        [[nodiscard]] Result<std::size_t> PackLit(const Render::MaterialBindingLayout &layout, std::span<std::byte> destination) const;
    };

    /** @brief Uses camera-relative instance translation with the exact cascade projection. */
    [[nodiscard]] Math::Mat4 EditorViewportShadowMvp(const Render::DirectionalShadowFrame &frame, std::size_t cascade,
                                                     const Math::Mat4 &model) noexcept;

    /** @brief Packs one grid/visualizer draw through its accepted reflected layout. */
    [[nodiscard]] Result<std::size_t> PackEditorViewportGrid(const Render::MaterialBindingLayout &layout, const Math::Mat4 &mvp,
                                                             Math::Vec3 color, std::span<std::byte> destination, float alpha = 1.0F);
    /** @brief Packs one directional depth draw through its accepted reflected layout. */
    [[nodiscard]] Result<std::size_t> PackEditorViewportShadow(const Render::MaterialBindingLayout &layout, const Math::Mat4 &mvp,
                                                               std::span<std::byte> destination);
}  // namespace Horo::Editor
