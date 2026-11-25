#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Runtime/Ui/UiCaptionSettings.h"
#include "ViewportRenderTestSupport.h"
#include "editor/screens/workspace/panels/viewport/ViewportOverlay.h"
#include "editor/screens/workspace/panels/viewport/ViewportPlayCaptions.h"
#include "editor_ui/HeadlessEditorGuiFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <utility>

using TestLocalization = Horo::Tests::ViewportRenderLocalization;

namespace {
    /** @brief Checks the default caption safe area, including the antialias fringe. */
    void CheckCaptionPlacement(const ImDrawList &draw, int firstVertex, ImVec2 origin, ImVec2 extent) {
        constexpr float antialiasFringe = 1.0F;
        for (int index = firstVertex; index < draw.VtxBuffer.Size; ++index) {
            const auto point = draw.VtxBuffer[index].pos;
            CHECK(point.x >= origin.x + 0.05F * extent.x - antialiasFringe);
            CHECK(point.x <= origin.x + 0.95F * extent.x + antialiasFringe);
            CHECK(point.y >= origin.y + 0.50F * extent.y);
            CHECK(point.y <= origin.y + 0.95F * extent.y + antialiasFringe);
        }
    }

    /** @brief Loads a translation long enough to distinguish it from the authored fallback. */
    void ActivateCaptionTranslation(Horo::Editor::LocalizationService &localization) {
        using namespace Horo::Editor;
        LocalizationCatalog catalog{.locale = LocaleTag{"en-US"}};
        catalog.messages.emplace(MessageKey{"game.dialogue", "hi"}, "Welcome back.");
        REQUIRE(localization.RegisterCatalog(std::move(catalog)));
        REQUIRE(localization.Prepare(LocaleTag{"en-US"}));
        REQUIRE(localization.ActivatePrepared());
    }
}  // namespace

TEST_CASE("Desktop Play caption preview draws localized dialogue inside the viewport", "[unit][editor][viewport][captions]") {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;
    ConfigurationSchema schema;
    for (const auto &setting : MakeCaptionSettingsContribution().settings)
        REQUIRE(schema.Register(setting).HasValue());
    REQUIRE(schema.Seal().HasValue());
    ConfigurationService disabled{schema};
    ConfigurationResolutionRequest settings;
    settings.user.emplace(SettingKey{"accessibility.captions.enabled"}, ConfigurationInputValue{true, {}});
    ConfigurationService enabled{std::move(schema), nullptr, settings};
    LocalizationService localization{Horo::Editor::LocaleTag{"en-US"}};
    const auto message = LocalizedMessageRef::Create(UiMessageKey::Create("game.dialogue", "hi").Value(), "Hi").Value();
    auto queue = std::move(CaptionCueQueue::Create(1, {message})).Value();
    CaptionTimeline timeline;
    REQUIRE(timeline.Reset(1).HasValue());
    REQUIRE(queue->TryEnqueue({{1, 1}, CaptionCueKind::Dialogue, 0, 0, 10, 20}) == CaptionAdmission::Accepted);
    REQUIRE(timeline.Drain(*queue) == 0);

    const Horo::Editor::Tests::HeadlessEditorGuiFixture gui{{900, 600}};
    gui.BeginFrame();
    auto &draw = *ImGui::GetForegroundDrawList();
    const auto &fonts = gui.Fonts();
    const ImVec2 origin{30, 20};
    const ImVec2 extent{800, 500};
    const auto vertices = [&] { return draw.VtxBuffer.Size; };
    const int initial = vertices();
    DrawViewportPlayCaptions(draw, origin, extent, fonts, {timeline, enabled, localization});
    CHECK(vertices() == initial);

    timeline.BeginFrame(10);
    DrawViewportPlayCaptions(draw, origin, extent, fonts, {timeline, disabled, localization});
    DrawViewportPlayCaptions(draw, origin, {2, 2}, fonts, {timeline, enabled, localization});
    CHECK(vertices() == initial);
    DrawViewportPlayCaptions(draw, origin, extent, fonts, {timeline, enabled, localization});
    const int fallback = vertices();
    REQUIRE(fallback > initial);
    CheckCaptionPlacement(draw, initial, origin, extent);
    ActivateCaptionTranslation(localization);
    DrawViewportPlayCaptions(draw, origin, extent, fonts, {timeline, enabled, localization});
    CHECK(vertices() - fallback > fallback - initial);

    const int translated = vertices();
    timeline.BeginFrame(30);
    DrawViewportPlayCaptions(draw, origin, extent, fonts, {timeline, enabled, localization});
    CHECK(vertices() == translated);
    REQUIRE(timeline.Reset(0).HasValue());
    CHECK(timeline.NextSessionId() == 2);
    gui.EndFrame();
}

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
    // The same pointer gesture is inert until the workspace offers the local agent route.
    const auto row = ResolveViewportStatusRow({0.0F, 0.0F}, {800.0F, 500.0F}, state.objectCount, localization);
    io.AddMousePosEvent(row.actionsPosition.x + 20.0F, row.actionsPosition.y + row.height * 0.5F);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK_FALSE(draw().reviewWithAgent);
    state.canReviewWithAgent = true;
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(draw());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK(draw().reviewWithAgent);
    ImGui::DestroyContext();
}
