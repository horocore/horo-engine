#include "editor/renderer/vulkan/EditorViewportVulkanCanvasProjection.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests {
    TEST_CASE("Vulkan canvas corners use the same world plane and depth as the selected viewport", "[unit][editor][vulkan][canvas]") {
        using Render::UiRenderCompositionSpace;
        constexpr Render::FramebufferExtent extent{1200, 600};
        const Render::RenderCameraView camera{.position = {0.4F, 0.5F, 6.0F}, .target = {0.0F, 0.1F, -1.0F}};
        const Math::Mat4 worldFromCanvas{{1.9F, 0.2F, 0.3F, 0, -0.2F, 1.4F, -0.2F, 0, 0, 0, 1, 0, 0.15F, 0.1F, -0.75F, 1}};
        const auto canvas = Editor::BuildVulkanEditorCanvasMvp(UiRenderCompositionSpace::World, worldFromCanvas, camera, extent);
        REQUIRE(canvas.HasValue());
        const auto world = Editor::BuildRenderMvp(camera, Math::Mat4::Identity(), 2.0F, Math::ClipDepthRange::ZeroToOne);
        REQUIRE(world.HasValue());

        constexpr std::array expectedCorners{Math::Vec3{-0.9F, 0.7F, -1.0F}, Math::Vec3{1.0F, 0.9F, -0.7F}, Math::Vec3{-0.7F, -0.7F, -0.8F},
                                             Math::Vec3{1.2F, -0.5F, -0.5F}};
        constexpr std::array localCorners{Math::Vec3{-0.5F, 0.5F, 0}, Math::Vec3{0.5F, 0.5F, 0}, Math::Vec3{-0.5F, -0.5F, 0},
                                          Math::Vec3{0.5F, -0.5F, 0}};
        for (std::size_t index = 0; index < localCorners.size(); ++index) {
            const auto expected = Math::TransformPoint(world.Value(), expectedCorners[index]);
            const auto actual = Math::TransformPoint(canvas.Value(), localCorners[index]);
            CHECK(Math::NearlyEqual(actual, Math::Vec3{expected.x, -expected.y, expected.z}, 1e-5F));
        }
    }

    TEST_CASE("Vulkan camera canvas maps the complete view and rejects incompatible placement", "[unit][editor][vulkan][canvas]") {
        using enum Render::UiRenderCompositionSpace;
        constexpr Render::FramebufferExtent extent{600, 400};
        const Render::RenderCameraView camera;
        const auto cameraCanvas = Editor::BuildVulkanEditorCanvasMvp(Camera, std::nullopt, camera, extent);
        REQUIRE(cameraCanvas.HasValue());
        CHECK(Math::NearlyEqual(Math::TransformPoint(cameraCanvas.Value(), {-0.5F, 0.5F, 0}), Math::Vec3{-1.0F, -1.0F, 0}));
        CHECK(Math::NearlyEqual(Math::TransformPoint(cameraCanvas.Value(), {0.5F, -0.5F, 0}), Math::Vec3{1.0F, 1.0F, 0}));

        const Math::Mat4 degenerate{};
        CHECK(Editor::BuildVulkanEditorCanvasMvp(World, degenerate, camera, extent).HasError());
        CHECK(Editor::BuildVulkanEditorCanvasMvp(World, std::nullopt, camera, extent).HasError());
        CHECK(Editor::BuildVulkanEditorCanvasMvp(Camera, degenerate, camera, extent).HasError());
        CHECK(Editor::BuildVulkanEditorCanvasMvp(Camera, std::nullopt, camera, {}).HasError());
    }

    TEST_CASE("Vulkan canvas clip adaptation agrees with both asymmetric XR viewports", "[unit][editor][vulkan][canvas]") {
        struct Eye {
            Math::Mat4 clip;
            float y;
            float height;
        };

        constexpr std::array eyes{Eye{Math::Mat4{{1.2F, 0.1F, 0, 0, 0, 1.4F, 0, 0, 0.1F, -0.1F, 1, -1, -0.15F, 0.1F, 0.7F, 1.4F}}, 8, 940},
                                  Eye{Math::Mat4{{1.1F, -0.1F, 0, 0, 0, 1.2F, 0, 0, -0.15F, 0.2F, 1, -1, 0.18F, -0.1F, 0.75F, 1.5F}}, 20,
                                      876}};
        constexpr std::array corners{Math::Vec4{-0.5F, 0.5F, 0, 1}, Math::Vec4{0.5F, -0.5F, 0, 1}};
        for (const auto &eye : eyes) {
            const auto adapted = Editor::FlipVulkanCanvasClipY(eye.clip);
            for (const auto corner : corners) {
                const auto scene = Math::TransformHomogeneous(eye.clip, corner);
                const auto canvas = Math::TransformHomogeneous(adapted, corner);
                const auto scenePixel = eye.y + (1.0F - scene.y / scene.w) * eye.height * 0.5F;
                const auto canvasPixel = eye.y + (1.0F + canvas.y / canvas.w) * eye.height * 0.5F;
                CHECK(canvasPixel == scenePixel);
                CHECK(canvas.x == scene.x);
                CHECK(canvas.z == scene.z);
                CHECK(canvas.w == scene.w);
            }
        }
    }
}  // namespace Horo::Tests
