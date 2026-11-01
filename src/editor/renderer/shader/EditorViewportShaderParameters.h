#pragma once
/** @file EditorViewportShaderParameters.h @brief Canonical HLSL viewport parameter packing without frame allocation. */
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"

#include <array>

namespace Horo::Editor {
    /** @brief Admits one exact canonical role and its same-boundary artifact before native preparation. */
    [[nodiscard]] Result<const Render::CompiledShaderArtifact *> ViewportReloadArtifact(const Render::ShaderHotReloadProgramView &view,
                                                                                        Render::ShaderTargetBackend backend,
                                                                                        std::size_t role);

    struct EditorViewportShaderParameters final {
        Math::Mat4 mvp{Math::Mat4::Identity()}, model{Math::Mat4::Identity()}, shadow{Math::Mat4::Identity()};
        std::array<float, 4> camera{}, selection{}, shadowSettings{}, shadowCoordinates{};
        std::array<std::array<float, 64>, 4> lights{};
        /** @brief Captures the bounded canonical 16-light scene and current backend shadow coordinates. */
        void SetScene(const Render::RenderSceneView &scene, const Math::Mat4 &shadowViewProjection, std::size_t shadowLightIndex,
                      bool shadowEnabled, Render::ShaderTargetBackend backend) noexcept;
        /** @brief Packs complete lit parameters through the exact accepted reflected layout. */
        [[nodiscard]] Result<std::size_t> PackLit(const Render::MaterialBindingLayout &layout, std::span<std::byte> destination) const;
    };

    /** @brief Packs one grid/visualizer draw through its accepted reflected layout. */
    [[nodiscard]] Result<std::size_t> PackEditorViewportGrid(const Render::MaterialBindingLayout &layout, const Math::Mat4 &mvp,
                                                             Math::Vec3 color, std::span<std::byte> destination);
    /** @brief Packs one directional depth draw through its accepted reflected layout. */
    [[nodiscard]] Result<std::size_t> PackEditorViewportShadow(const Render::MaterialBindingLayout &layout, const Math::Mat4 &mvp,
                                                               std::span<std::byte> destination);
}  // namespace Horo::Editor
