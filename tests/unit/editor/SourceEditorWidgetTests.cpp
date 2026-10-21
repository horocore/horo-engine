#include "Horo/Editor/SourceFileOpenService.h"
#include "SourceEditorTheme.h"
#include "SourceEditorWidget.h"
#include "SourceSyntax.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <array>
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
                                                  const bool enabled = true, const Theme::Fonts *fonts = nullptr) {
            gui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({700, 500});
            ImGui::Begin("Widget test", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
            const auto intent = widget.Draw({620, 400, scale, focus, enabled}, fonts);
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

    /** @brief Counts actual rendered source-glyph color vertices rather than inspecting a detached lexical result. */
    std::size_t ColoredVertices(const SourceSyntaxRole role) {
        const auto theme = ResolveSourceEditorTheme(Theme::GetActiveTokens());
        const auto &rgba = theme.styles[static_cast<std::size_t>(role)].color;
        const auto color = ImGui::GetColorU32(ImVec4{rgba[0], rgba[1], rgba[2], rgba[3]});
        std::size_t count = 0;
        const auto *data = ImGui::GetDrawData();
        for (int list = 0; list < data->CmdListsCount; ++list)
            for (const auto &vertex : data->CmdLists[list]->VtxBuffer)
                count += vertex.col == color ? 1 : 0;
        return count;
    }

    /** @brief Checks that a supplied font face contributes real glyph UVs to the draw output. */
    bool DrawnGlyph(const ImFontGlyph &glyph) {
        const auto *data = ImGui::GetDrawData();
        for (int list = 0; list < data->CmdListsCount; ++list)
            for (const auto &vertex : data->CmdLists[list]->VtxBuffer)
                if (vertex.uv.x >= glyph.U0 && vertex.uv.x <= glyph.U1 && vertex.uv.y >= glyph.V0 && vertex.uv.y <= glyph.V1)
                    return true;
        return false;
    }
}  // namespace

TEST_CASE("Zep widget emits exact Unicode edits while preserving BOM and mixed newline bytes", "[editor][source][widget]") {
    WidgetFixture fixture;
    const auto original = fixture.Open("code.cpp", "\xEF\xBB\xBF"
                                                   "one\r\ntwo\nthree\r");
    fixture.first.Publish(original);
    static_cast<void>(fixture.Frame(fixture.first));
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

TEST_CASE("Every open widget draws lexical roles from the immediately active theme", "[editor][source][widget][syntax][theme]") {
    WidgetFixture fixture;
    Theme::RefreshThemeList();
    Theme::SelectThemeByIndex(0);
    const auto a = fixture.Open("a.cpp", "\xEF\xBB\xBF"
                                         "// Türkçe\r\nconst char* label = \"text\";\r\n");
    const auto b = fixture.Open("b.lua", "local label = \"text\"\n");
    fixture.first.Publish(a);
    fixture.second.Publish(b);
    REQUIRE(fixture.first.Highlight(a.Revision(), TokenizeSource(SourceLanguage::Cpp, a.Text())));
    REQUIRE(fixture.second.Highlight(b.Revision(), TokenizeSource(SourceLanguage::HoroScript, b.Text())));
    for (auto *widget : {&fixture.first, &fixture.second}) {
        static_cast<void>(fixture.Frame(*widget));
        static_cast<void>(fixture.Frame(*widget));
        REQUIRE(ColoredVertices(SourceSyntaxRole::Keyword) > 0);
        REQUIRE(ColoredVertices(SourceSyntaxRole::String) > 0);
    }
    Theme::SelectThemeByIndex(2);
    // No Publish or Highlight call: a palette switch must reach both retained mirrors on their next draw.
    for (auto *widget : {&fixture.first, &fixture.second}) {
        static_cast<void>(fixture.Frame(*widget, false, 2.0F));
        REQUIRE(ColoredVertices(SourceSyntaxRole::Keyword) > 0);
        REQUIRE(ColoredVertices(SourceSyntaxRole::String) > 0);
    }
    REQUIRE(fixture.first.HasHighlight(a.Revision()));
    REQUIRE(fixture.second.HasHighlight(b.Revision()));
    Theme::SelectThemeByIndex(0);
}

TEST_CASE("Widget fallback stays readable and draws bounded visible glyphs for large files and long lines",
          "[editor][source][widget][syntax]") {
    WidgetFixture fixture;
    Theme::RefreshThemeList();
    Theme::SelectThemeByIndex(0);
    std::string large = "const int value = 1;\n";
    while (large.size() <= SourceSyntaxLimits{}.maximumBytes)
        large += "const int value = 1;\n";
    const std::array inputs{
        std::pair<std::string, std::string>{"long.cpp", "const " + std::string(65'537, 'x')},
        std::pair<std::string, std::string>{"large.cpp", std::move(large)},
        std::pair<std::string, std::string>{"malformed.cpp", "const char* s = \"unclosed"},
        std::pair<std::string, std::string>{"readme.txt", "const ordinary readable text"},
    };
    for (const auto &[name, text] : inputs) {
        const auto source = fixture.Open(name.c_str(), text);
        const auto syntax = TokenizeSource(ClassifySourceLanguage(name), source.Text());
        REQUIRE(syntax.fallback != SourceSyntaxFallback::None);
        fixture.first.Publish(source);
        REQUIRE(fixture.first.Highlight(source.Revision(), syntax));
        static_cast<void>(fixture.Frame(fixture.first, false));
        static_cast<void>(fixture.Frame(fixture.first, false));
        REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
        REQUIRE(ImGui::GetDrawData()->TotalVtxCount < 50'000);
        REQUIRE(ColoredVertices(SourceSyntaxRole::Keyword) == 0);
        REQUIRE(ColoredVertices(SourceSyntaxRole::Text) > 0);
        REQUIRE(source.Text() == text);
    }
}

TEST_CASE("Widget lexical replacement validates revisions and preserves the previous projection on invalid input",
          "[editor][source][widget][syntax]") {
    WidgetFixture fixture;
    const auto source = fixture.Open("code.cpp", "const int value = 1;\r\n");
    fixture.first.Publish(source);
    const auto syntax = TokenizeSource(SourceLanguage::Cpp, source.Text());
    REQUIRE_FALSE(fixture.first.HasHighlight(source.Revision()));
    REQUIRE_FALSE(fixture.first.Highlight(source.Revision() + 1, syntax));
    REQUIRE(fixture.first.Highlight(source.Revision(), syntax));
    SourceSyntax invalid{.spans = {{0, source.Text().size() + 1, SourceSyntaxRole::Keyword}}};
    REQUIRE_FALSE(fixture.first.Highlight(source.Revision(), invalid));
    REQUIRE(fixture.first.HasHighlight(source.Revision()));
    static_cast<void>(fixture.Frame(fixture.first));
    ImGui::GetIO().AddInputCharactersUTF8("Q");
    REQUIRE(fixture.Frame(fixture.first));
    REQUIRE_FALSE(fixture.first.HasHighlight(source.Revision()));
    REQUIRE_FALSE(fixture.first.Highlight(source.Revision(), syntax));
    fixture.first.Publish(source);
    REQUIRE(fixture.first.Highlight(source.Revision(), syntax));
    fixture.first.Publish({});
    REQUIRE_FALSE(fixture.first.HasHighlight(source.Revision()));
}

TEST_CASE("Source semantic font roles use the supplied regular and emphasis atlas faces", "[editor][source][widget][theme]") {
    WidgetFixture fixture;
    auto *atlas = ImGui::GetIO().Fonts;
    Theme::Fonts fonts;
    fonts.monospace = atlas->AddFontDefault();
    fonts.monospaceEmphasis = atlas->AddFontDefault();
    REQUIRE(atlas->Build());
    const auto source = fixture.Open("code.cpp", "const char* value = \"c\";");
    fixture.first.Publish(source);
    REQUIRE(fixture.first.Highlight(source.Revision(), TokenizeSource(SourceLanguage::Cpp, source.Text())));
    static_cast<void>(fixture.Frame(fixture.first, false, 1, true, &fonts));
    static_cast<void>(fixture.Frame(fixture.first, false, 1, true, &fonts));
    const auto *regular = fonts.monospace->FindGlyphNoFallback('c');
    const auto *emphasis = fonts.monospaceEmphasis->FindGlyphNoFallback('c');
    REQUIRE(regular);
    REQUIRE(emphasis);
    REQUIRE(DrawnGlyph(*regular));
    REQUIRE(DrawnGlyph(*emphasis));
    REQUIRE(fonts.monospace->CalcTextSizeA(18, 1000, 1000, "iiii").x == fonts.monospaceEmphasis->CalcTextSizeA(18, 1000, 1000, "iiii").x);
}
