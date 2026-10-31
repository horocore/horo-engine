#include "UiCanvasPreviewUiFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Tests::CanvasUi;

    /** @brief Selection is produced by a real preview click, never a direct source command. */
    void SelectPanel(ImGuiTestContext &ui, Fixture &fixture) {
        ui.Yield(3);
        fixture.LogPresentation(ui);
        const auto center = Center(ui, fixture);
        if (ui.IsError())
            return;
        ui.MouseMoveToPos(center);
        ui.MouseDown();
        ui.Yield();
        ui.MouseUp();
        ui.Yield(2);
        IM_CHECK(fixture.Source().selection == fixture.project.Identity<UiElementId>(14));
        IM_CHECK(fixture.Source().selectedElement.has_value());
    }

    /** @brief Device and camera actions update actual workspace state while preserving the canonical source/history. */
    void Devices(ImGuiTestContext &ui, Fixture &fixture) {
        ui.Yield(3);
        const auto state = fixture.Source().state;
        const auto bytes = fixture.SavedBytes();
        if (!Choose(ui, fixture, "preview_device", "preview.phone", 1))
            return;
        IM_CHECK(fixture.Source().preview.state.device == UiCanvasPreviewDevice::Phone);
        IM_CHECK((fixture.Source().preview.layout->viewport.dpiScale == UiCanvasDeviceScale{3, 1}));
        IM_CHECK(fixture.Source().preview.layout->canvas.safeAreaInsets.top == 141);
        IM_CHECK(fixture.Source().preview.layout->profile == fixture.project.Identity<UiPresentationProfileId>(15));
        Click(ui, fixture, "preview.rotate");
        IM_CHECK((fixture.Source().preview.layout->viewport.pixelExtent == UiCanvasPixelExtent{2532, 1170}));
        IM_CHECK(fixture.Source().preview.layout->viewport.safeAreaInsets.left == 102);
        if (!Choose(ui, fixture, "preview_device", "preview.tablet", 2))
            return;
        IM_CHECK((fixture.Source().preview.layout->viewport.dpiScale == UiCanvasDeviceScale{2, 1}));
        if (!Choose(ui, fixture, "preview_device", "preview.desktop", 0))
            return;
        Click(ui, fixture, "preview.rotate");
        const auto zoom = fixture.Source().preview.state.zoomPercent;
        Click(ui, fixture, "preview.zoom_in");
        IM_CHECK_EQ(fixture.Source().preview.state.zoomPercent, zoom + 10);
        Click(ui, fixture, "preview.zoom_out");
        IM_CHECK_EQ(fixture.Source().preview.state.zoomPercent, zoom);
        IM_CHECK(fixture.Source().state == state);
        IM_CHECK(!fixture.Source().dirty && !fixture.Source().canUndo);
        IM_CHECK(fixture.SavedBytes() == bytes);
        static_cast<void>(Center(ui, fixture));
    }

    /** @brief The actual Move action commits once; visible history controls replay its complete typed presentation. */
    void MoveAndHistory(ImGuiTestContext &ui, Fixture &fixture) {
        SelectPanel(ui, fixture);
        if (ui.IsError())
            return;
        const auto original = *fixture.Source().selectedElement->presentation;
        if (!Choose(ui, fixture, "preview_tool", "preview.move", 1))
            return;
        Drag(ui, Center(ui, fixture), {7, 5});
        IM_CHECK_EQ(fixture.authoredCommands, 1U);
        IM_CHECK(fixture.Source().dirty && fixture.Source().canUndo);
        const auto moved = *fixture.Source().selectedElement->presentation;
        IM_CHECK(moved.layout.offsets != original.layout.offsets);
        IM_CHECK(moved.inlineStyle == original.inlineStyle);
        Click(ui, fixture, "undo");
        IM_CHECK(*fixture.Source().selectedElement->presentation == original);
        Click(ui, fixture, "redo");
        IM_CHECK(*fixture.Source().selectedElement->presentation == moved);
    }

    /** @brief Resize and anchor gestures share the same canonical owner and preserve unrelated presentation values. */
    void ResizeAndAnchors(ImGuiTestContext &ui, Fixture &fixture) {
        SelectPanel(ui, fixture);
        if (ui.IsError())
            return;
        const auto original = *fixture.Source().selectedElement->presentation;
        const auto count = fixture.authoredCommands;
        if (!Choose(ui, fixture, "preview_tool", "preview.resize", 2))
            return;
        Drag(ui, Center(ui, fixture), {6, 4});
        const auto resized = fixture.Layout();
        IM_CHECK(resized.width.kind == UiLengthKind::Dip && resized.height.kind == UiLengthKind::Dip);
        IM_CHECK_EQ(fixture.authoredCommands, count + 1);
        if (!Choose(ui, fixture, "preview_tool", "preview.anchors", 3))
            return;
        Drag(ui, Center(ui, fixture), {4, 3});
        IM_CHECK(fixture.Layout().anchors != resized.anchors);
        IM_CHECK(fixture.Source().selectedElement->presentation->inlineStyle == original.inlineStyle);
        IM_CHECK_EQ(fixture.authoredCommands, count + 2);
        Click(ui, fixture, "undo");
        IM_CHECK(fixture.Layout() == resized);
    }

    /** @brief Modifier clicks and middle drag exercise the real session-only guide/pan surface. */
    void GuidesAndPan(ImGuiTestContext &ui, Fixture &fixture) {
        SelectPanel(ui, fixture);
        if (ui.IsError())
            return;
        const auto state = fixture.Source().state;
        const auto guideClick = [&](const ImGuiKeyChord modifiers) {
            const auto center = Center(ui, fixture);
            if (ui.IsError())
                return;
            ui.KeyDown(modifiers);
            ui.MouseMoveToPos(center);
            ui.MouseDown();
            ui.MouseUp();
            ui.KeyUp(modifiers);
            ui.Yield(2);
        };
        guideClick(ImGuiMod_Shift);
        IM_CHECK_EQ(fixture.Source().preview.state.verticalGuideCount, 1U);
        guideClick(ImGuiMod_Ctrl | ImGuiMod_Shift);
        IM_CHECK_EQ(fixture.Source().preview.state.horizontalGuideCount, 1U);
        const auto pan = fixture.Source().preview.state.pan;
        Drag(ui, Center(ui, fixture), {8, 4}, ImGuiMouseButton_Middle);
        IM_CHECK(fixture.Source().preview.state.pan != pan);
        const auto snap = fixture.Source().preview.state.snap;
        Click(ui, fixture, "preview.snap");
        IM_CHECK(fixture.Source().preview.state.snap != snap);
        Click(ui, fixture, "preview.clear_guides");
        IM_CHECK_EQ(fixture.Source().preview.state.verticalGuideCount, 0U);
        IM_CHECK_EQ(fixture.Source().preview.state.horizontalGuideCount, 0U);
        IM_CHECK(fixture.Source().state == state);
        IM_CHECK_EQ(fixture.authoredCommands, 0U);
    }

    /** @brief A real click selects the last painted peer where equal same-parent layouts fully overlap. */
    void SelectOverlappingPeer(ImGuiTestContext &ui, Fixture &fixture) {
        ui.Yield(3);
        const auto &boxes = fixture.Source().preview.layout->boxes;
        const auto first = std::ranges::find(boxes, fixture.project.Identity<UiElementId>(14), &UiCanvasPreviewBox::element);
        const auto last = std::ranges::find(boxes, fixture.project.Identity<UiElementId>(16), &UiCanvasPreviewBox::element);
        IM_CHECK(first != boxes.end() && last != boxes.end());
        IM_CHECK(first->bounds == last->bounds);
        const auto state = fixture.Source().state;
        const auto start = Center(ui, fixture);
        if (ui.IsError())
            return;
        ui.MouseMoveToPos(start);
        ui.MouseDown();
        ui.MouseUp();
        ui.Yield(2);
        IM_CHECK(fixture.Source().selection == fixture.project.Identity<UiElementId>(16));
        IM_CHECK(fixture.Source().state == state);
        IM_CHECK_EQ(fixture.authoredCommands, 0U);
    }

    /** @brief A real view-size change invalidates the held gesture before release can author its old geometry. */
    void CancelOnResize(ImGuiTestContext &ui, Fixture &fixture) {
        const auto state = fixture.Source().state;
        const auto count = fixture.authoredCommands;
        const auto start = Center(ui, fixture);
        if (ui.IsError())
            return;
        ui.MouseMoveToPos(start);
        ui.MouseDown();
        ui.Yield();
        ui.MouseMoveToPos({start.x + 5, start.y + 3});
        fixture.SetExtent({fixture.extent.x, fixture.extent.y - 100});
        ui.Yield(2);
        ui.MouseUp();
        ui.Yield(2);
        IM_CHECK(fixture.Source().state == state);
        IM_CHECK_EQ(fixture.authoredCommands, count);
        fixture.SetExtent({fixture.extent.x, fixture.extent.y + 100});
        ui.Yield(2);
    }

    /** @brief Escape revokes a real held drag; save and reload retain the previously committed source. */
    void CancelSaveReload(ImGuiTestContext &ui, Fixture &fixture) {
        MoveAndHistory(ui, fixture);
        if (ui.IsError())
            return;
        const auto savedLayout = fixture.Layout();
        const auto state = fixture.Source().state;
        const auto count = fixture.authoredCommands;
        const auto start = Center(ui, fixture);
        if (ui.IsError())
            return;
        ui.MouseMoveToPos(start);
        ui.MouseDown();
        ui.Yield();
        ui.MouseMoveToPos({start.x + 10, start.y + 5});
        ui.KeyPress(ImGuiKey_Escape);
        ui.MouseUp();
        ui.Yield(2);
        IM_CHECK(fixture.Source().state == state);
        IM_CHECK_EQ(fixture.authoredCommands, count);
        CancelOnResize(ui, fixture);
        if (ui.IsError())
            return;
        if (!RevealToolbar(ui, fixture))
            return;
        const auto &save = fixture.localization.Get("editor", "workspace.documents.save");
        ui.ItemClick(("//**/" + save).c_str());
        ui.Yield(2);
        IM_CHECK(!fixture.Source().dirty);
        const auto bytes = fixture.SavedBytes();
        Click(ui, fixture, "reload");
        SelectPanel(ui, fixture);
        if (ui.IsError())
            return;
        IM_CHECK(fixture.Layout() == savedLayout);
        IM_CHECK(fixture.SavedBytes() == bytes);
    }
}  // namespace

TEST_CASE("Actual canvas preview controls select localized device evidence without authoring source", "[ui][imgui][editor][ui_canvas]") {
    Fixture fixture{GENERATE("en-US", "tr-TR")};
    const auto result = fixture.harness.Run("ui_canvas", "device_controls", [&](ImGuiTestContext *ui) {
        fixture.Frame(ui);
    }, [&](ImGuiTestContext *ui) {
        Devices(*ui, fixture);
    }, {}, &fixture.input);
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Actual canvas pointer move resize anchors and visible history use canonical transactions", "[ui][imgui][editor][ui_canvas]") {
    Fixture fixture{GENERATE("en-US", "tr-TR")};
    const auto result = fixture.harness.Run("ui_canvas", "layout_gestures", [&](ImGuiTestContext *ui) {
        fixture.Frame(ui);
    }, [&](ImGuiTestContext *ui) {
        MoveAndHistory(*ui, fixture);
        ResizeAndAnchors(*ui, fixture);
    }, {}, &fixture.input);
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Actual canvas modifier guides middle pan and snapping remain session only", "[ui][imgui][editor][ui_canvas]") {
    Fixture fixture{GENERATE("en-US", "tr-TR")};
    const auto result = fixture.harness.Run("ui_canvas", "session_gestures", [&](ImGuiTestContext *ui) {
        fixture.Frame(ui);
    }, [&](ImGuiTestContext *ui) {
        GuidesAndPan(*ui, fixture);
    }, {}, &fixture.input);
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Actual localized canvas drag cancellation save and reload preserve canonical values", "[ui][imgui][editor][ui_canvas]") {
    Fixture fixture{GENERATE("en-US", "tr-TR")};
    fixture.SetExtent({640, 900});
    const auto result = fixture.harness.Run("ui_canvas", "cancel_reload", [&](ImGuiTestContext *ui) {
        fixture.Frame(ui);
    }, [&](ImGuiTestContext *ui) {
        CancelSaveReload(*ui, fixture);
    }, {}, &fixture.input);
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Actual canvas selection follows reverse paint order for fully overlapping peers", "[ui][imgui][editor][ui_canvas]") {
    Fixture fixture{GENERATE("en-US", "tr-TR"), true};
    const auto result = fixture.harness.Run("ui_canvas", "overlapping_peers", [&](ImGuiTestContext *ui) {
        fixture.Frame(ui);
    }, [&](ImGuiTestContext *ui) {
        SelectOverlappingPeer(*ui, fixture);
    }, {}, &fixture.input);
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}
