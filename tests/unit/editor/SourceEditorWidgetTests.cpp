#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/Platform.h"
#include "SourceEditorSession.h"
#include "SourceEditorWidget.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <imgui_internal.h>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Drives the actual Zep adapter with real immutable service publications and ImGui input frames. */
    struct WidgetFixture final {
        Editor::Tests::HeadlessEditorGuiFixture gui;
        Horo::Tests::OwnedTestDirectory directory{"source-widget"};
        SourceFileOpenService documents{directory.Path()};
        SourceEditorWidget first{SourceEditingViewId{1}};
        SourceEditorWidget second{SourceEditingViewId{2}};
        std::string clipboard;

        WidgetFixture() {
            ImGui::GetIO().ConfigMacOSXBehaviors = false;
            ImGui::GetPlatformIO().Platform_ClipboardUserData = &clipboard;
            ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext *context) {
                return static_cast<std::string *>(context->PlatformIO.Platform_ClipboardUserData)->c_str();
            };
            ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext *context, const char *text) {
                *static_cast<std::string *>(context->PlatformIO.Platform_ClipboardUserData) = text;
            };
        }

        SourceDocumentSnapshot Open(const char *name, const std::string_view text) {
            {
                std::ofstream file(directory.Path() / name, std::ios::binary);
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
                REQUIRE(file.good());
            }
            const auto opened = documents.Open({.path = name, .mode = SourceOpenMode::EmbeddedOnly});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot);
            return *opened.Value().sourceSnapshot;
        }

        std::optional<SourceEditingCommand> Frame(SourceEditorWidget &widget, const bool focus = true, const float scale = 1.0F,
                                                  const bool enabled = true) {
            gui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({700, 500});
            ImGui::Begin("Widget test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
            const auto intent = widget.Draw({620, 400, scale, focus, enabled});
            ImGui::End();
            gui.EndFrame();
            return intent;
        }

        SourceDocumentSnapshot Apply(const SourceEditingCommand &intent) {
            const auto *edit = std::get_if<SourceEditSubmission>(&intent);
            REQUIRE(edit);
            const auto result = SubmitSourceEditingCommand(documents.Documents(), *edit);
            REQUIRE(result.HasValue());
            return result.Value().snapshot;
        }

        std::optional<SourceEditingCommand> Shortcut(SourceEditorWidget &widget, const ImGuiKey key) {
            auto &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(key, true);
            const auto intent = Frame(widget);
            io.AddKeyEvent(key, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            static_cast<void>(Frame(widget));
            return intent;
        }
    };
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
    fixture.gui.BeginFrame();
    ImGui::Begin("Two source views");
    static_cast<void>(fixture.first.Draw({320, 250, 1, false, true}));
    ImGui::SameLine();
    static_cast<void>(fixture.second.Draw({320, 250, 2, false, true}));
    ImGui::End();
    fixture.gui.EndFrame();
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

TEST_CASE("Widget undo is an outward document intent and never changes the local text", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "abc");
    fixture.first.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
    const auto undo = fixture.Shortcut(fixture.first, ImGuiKey_Z);
    REQUIRE(undo);
    const auto *request = std::get_if<SourceHistoryRequest>(&*undo);
    REQUIRE(request);
    CHECK(request->instance == original.Identity().instance);
    CHECK(request->expectedRevision == original.Revision());
    CHECK(request->direction == SourceHistoryDirection::Undo);
    CHECK_FALSE(fixture.Frame(fixture.first));
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
