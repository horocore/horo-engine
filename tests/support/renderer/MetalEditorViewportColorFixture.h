#pragma once

/** @file MetalEditorViewportColorFixture.h @brief Actual full Metal viewport host shared by bounded native color/depth tests. */
#include "Horo/Runtime/Render/ColorPipeline.h"
#include "Horo/Runtime/Render/RenderScene.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Editor {
    class EditorViewportRendererMetal;
}

namespace Horo::Editor::Test {
    /** @brief Explicitly selected genuine program family; neither mode silently falls back to another. */
    enum class ViewportColorLighting {
        Baseline,
        Clustered
    };

    /** @brief Completed native attachment copies, not CPU-rendered pixels or exposure verdicts. */
    struct ViewportColorPixels final {
        Render::FramebufferExtent extent;
        std::vector<std::uint16_t> rawScene;
        std::vector<std::uint8_t> encoded;
        /** @brief Decodes one actual FP16 scene pixel. @param x Column. @param y Row. @return Native copied RGB. */
        [[nodiscard]] std::array<float, 3> Scene(std::uint32_t x, std::uint32_t y) const;
        /** @brief Reads one actual final RGBA8 pixel. @param x Column. @param y Row. @return Native copied bytes. */
        [[nodiscard]] std::array<std::uint8_t, 4> Final(std::uint32_t x, std::uint32_t y) const;
    };

    /** @brief Owns an actual selected frontend and the full production viewport adapter, with finite native teardown. */
    class MetalEditorViewportColorFixture final {
    public:
        /** @brief Loads genuine color/exposure and the explicit raster family. @param lighting Requested actual recipe.
         * @param look Optional explicit nonidentity asset interpolation; absent disables creative look selection. */
        explicit MetalEditorViewportColorFixture(ViewportColorLighting lighting, std::optional<Render::ColorLutInterpolation> look = {});
        /** @brief Drains the genuine frontend before destroying its borrowed viewport/native parents. */
        ~MetalEditorViewportColorFixture();
        MetalEditorViewportColorFixture(const MetalEditorViewportColorFixture &) = delete;
        MetalEditorViewportColorFixture &operator=(const MetalEditorViewportColorFixture &) = delete;
        /** @brief Executes one actual mesh/overlay/color graph and accepts only a successful native Present.
         * @param scene Canonical CPU mesh/light snapshot retained throughout the call. @param grid Actual grid selection. */
        void Render(const Render::RenderSceneView &scene, bool grid = false);
        /** @brief Copies actual completed scene/final attachments through admitted copy-readable storage. @return Real pixels. */
        [[nodiscard]] ViewportColorPixels Read();
        /** @brief Borrows the production adapter for explicit authored settings/diagnostics. @return Existing host-owned adapter. */
        [[nodiscard]] EditorViewportRendererMetal &Viewport() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor::Test
