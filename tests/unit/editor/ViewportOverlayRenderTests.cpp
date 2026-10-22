#include "Horo/Editor/EditorTheme.h"
#include "ViewportRenderTestSupport.h"
#include "editor/screens/workspace/panels/viewport/ViewportOverlay.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <utility>

using TestLocalization = Horo::Tests::ViewportRenderLocalization;

TEST_CASE("Viewport compass selects both signed ends of each axis", "[unit][editor][viewport]") {
    using namespace Horo::Editor;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {900.0F, 600.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    ImFont *font = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    TestLocalization localization;
    ViewportOverlayState state;
    state.camera.position = {4.0F, 3.0F, 5.0F};
    const Horo::Math::Vec3 forward = Horo::Math::TryNormalize(state.camera.target - state.camera.position).Value();
    const Horo::Math::Vec3 right = Horo::Math::TryNormalize(Horo::Math::Cross(forward, state.camera.up)).Value();
    const Horo::Math::Vec3 up = Horo::Math::Cross(right, forward);
    const auto draw = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({900.0F, 600.0F});
        ImGui::Begin("CompassTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const ViewportOverlayAction action = DrawViewportOverlay({0.0F, 0.0F}, {800.0F, 500.0F}, state, fonts, localization);
        ImGui::End();
        ImGui::Render();
        return action;
    };
    constexpr std::array endpoints{
        std::pair{Horo::Math::Vec3{1.0F, 0.0F, 0.0F}, EditorViewportAxisView::PositiveX},
        std::pair{Horo::Math::Vec3{-1.0F, 0.0F, 0.0F}, EditorViewportAxisView::NegativeX},
        std::pair{Horo::Math::Vec3{0.0F, 1.0F, 0.0F}, EditorViewportAxisView::PositiveY},
        std::pair{Horo::Math::Vec3{0.0F, -1.0F, 0.0F}, EditorViewportAxisView::NegativeY},
        std::pair{Horo::Math::Vec3{0.0F, 0.0F, 1.0F}, EditorViewportAxisView::PositiveZ},
        std::pair{Horo::Math::Vec3{0.0F, 0.0F, -1.0F}, EditorViewportAxisView::NegativeZ},
    };
    for (const auto &[axis, expected] : endpoints) {
        io.AddMousePosEvent(752.0F + Horo::Math::Dot(axis, right) * 29.0F, 64.0F - Horo::Math::Dot(axis, up) * 29.0F);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(draw());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(draw());
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        REQUIRE(draw().axisView == expected);
    }
    ImGui::DestroyContext();
}

TEST_CASE("Viewport grid control follows shading control", "[unit][editor][viewport]") {
    using namespace Horo::Editor;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {900.0F, 600.0F};
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());
    ImFont *font = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    TestLocalization localization;
    ViewportOverlayState state;
    const auto draw = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::SetNextWindowSize({900.0F, 600.0F});
        ImGui::Begin("GridTest", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const ViewportOverlayAction action = DrawViewportOverlay({0.0F, 0.0F}, {800.0F, 500.0F}, state, fonts, localization);
        ImGui::End();
        ImGui::Render();
        return action;
    };

    io.AddMousePosEvent(299.0F, 34.0F);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    REQUIRE(draw().toggleGrid);
    ImGui::DestroyContext();
}
