#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/Platform.h"
#include "SourceEditorSession.h"
#include "SourceEditorTools.h"
#include "SourceEditorWidget.h"
#include "SourceSyntax.h"
#include "SourceWidgetPlatform.h"
#include "editor/input/EditorInputActions.h"
#include "helpers/editor_ui/SourceEditorWidgetFixture.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <imgui_internal.h>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    using WidgetFixture = Horo::Editor::Tests::SourceEditorWidgetFixture;
}  // namespace

TEST_CASE("Zep widget emits exact Unicode edits while preserving BOM and mixed newline bytes", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "\xEF\xBB\xBF"
                                                   "one\r\ntwo\nthree\r");
    fixture.first.Publish(original);
    // Initial publication must bind the mode before any frame or selection call.
    CHECK(fixture.first.Selection() == SourceWidgetSelection{3, 3});
    REQUIRE(fixture.first.Select({3, 6}));
    ImGui::GetIO().AddInputCharactersUTF8("ğ中😀");
    const auto intent = fixture.Frame(fixture.first);
    REQUIRE(intent);
    const auto edited = fixture.Apply(*intent);
    CHECK(edited.Text() == "\xEF\xBB\xBF"
                           "ğ中😀\r\ntwo\nthree\r");
    CHECK(original.Text() == "\xEF\xBB\xBF"
                             "one\r\ntwo\nthree\r");
    fixture.first.Publish(edited);
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK_FALSE(fixture.first.Select({1, 3}));
    CHECK_FALSE(fixture.first.Select({4, 5}));
}

TEST_CASE("Zep views render different publications and retain independent selection", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto a = fixture.Open("a.cpp", "alpha\n");
    const auto b = fixture.Open("b.cpp", "bravo\n");
    fixture.first.Publish(a);
    fixture.second.Publish(b);
    REQUIRE(fixture.first.Select({1, 3}));
    REQUIRE(fixture.second.Select({2, 4}));
    for (unsigned frame = 0; frame < 2; ++frame) {
        fixture.gui.BeginFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({700, 400});
        ImGui::Begin("Two source views", nullptr, ImGuiWindowFlags_NoSavedSettings);
        static_cast<void>(fixture.first.Draw({320, 250, 1, false, true}));
        ImGui::SameLine();
        static_cast<void>(fixture.second.Draw({320, 250, 2, false, true}));
        ImGui::End();
        fixture.gui.EndFrame();
    }
    CHECK(fixture.first.Selection() == SourceWidgetSelection{1, 3});
    CHECK(fixture.second.Selection() == SourceWidgetSelection{2, 4});
    REQUIRE(ImGui::GetDrawData());
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    fixture.first.Scroll(-10);
    CHECK_FALSE(fixture.Frame(fixture.first, true, 2));
    CHECK(fixture.second.Selection() == SourceWidgetSelection{2, 4});
}

TEST_CASE("Zep clipboard uses the host and rejected edits reconcile to the owner", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "hello\r\n");
    fixture.first.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
    static_cast<void>(fixture.Shortcut(fixture.first, ImGuiKey_A));
    static_cast<void>(fixture.Shortcut(fixture.first, ImGuiKey_C));
    CHECK(fixture.clipboard == "hello\r\n");
    fixture.clipboard = "世界\r\n";
    const auto intent = fixture.Shortcut(fixture.first, ImGuiKey_V);
    REQUIRE(intent);
    const auto edited = fixture.Apply(*intent);
    CHECK(edited.Text() == "世界\r\n");
    // The host rejected this presentation edit: the unchanged authoritative root wins.
    fixture.first.Publish(original);
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(fixture.documents.Documents().Snapshot(original.Identity().instance).Value().Text() == edited.Text());
    fixture.first.Publish(edited);
    static_cast<void>(fixture.Frame(fixture.first));
}

TEST_CASE("Widget leaves history shortcuts to the host registry without changing text", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "abc");
    fixture.first.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
    CHECK_FALSE(fixture.Shortcut(fixture.first, ImGuiKey_Z));
    CHECK(fixture.documents.Documents().Snapshot(original.Identity().instance).Value().Revision() == original.Revision());
}

TEST_CASE("Widget focus disabled and teardown paths do not mutate documents", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "text");
    fixture.first.Publish(source);
    fixture.second.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    CHECK(ImGui::GetCurrentContext()->PlatformImeData.WantVisible);
    ImGui::GetIO().AddInputCharactersUTF8("ignored");
    CHECK_FALSE(fixture.Frame(fixture.first, false, 1, false));
    CHECK_FALSE(ImGui::GetCurrentContext()->PlatformImeData.WantVisible);
    static_cast<void>(fixture.Frame(fixture.second));
    ImGui::GetIO().AddInputCharactersUTF8("Q");
    REQUIRE(fixture.Frame(fixture.second));
    CHECK_FALSE(fixture.Frame(fixture.first, false));
    fixture.first.Publish({});
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(source.Text() == "text");
    CHECK_FALSE(fixture.Frame(fixture.second, false, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("Widget decorations are revision fenced and validated before replacement", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "int value;\r\n");
    fixture.first.Publish(source);
    const std::array decorations{SourceWidgetDecoration{0, 3, SourceWidgetDecorationKind::Information, "type"}};
    CHECK(fixture.first.Decorate(source.Revision(), decorations));
    CHECK_FALSE(fixture.first.Decorate(source.Revision() + 1, decorations));
    const std::array invalid{SourceWidgetDecoration{10, 11, SourceWidgetDecorationKind::Error, "split CRLF"}};
    CHECK_FALSE(fixture.first.Decorate(source.Revision(), invalid));
    CHECK_FALSE(fixture.Frame(fixture.first));
}

TEST_CASE("Mouse selection releases outside the widget and keyboard movement stays view local", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "alpha bravo charlie\nsecond line\n");
    fixture.first.Publish(source);
    fixture.second.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({0, 0}));
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_RightArrow, true);
    CHECK_FALSE(fixture.Frame(fixture.first));
    io.AddKeyEvent(ImGuiKey_RightArrow, false);
    static_cast<void>(fixture.Frame(fixture.first));
    CHECK(fixture.first.Selection().caret == 1);
    CHECK(fixture.second.Selection().caret == 0);
    io.AddMousePosEvent(110, 70);
    static_cast<void>(fixture.Frame(fixture.first));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(fixture.Frame(fixture.first));
    io.AddMousePosEvent(240, 70);
    static_cast<void>(fixture.Frame(fixture.first));
    io.AddMousePosEvent(1100, 700);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK_FALSE(io.MouseDown[ImGuiMouseButton_Left]);
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Revision() == source.Revision());
}

TEST_CASE("Widget destruction releases callbacks after its GUI context has gone", "[editor][source][widget]") {
    auto widget = std::make_unique<SourceEditorWidget>(SourceEditingViewId{99});
    SourceDocumentSnapshot held;
    {
        WidgetFixture fixture;
        held = fixture.Open("code.cpp", "lease");
        widget->Publish(held);
        CHECK_FALSE(fixture.Frame(*widget));
    }
    CHECK(ImGui::GetCurrentContext() == nullptr);
    widget.reset();
    CHECK(held.Text() == "lease");
}

TEST_CASE("Widget history restores only the originating host issued view", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "abc\n");
    fixture.first.Publish(original);
    fixture.second.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({1, 1}));
    REQUIRE(fixture.second.Select({2, 2}));
    ImGui::GetIO().AddInputCharactersUTF8("X");
    const auto intent = fixture.Frame(fixture.first);
    REQUIRE(intent);
    const auto edited = fixture.Apply(*intent);
    fixture.first.Publish(edited);
    fixture.second.Publish(edited);
    const SourceEditingCommand undo = SourceHistoryRequest{original.Identity().instance, edited.Revision(), SourceHistoryDirection::Undo};
    const auto undone = SubmitSourceEditingCommand(fixture.documents.Documents(), undo);
    REQUIRE(undone.HasValue());
    fixture.first.Publish(undone.Value().snapshot);
    fixture.second.Publish(undone.Value().snapshot);
    CHECK(fixture.first.Restore(undone.Value()));
    CHECK_FALSE(fixture.second.Restore(undone.Value()));
    CHECK(fixture.first.Selection() == SourceWidgetSelection{1, 1});
    CHECK(fixture.second.Selection() == SourceWidgetSelection{2, 2});
    CHECK_FALSE(fixture.first.Restore(undone.Value()));
    SourceEditorWidget reopened{SourceEditingViewId{3}};
    reopened.Publish(undone.Value().snapshot);
    CHECK_FALSE(reopened.Restore(undone.Value()));
}

TEST_CASE("Widget Enter and Tab consume the canonical semantic editing planners", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "  line\r\n");
    fixture.first.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({6, 6}));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    const auto newline = fixture.Frame(fixture.first);
    REQUIRE(newline);
    auto edited = fixture.Apply(*newline);
    CHECK(edited.Text() == "  line\r\n  \r\n");
    fixture.first.Publish(edited);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({0, 0}));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, true);
    const auto indent = fixture.Frame(fixture.first);
    REQUIRE(indent);
    edited = fixture.Apply(*indent);
    CHECK(edited.Text().starts_with("      line"));
}

TEST_CASE("Reversed selections retain their orientation and invalid clipboard bytes are inert", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "abcdef");
    fixture.first.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({4, 1}));
    CHECK(fixture.first.Selection() == SourceWidgetSelection{4, 1});
    fixture.clipboard = std::string{"\xC0\xAF", 2};
    CHECK_FALSE(fixture.Shortcut(fixture.first, ImGuiKey_V));
    CHECK(fixture.first.Selection() == SourceWidgetSelection{4, 1});
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Text() == "abcdef");
}

TEST_CASE("An open workspace popup blocks the actual source input and IME focus", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "unchanged");
    fixture.first.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    ImGui::GetIO().AddInputCharactersUTF8("blocked");
    fixture.gui.BeginFrame();
    ImGui::Begin("Popup owner");
    ImGui::OpenPopup("workspace popup");
    if (ImGui::BeginPopup("workspace popup")) {
        ImGui::TextUnformatted("popup");
        ImGui::EndPopup();
    }
    const auto intent = fixture.first.Draw({620, 400, 1, true, true});
    ImGui::End();
    fixture.gui.EndFrame();
    CHECK_FALSE(intent);
    CHECK_FALSE(ImGui::GetCurrentContext()->PlatformImeData.WantVisible);
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Text() == "unchanged");
}

TEST_CASE("First focused frame owns no-op Delete and navigation without relying on next-frame IME flags", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("empty.cpp", "");
    fixture.first.Publish(source);
    ImGui::GetIO().WantTextInput = false;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true);
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(fixture.first.OwnsInput());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(fixture.first.OwnsInput());
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Revision() == source.Revision());
}

TEST_CASE("Collapsed midline Tab inserts at the caret to the next indentation stop", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "abcd\r\n");
    fixture.first.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({1, 1}));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, true);
    const auto intent = fixture.Frame(fixture.first);
    REQUIRE(intent);
    const auto edited = fixture.Apply(*intent);
    CHECK(edited.Text() == "a   bcd\r\n");
    const auto &plan = std::get<SourceEditSubmission>(*intent).plan;
    CHECK(plan.offset == 1);
    CHECK(plan.after.caret == 4);
}

TEST_CASE("Actual source viewport captures keyboard scrolling and restores more than one thousand lines", "[editor][source][widget]") {
    WidgetFixture fixture;
    std::string text;
    for (unsigned line = 0; line < 5000; ++line)
        text += "line\n";
    const auto source = fixture.Open("long.cpp", text);
    fixture.first.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_PageDown, true);
    static_cast<void>(fixture.Frame(fixture.first));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_PageDown, false);
    static_cast<void>(fixture.Frame(fixture.first));
    const auto keyboard = fixture.first.View();
    CHECK(keyboard.scrollLines > 0);
    fixture.first.Scroll(2000);
    static_cast<void>(fixture.Frame(fixture.first));
    CHECK(fixture.first.View().scrollLines == Catch::Approx(keyboard.scrollLines + 2000));
    SourceEditingPublication restored{source, SourceEditingView{0, 0, 2111, fixture.first.Identity()}, {}};
    REQUIRE(fixture.first.Restore(restored));
    static_cast<void>(fixture.Frame(fixture.first));
    CHECK(fixture.first.View().scrollLines == Catch::Approx(2111));
    static_cast<void>(fixture.Frame(fixture.first, true, 2));
    CHECK(fixture.first.View().scrollLines == Catch::Approx(2111));
    ImGui::GetIO().AddMousePosEvent(200, 120);
    static_cast<void>(fixture.Frame(fixture.first, true, 2));
    ImGui::GetIO().AddMouseWheelEvent(0, -2);
    static_cast<void>(fixture.Frame(fixture.first, true, 2));
    CHECK(fixture.first.View().scrollLines == Catch::Approx(2121));
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Revision() == source.Revision());
}

TEST_CASE("Source presentation owner fences reused document IDs while retaining view state across Save As", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto first = fixture.Open("first.cpp", "first");
    SourceEditorSession session;
    auto *view = session.Acquire(first.Identity().instance);
    REQUIRE(view);
    view->widget.Publish(first);
    static_cast<void>(fixture.Frame(view->widget));
    REQUIRE(view->widget.Select({1, 3}));
    const auto oldViewId = view->widget.Identity();
    NativeDurableFileSystem files;
    const auto saved = fixture.documents.SaveAs({first.Identity().instance, first.Revision(), {}}, "renamed.cpp", files);
    REQUIRE(saved.HasValue());
    view->widget.Publish(saved.Value().snapshot);
    CHECK(view->widget.Identity() == oldViewId);
    CHECK(view->widget.Selection() == SourceWidgetSelection{1, 3});
    fixture.Open("second.cpp", "second");
    SourceFileOpenService replacement{fixture.directory.Path()};
    const auto reopened = replacement.Open({.path = "second.cpp", .mode = SourceOpenMode::EmbeddedOnly});
    REQUIRE(reopened.HasValue());
    REQUIRE(reopened.Value().sourceSnapshot);
    const auto next = *reopened.Value().sourceSnapshot;
    REQUIRE(next.Identity().instance == first.Identity().instance);
    session.Reset();
    auto *fresh = session.Acquire(next.Identity().instance);
    REQUIRE(fresh);
    fresh->widget.Publish(next);
    CHECK(fresh->widget.Identity().value > oldViewId.value);
    CHECK(fresh->widget.Selection() == SourceWidgetSelection{0, 0});
    CHECK(session.NeedsFocus(next.Identity().instance));
}

TEST_CASE("Native Backspace and Delete emit scalar aligned document edits", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "a😀b");
    fixture.first.Publish(source);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({6, 6}));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
    const auto backspace = fixture.Frame(fixture.first);
    REQUIRE(backspace);
    const auto edited = fixture.Apply(*backspace);
    CHECK(edited.Text() == "a😀");
    fixture.first.Publish(edited);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({1, 1}));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true);
    const auto erase = fixture.Frame(fixture.first);
    REQUIRE(erase);
    CHECK(fixture.Apply(*erase).Text() == "a");
}

TEST_CASE("Source focus hands pointer input back to a shared text control", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "unchanged");
    fixture.first.Publish(source);
    std::string query;
    ImVec2 target;
    bool hovered{};
    const auto frame = [&](const bool focus) {
        fixture.gui.BeginFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({700, 500});
        ImGui::Begin("Source focus handoff", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        static_cast<void>(Ui::InputTextControl("##query", query, 64, Theme::Fonts{}));
        target = ImGui::GetItemRectMin();
        target.x += 20;
        target.y += 10;
        hovered = ImGui::IsItemHovered();
        CHECK_FALSE(fixture.first.Draw({620, 350, 1, focus, true}));
        ImGui::End();
        fixture.gui.EndFrame();
    };
    frame(true);
    REQUIRE(fixture.first.OwnsInput());
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(target.x, target.y);
    frame(false);
    REQUIRE(hovered);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(false);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(false);
    REQUIRE_FALSE(fixture.first.OwnsInput());
    io.AddInputCharactersUTF8("query");
    frame(false);
    CHECK(query == "query");
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Revision() == source.Revision());
    CHECK(source.Text() == "unchanged");
    frame(true);
    CHECK(fixture.first.OwnsInput());
}

TEST_CASE("Dragging through several source lines selects bytes without creating a document edit", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original =
        fixture.Open("drag.horo_script",
                     "alpha bravo charlie\nsecond line here\nthird line here\nfourth line here\nfifth line here\nlast line\n");
    fixture.first.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(110, 50);
    static_cast<void>(fixture.Frame(fixture.first));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    CHECK_FALSE(fixture.Frame(fixture.first));
    io.AddMousePosEvent(220, 110);
    CHECK_FALSE(fixture.Frame(fixture.first));
    const auto selected = fixture.first.Selection();
    REQUIRE(selected.anchor != selected.caret);
    const auto low = std::min(selected.anchor, selected.caret);
    const auto high = std::max(selected.anchor, selected.caret);
    CHECK(original.Text().substr(low, high - low).find('\n') != std::string_view::npos);
    io.AddMousePosEvent(1100, 700);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK_FALSE(fixture.Frame(fixture.first));
    CHECK(fixture.first.Selection().anchor != fixture.first.Selection().caret);
    CHECK(fixture.documents.Documents().Snapshot(original.Identity().instance).Value().Revision() == original.Revision());
}

TEST_CASE("Source newline planning follows Horo Script blocks and ignores strings and comments", "[editor][source][widget][syntax]") {
    WidgetFixture fixture;
    const auto block = fixture.Open("block.horo_script", "    on_update = function(ctx, dt)");
    const auto at = block.Text().size();
    const auto plan = PlanSourceLanguageNewline(block, {at, at, 0, {1}});
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().insert == "\n        ");
    const auto braces = fixture.Open("braces.horo_script", "return horo.behavior {\r\n}");
    const auto brace = braces.Text().find('{') + 1;
    const auto bracePlan = PlanSourceLanguageNewline(braces, {brace, brace, 0, {1}});
    REQUIRE(bracePlan.HasValue());
    CHECK(bracePlan.Value().insert == "\r\n    ");
    const auto comment = fixture.Open("comment.horo_script", "    -- function(ctx) {");
    const auto commentEnd = comment.Text().size();
    REQUIRE(PlanSourceLanguageNewline(comment, {commentEnd, commentEnd, 0, {1}}).Value().insert == "\n    ");
    const auto string = fixture.Open("string.horo_script", "    local x = \"function {\"");
    const auto stringEnd = string.Text().size();
    REQUIRE(PlanSourceLanguageNewline(string, {stringEnd, stringEnd, 0, {1}}).Value().insert == "\n    ");
    const auto paired = fixture.Open("paired.horo_script", "local x = {}");
    const auto inner = paired.Text().find('{') + 1;
    const auto pairedPlan = PlanSourceLanguageNewline(paired, {inner, inner, 0, {1}});
    REQUIRE(pairedPlan.HasValue());
    CHECK(pairedPlan.Value().insert == "\n    \n");
    CHECK(pairedPlan.Value().after.caret == inner + 5);
}

TEST_CASE("Source tools resolve Cmd zoom and remapped history through the shared registry", "[editor][source][widget][keymap]") {
    const bool mac = GENERATE(true, false);
    Horo::Editor::Tests::EditorGuiContextFixture gui;
    Horo::Tests::OwnedTestDirectory directory{"source-keymap"};
    std::ofstream(directory.Path() / "code.cpp") << "hello";
    SourceFileOpenService documents{directory.Path()};
    const auto opened = documents.Open({.path = "code.cpp", .mode = SourceOpenMode::EmbeddedOnly});
    REQUIRE(opened.HasValue());
    const auto source = *opened.Value().sourceSnapshot;
    SourceEditorWidget widget{SourceEditingViewId{9}};
    SourceEditorTools tools;
    auto &io = ImGui::GetIO();
    io.ConfigMacOSXBehaviors = mac;
    const auto modifier = mac ? ImGuiMod_Super : ImGuiMod_Ctrl;
    const auto frame = [&](const bool enabled = true) {
        gui.imgui.BeginFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({700, 500});
        ImGui::Begin("Source commands");
        widget.Publish(source);
        auto intent = tools.Draw(widget, source, {.undoEntries = 1}, {}, gui.context);
        static_cast<void>(
            widget.Draw({620, 400, 1, true, enabled && !intent, &gui.settings.settings.keymap.profile, 18, tools.HandledShortcut()},
                        &gui.theme.fonts));
        ImGui::End();
        gui.imgui.EndFrame();
        return intent;
    };
    frame();
    frame();
    const auto chord = [&](const ImGuiKey key, const bool enabled = true) {
        io.AddKeyEvent(modifier, true);
        frame(enabled);
        io.AddKeyEvent(key, true);
        auto intent = frame(enabled);
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(modifier, false);
        frame(enabled);
        frame(enabled);
        return intent;
    };
    CHECK_FALSE(chord(ImGuiKey_Equal));
    CHECK(widget.FontZoom() == 1);
    CHECK_FALSE(chord(ImGuiKey_Minus));
    CHECK(widget.FontZoom() == 0);
    gui.settings.settings.keymap.profile.overrides.push_back(
        {Input::ActionId{kActionSourceZoomIn},
         {{.kind = Input::BindingControlKind::Key, .key = Input::Key::F2, .requiredModifiers = {.control = !mac, .command = mac}}}});
    CHECK_FALSE(chord(ImGuiKey_Equal));
    CHECK(widget.FontZoom() == 0);
    CHECK_FALSE(chord(ImGuiKey_F2));
    CHECK(widget.FontZoom() == 1);
    CHECK_FALSE(chord(ImGuiKey_0));
    CHECK(widget.FontZoom() == 0);
    gui.settings.settings.keymap.profile.overrides.push_back(
        {Input::ActionId{kActionUndo},
         {{.kind = Input::BindingControlKind::Key, .key = Input::Key::U, .requiredModifiers = {.control = !mac, .command = mac}}}});
    auto &zoomBinding = gui.settings.settings.keymap.profile.overrides.front().bindings.front();
    zoomBinding.key = Input::Key::Q;
    zoomBinding.requiredModifiers = {};
    io.AddKeyEvent(ImGuiKey_Q, true);
    io.AddInputCharactersUTF8("q");
    CHECK_FALSE(frame());
    CHECK(widget.FontZoom() == 1);
    io.AddKeyEvent(ImGuiKey_Q, false);
    frame();
    widget.SetFontZoom(0);
    CHECK_FALSE(chord(ImGuiKey_Z));
    const auto undo = chord(ImGuiKey_U);
    REQUIRE(undo);
    const auto *request = std::get_if<SourceHistoryRequest>(&*undo);
    REQUIRE(request);
    CHECK(request->instance == source.Identity().instance);
    CHECK(request->direction == SourceHistoryDirection::Undo);
    frame(false);
    CHECK_FALSE(chord(ImGuiKey_F2, false));
    CHECK(widget.FontZoom() == 0);
    CHECK(documents.Documents().Snapshot(source.Identity().instance).Value().Revision() == source.Revision());
}

TEST_CASE("Live source display preferences preserve document bytes and selection", "[editor][source][widget][settings]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("view.lua", "first  = 1\nsecond = 2\n");
    fixture.first.Publish(source);
    REQUIRE(fixture.first.Select({0, 5}));
    CodeEditorSettings preferences;
    const auto draw = [&] {
        fixture.gui.BeginFrame();
        ImGui::SetNextWindowSize({700, 500});
        ImGui::Begin("Live source settings");
        const auto intent = fixture.first.Draw({.width = 620, .height = 400, .preferences = &preferences});
        ImGui::End();
        fixture.gui.EndFrame();
        CHECK_FALSE(intent);
        CHECK(fixture.first.Selection() == SourceWidgetSelection{0, 5});
        return ImGui::GetDrawData()->TotalVtxCount;
    };
    static_cast<void>(draw());
    const int ordinary = draw();
    preferences.showWhitespace = true;
    static_cast<void>(draw());
    CHECK(draw() > ordinary);
    preferences.showWhitespace = false;
    preferences.showLineNumbers = false;
    preferences.wordWrap = false;
    preferences.showScrollBar = false;
    static_cast<void>(draw());
    CHECK(draw() < ordinary);
    CHECK(fixture.documents.Documents().Snapshot(source.Identity().instance).Value().Text() == source.Text());
    CHECK_FALSE(source.Dirty());
}

TEST_CASE("Source font families select registered atlas faces and fall back without per-frame cache churn", "[editor][source][font]") {
    WidgetFixture fixture;
    auto fonts = fixture.gui.Fonts();
    auto *alternate = ImGui::GetIO().Fonts->AddFontDefault();
    REQUIRE(ImGui::GetIO().Fonts->Build());
    fonts.monospace = fonts.sans;
    fonts.codeFamilies.push_back({"Registered Mono", alternate, alternate});
    WidgetDisplay display;
    const auto theme = ResolveSourceEditorTheme(Theme::GetActiveTokens());
    const auto currentFace = [&]() {
        return static_cast<Zep::ZepFont_ImGui &>(display.GetFont(Zep::ZepTextType::Text)).GetImFont();
    };
    display.Configure(theme, &fonts, "Registered Mono");
    CHECK(currentFace() == alternate);
    const auto generation = display.FontGeneration();
    display.Configure(theme, &fonts, "Registered Mono");
    CHECK(currentFace() == alternate);
    CHECK(display.FontGeneration() == generation);
    display.Configure(theme, &fonts, "Unavailable Mono");
    CHECK(currentFace() == fonts.monospace);
    CHECK(display.FontGeneration() > generation);
}
