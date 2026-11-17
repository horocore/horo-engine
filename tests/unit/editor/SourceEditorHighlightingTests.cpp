#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/Platform.h"
#include "SourceEditorSession.h"
#include "SourceEditorTheme.h"
#include "SourceEditorWidget.h"
#include "SourceSyntax.h"
#include "helpers/editor_ui/SourceEditorWidgetFixture.h"
#include "support/OwnedTestDirectory.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <imgui_internal.h>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    using WidgetFixture = Horo::Editor::Tests::SourceEditorWidgetFixture;

    /** @brief Counts actual rendered source-glyph color vertices rather than inspecting a detached lexical result. */
    std::size_t ColoredVertices(const SourceSyntaxRole role, const bool selected = false) {
        const auto theme = ResolveSourceEditorTheme(Theme::GetActiveTokens());
        const auto &style = theme.styles[static_cast<std::size_t>(role)];
        const auto &rgba = selected ? style.selectedColor : style.color;
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
        REQUIRE(ImGui::GetCurrentContext()->PlatformImeData.InputLineHeight == std::round(Theme::GetActiveTokens().typography.body));
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

TEST_CASE("Selected source glyphs retain semantic roles and readable contrast in the light theme", "[editor][source][widget][theme]") {
    WidgetFixture fixture;
    Theme::RefreshThemeList();
    Theme::SelectThemeByIndex(2);
    const auto source = fixture.Open("selected.cpp", "const char* text = \"value\"; // comment\n");
    fixture.first.Publish(source);
    REQUIRE(fixture.first.Highlight(source.Revision(), TokenizeSource(SourceLanguage::Cpp, source.Text())));
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(fixture.first.Select({0, source.Text().size()}));
    static_cast<void>(fixture.Frame(fixture.first, false));
    REQUIRE(ColoredVertices(SourceSyntaxRole::Keyword, true) > 0);
    REQUIRE(ColoredVertices(SourceSyntaxRole::String, true) > 0);
    REQUIRE(ColoredVertices(SourceSyntaxRole::Comment, true) > 0);
    Theme::SelectThemeByIndex(0);
}

TEST_CASE("Owner session reset replaces lexical projections even when document IDs and revisions repeat",
          "[editor][source][widget][syntax]") {
    WidgetFixture fixture;
    const auto first = fixture.Open("first.cpp", "const int value = 1;");
    static_cast<void>(fixture.Open("second.lua", "local value = true"));
    SourceFileOpenService nextOwner{fixture.directory.Path()};
    const auto opened = nextOwner.Open({.path = "second.lua", .mode = SourceOpenMode::EmbeddedOnly});
    REQUIRE(opened.HasValue());
    REQUIRE(opened.Value().sourceSnapshot);
    const auto second = *opened.Value().sourceSnapshot;
    REQUIRE(first.Identity().instance == second.Identity().instance);
    REQUIRE(first.Revision() == second.Revision());
    SourceEditorSession session;
    auto *view = session.Acquire(first.Identity().instance);
    REQUIRE(view);
    view->Publish(first);
    const auto oldIdentity = view->widget.Identity();
    REQUIRE(view->syntax.Syntax().language == SourceLanguage::Cpp);
    REQUIRE(view->widget.HasHighlight(first.Revision()));
    session.Reset();
    view = session.Acquire(second.Identity().instance);
    REQUIRE(view);
    view->Publish(second);
    REQUIRE(view->widget.Identity() != oldIdentity);
    REQUIRE(view->syntax.Syntax().language == SourceLanguage::HoroScript);
    REQUIRE(view->widget.HasHighlight(second.Revision()));
    REQUIRE_FALSE(fixture.Frame(view->widget));
}

TEST_CASE("Pointer clicks reveal the insertion caret immediately during its hidden blink phase", "[editor][source][widget][caret]") {
    WidgetFixture fixture;
    fixture.first.Publish(fixture.Open("caret.lua", "local value = 1\nreturn value\n"));
    const auto visibleCaret = [&] {
        const auto rgba = ResolveSourceEditorTheme(Theme::GetActiveTokens()).cursor;
        const auto *draw = ImGui::GetDrawData();
        for (int list = 0; list < draw->CmdListsCount; ++list)
            for (int i = 0; i + 3 < draw->CmdLists[list]->VtxBuffer.Size; ++i) {
                const auto *vertices = &draw->CmdLists[list]->VtxBuffer[i];
                const auto rendered = ImGui::ColorConvertU32ToFloat4(vertices[0].col);
                constexpr float channelTolerance = 1.1F / 255.0F;
                if (std::abs(rendered.x - rgba[0]) <= channelTolerance && std::abs(rendered.y - rgba[1]) <= channelTolerance &&
                    std::abs(rendered.z - rgba[2]) <= channelTolerance && vertices[0].col == vertices[2].col &&
                    vertices[2].pos.x - vertices[0].pos.x > 0 && vertices[2].pos.x - vertices[0].pos.x <= 1.1F &&
                    vertices[2].pos.y - vertices[0].pos.y >= 14.0F)
                    return true;
            }
        return false;
    };
    static_cast<void>(fixture.Frame(fixture.first));
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(visibleCaret());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (visibleCaret() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        static_cast<void>(fixture.Frame(fixture.first));
    }
    REQUIRE_FALSE(visibleCaret());
    ImGui::GetIO().AddMousePosEvent(140, 65);
    static_cast<void>(fixture.Frame(fixture.first));
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(visibleCaret());
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(fixture.Frame(fixture.first));
    REQUIRE(visibleCaret());
}
