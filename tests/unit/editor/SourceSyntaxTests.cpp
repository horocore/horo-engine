#include "../../support/OwnedTestDirectory.h"
#include "Horo/Editor/DesignSystem/DesignTokens.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "editor/source_editor/SourceEditorTheme.h"
#include "editor/source_editor/SourceSyntax.h"
#include "editor/source_editor/SourceSyntaxCache.h"
#include "editor/source_editor/SourceSyntaxProjection.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>

using namespace Horo::Editor;

namespace {
    void RequireRole(const std::string_view path, const std::string_view text, const std::string_view token,
                     const SourceSyntaxRole expected) {
        const auto syntax = TokenizeSource(ClassifySourceLanguage(path), text);
        REQUIRE(syntax.fallback == SourceSyntaxFallback::None);
        const auto begin = text.find(token);
        REQUIRE(begin != text.npos);
        REQUIRE(syntax.RoleAt(begin) == expected);
        REQUIRE(syntax.RoleAt(begin + token.size() - 1) == expected);
        std::size_t previous = 0;
        for (const auto &span : syntax.spans) {
            REQUIRE(span.begin >= previous);
            REQUIRE(span.end > span.begin);
            REQUIRE(span.end <= text.size());
            previous = span.end;
        }
    }
}  // namespace

TEST_CASE("Source syntax classifies project language families without I/O", "[editor][source][syntax]") {
    REQUIRE(ClassifySourceLanguage("src/player.CPP") == SourceLanguage::Cpp);
    REQUIRE(ClassifySourceLanguage("a\\b\\player.horo_script") == SourceLanguage::HoroScript);
    REQUIRE(ClassifySourceLanguage("shader/file.hlsl") == SourceLanguage::Shader);
    REQUIRE(ClassifySourceLanguage("shader/file.metal") == SourceLanguage::Shader);
    REQUIRE(ClassifySourceLanguage("CMakeLists.txt") == SourceLanguage::CMake);
    REQUIRE(ClassifySourceLanguage("directory.cpp/readme") == SourceLanguage::PlainText);
    REQUIRE(ClassifySourceLanguage("readme.unknown") == SourceLanguage::PlainText);
    REQUIRE(ClassifySourceLanguage("readme.extension_longer_than_the_language_admission") == SourceLanguage::PlainText);
}

TEST_CASE("Native and shader syntax uses typed semantic roles", "[editor][source][syntax]") {
    RequireRole("a.cpp", "const float speed = 42; // return", "const", SourceSyntaxRole::Keyword);
    RequireRole("a.cpp", "const float speed = 42; // return", "float", SourceSyntaxRole::Type);
    RequireRole("a.cpp", "const float speed = 42; // return", "42", SourceSyntaxRole::Number);
    RequireRole("a.cpp", "const float speed = 42; // return", "return", SourceSyntaxRole::Comment);
    RequireRole("a.cpp", "auto s = R\"tag(/* return */)tag\";", "return", SourceSyntaxRole::String);
    RequireRole("a.cpp", "/* first\r\nsecond */ return;", "second", SourceSyntaxRole::Comment);
    RequireRole("a.cpp", "int count = 1'000;", "1'000", SourceSyntaxRole::Number);
    RequireRole("a.cpp", "float count = 1.0e-3;", "1.0e-3", SourceSyntaxRole::Number);
    RequireRole("a.cpp", "int count = 1+2;", "2", SourceSyntaxRole::Number);
    RequireRole("a.hlsl", "cbuffer Scene { float4 color; };", "cbuffer", SourceSyntaxRole::Keyword);
    RequireRole("a.hlsl", "cbuffer Scene { float4 color; };", "float4", SourceSyntaxRole::Type);
    RequireRole("a.glsl", "uniform vec3 color;", "vec3", SourceSyntaxRole::Type);
    RequireRole("a.cpp", "#include <vector>\r\nint x;", "include", SourceSyntaxRole::Directive);
}

TEST_CASE("Horo Script syntax supports Lua comments strings and keywords", "[editor][source][syntax]") {
    RequireRole("a.horo_script", "local value = true -- return", "local", SourceSyntaxRole::Keyword);
    RequireRole("a.lua", "local value = true -- return", "return", SourceSyntaxRole::Comment);
    RequireRole("a.lua", "--[=[ first\r\nreturn ]=]\nlocal x", "return", SourceSyntaxRole::Comment);
    RequireRole("a.lua", "local s = [==[ -- text\nreturn ]==]", "return", SourceSyntaxRole::String);
    RequireRole("a.lua", "local s = \"return \\\"text\\\"\"", "return", SourceSyntaxRole::String);
}

TEST_CASE("Configuration syntax separates properties values and comments", "[editor][source][syntax]") {
    RequireRole("a.json", R"({"name": "text", "enabled": true})", "name", SourceSyntaxRole::Property);
    RequireRole("a.json", R"({"name": "text", "enabled": true})", "text", SourceSyntaxRole::String);
    RequireRole("a.json", R"({"name": "text", "enabled": true})", "true", SourceSyntaxRole::Keyword);
    RequireRole("a.toml", "name = 'player' # comment", "name", SourceSyntaxRole::Property);
    RequireRole("a.toml", "name = 'player' # comment", "comment", SourceSyntaxRole::Comment);
    RequireRole("a.toml", "name = '''first\nsecond'''", "second", SourceSyntaxRole::String);
    RequireRole("a.yaml", "enabled: true\n# note", "enabled", SourceSyntaxRole::Property);
    RequireRole("a.ini", "port = 80; comment", "port", SourceSyntaxRole::Property);
    RequireRole("a.cfg", "port = 80; comment", "80", SourceSyntaxRole::Number);
    RequireRole("a.xml", "<project name=\"demo\"><!-- note --></project>", "project", SourceSyntaxRole::Keyword);
    RequireRole("CMakeLists.txt", "set(NAME \"demo\") # note", "set", SourceSyntaxRole::Keyword);
}

TEST_CASE("Lexical offsets preserve UTF8 bytes CRLF and half-open boundaries", "[editor][source][syntax]") {
    const std::string text = "// Türkçe 日本語\r\nconst auto label = \"ışık\";\r\n";
    const auto syntax = TokenizeSource(SourceLanguage::Cpp, text);
    REQUIRE(syntax.fallback == SourceSyntaxFallback::None);
    const auto offset = text.find("const");
    REQUIRE(syntax.RoleAt(offset - 1) == SourceSyntaxRole::Text);
    REQUIRE(syntax.RoleAt(offset) == SourceSyntaxRole::Keyword);
    REQUIRE(syntax.RoleAt(offset + 4) == SourceSyntaxRole::Keyword);
    REQUIRE(syntax.RoleAt(offset + 5) == SourceSyntaxRole::Text);
    for (const auto &span : syntax.spans) {
        REQUIRE_FALSE(span.end > 0 && span.end < text.size() && text[span.end - 1] == '\r' && text[span.end] == '\n');
    }
    REQUIRE(syntax.RoleAt(text.size()) == SourceSyntaxRole::Text);
    REQUIRE(syntax.RoleAt(static_cast<std::size_t>(-1)) == SourceSyntaxRole::Text);
    REQUIRE(text == "// Türkçe 日本語\r\nconst auto label = \"ışık\";\r\n");
}

TEST_CASE("Unknown and malformed lexical input retains a whole-view plain text fallback", "[editor][source][syntax]") {
    const auto unknown = TokenizeSource(SourceLanguage::PlainText, "const auto x = 1;");
    REQUIRE(unknown.fallback == SourceSyntaxFallback::UnknownLanguage);
    REQUIRE(unknown.spans.empty());
    for (const auto text : {"int x; /* unclosed", "const char* s = \"unclosed", "auto s = R\"x(unclosed"}) {
        const auto syntax = TokenizeSource(SourceLanguage::Cpp, text);
        REQUIRE(syntax.fallback == SourceSyntaxFallback::Malformed);
        REQUIRE(syntax.spans.empty());
    }
    REQUIRE(TokenizeSource(SourceLanguage::HoroScript, "local s = [=[unclosed").fallback == SourceSyntaxFallback::Malformed);
    REQUIRE(TokenizeSource(SourceLanguage::Json, "{\"name\": \"unclosed}").spans.empty());
    REQUIRE(TokenizeSource(SourceLanguage::Json, "{'name':'invalid'}").fallback == SourceSyntaxFallback::Malformed);
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, "const char* s = \"line\nnext\";").fallback == SourceSyntaxFallback::Malformed);
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, "auto s = R\"abcdefghijklmnopq(text)abcdefghijklmnopq\";").fallback ==
            SourceSyntaxFallback::Malformed);
    REQUIRE(TokenizeSource(SourceLanguage::HoroScript, "local s = [=================[text]=================]").fallback ==
            SourceSyntaxFallback::Malformed);
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, "").fallback == SourceSyntaxFallback::None);
}

TEST_CASE("Source syntax memory and input work admission have explicit bounds", "[editor][source][syntax]") {
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, std::string(65'537, 'x')).fallback == SourceSyntaxFallback::BudgetExceeded);
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, std::string(2 * 1024 * 1024 + 1, '\n')).fallback == SourceSyntaxFallback::BudgetExceeded);
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, "int x = 1;", {.maximumSpans = 1}).fallback == SourceSyntaxFallback::BudgetExceeded);
    REQUIRE(TokenizeSource(SourceLanguage::Cpp, "int", {.maximumBytes = 3, .maximumLineBytes = 3, .maximumSpans = 1}).spans.size() == 1);
    const auto syntax = TokenizeSource(SourceLanguage::Cpp, std::string(65'536, 'x'));
    REQUIRE(syntax.fallback == SourceSyntaxFallback::None);
    REQUIRE(syntax.spans.empty());
    std::string dense;
    for (int i = 0; i < 65'537; ++i)
        dense += "int\n";
    const auto limited = TokenizeSource(SourceLanguage::Cpp, dense);
    REQUIRE(limited.fallback == SourceSyntaxFallback::BudgetExceeded);
    REQUIRE(limited.spans.empty());
}

TEST_CASE("Source styles follow each frame theme and preserve readable contrast at every DPI", "[editor][source][theme]") {
    auto tokens = DesignSystem::DefaultDesignTokens();
    const auto dark = ResolveSourceEditorTheme(tokens);
    for (const auto &style : dark.styles)
        REQUIRE(SourceContrast(style.color, dark.background) >= 4.5F);
    tokens.colors.surfaceWindow = ImVec4{1, 1, 1, 1};
    tokens.sourceSyntax.keyword = ImVec4{0.98F, 0.98F, 0.98F, 1};
    const auto light = ResolveSourceEditorTheme(tokens);
    REQUIRE(light.background != dark.background);
    REQUIRE(light.styles[static_cast<std::size_t>(SourceSyntaxRole::Keyword)].color !=
            dark.styles[static_cast<std::size_t>(SourceSyntaxRole::Keyword)].color);
    for (const auto &style : light.styles)
        REQUIRE(SourceContrast(style.color, light.background) >= 4.5F);
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        auto scaled = tokens;
        DesignSystem::ApplyGlobalUiScale(scaled, scale);
        const auto theme = ResolveSourceEditorTheme(scaled);
        REQUIRE(theme.fontPixels == tokens.typography.body * scale);
        REQUIRE(theme.styles[static_cast<std::size_t>(SourceSyntaxRole::Keyword)].font == SourceFontRole::MonospaceEmphasis);
        REQUIRE(theme.styles[static_cast<std::size_t>(SourceSyntaxRole::Comment)].font == SourceFontRole::Monospace);
    }
}

TEST_CASE("Source lexical cache keys exact publications rather than revision numbers or frame themes", "[editor][source][syntax]") {
    Horo::Tests::OwnedTestDirectory directory{"source-syntax-cache"};
    {
        std::ofstream file{directory.Path() / "a.cpp"};
        file << "const float x = 1;";
    }
    {
        std::ofstream file{directory.Path() / "b.lua"};
        file << "local x = true";
    }
    SourceFileOpenService documents{directory.Path()};
    const auto a = documents.Open({.path = "a.cpp", .mode = SourceOpenMode::EmbeddedOnly});
    const auto b = documents.Open({.path = "b.lua", .mode = SourceOpenMode::EmbeddedOnly});
    REQUIRE(a.HasValue());
    REQUIRE(b.HasValue());
    REQUIRE(a.Value().sourceSnapshot);
    REQUIRE(b.Value().sourceSnapshot);
    const auto first = *a.Value().sourceSnapshot;
    const auto second = *b.Value().sourceSnapshot;
    REQUIRE(first.Revision() == second.Revision());
    SourceSyntaxCache cache;
    REQUIRE(cache.Publish(first));
    const auto *spans = cache.Syntax().spans.data();
    REQUIRE_FALSE(cache.Publish(first));
    REQUIRE(cache.Syntax().spans.data() == spans);
    REQUIRE(cache.Syntax().language == SourceLanguage::Cpp);
    REQUIRE(cache.Publish(second));
    REQUIRE(cache.Syntax().language == SourceLanguage::HoroScript);
    const auto changed = documents.Documents().Edit(second.Identity().instance, {second.Revision(), 0, 0, "--comment\n"});
    REQUIRE(changed.HasValue());
    REQUIRE(cache.Publish(changed.Value()));
    REQUIRE(cache.Syntax().RoleAt(0) == SourceSyntaxRole::Comment);
    REQUIRE(cache.Publish({}));
    REQUIRE(cache.Syntax().spans.empty());
}

TEST_CASE("Widget syntax projection normalizes original BOM UTF8 and mixed endings exactly once", "[editor][source][syntax]") {
    Horo::Tests::OwnedTestDirectory directory{"source-syntax-projection"};
    const std::string original = "\xEF\xBB\xBF"
                                 "// Türkçe\r\nconst float value = 1;\r\n";
    {
        std::ofstream file{directory.Path() / "a.cpp", std::ios::binary};
        file << original;
    }
    SourceFileOpenService documents{directory.Path()};
    const auto opened = documents.Open({.path = "a.cpp", .mode = SourceOpenMode::EmbeddedOnly});
    REQUIRE(opened.HasValue());
    REQUIRE(opened.Value().sourceSnapshot);
    const auto source = *opened.Value().sourceSnapshot;
    const auto syntax = TokenizeSource(SourceLanguage::Cpp, source.Text());
    const auto display = ProjectSourceSyntaxForDisplay(source, syntax);
    REQUIRE(display);
    const std::string normalized = "// Türkçe\nconst float value = 1;\n";
    REQUIRE(display->RoleAt(normalized.find("const")) == SourceSyntaxRole::Keyword);
    REQUIRE(display->RoleAt(normalized.find("float")) == SourceSyntaxRole::Type);
    REQUIRE(source.Text() == original);
    REQUIRE(syntax.RoleAt(original.find("const")) == SourceSyntaxRole::Keyword);
    for (const auto invalid : {SourceSyntaxSpan{0, 2, SourceSyntaxRole::Comment},
                               SourceSyntaxSpan{original.find('\n'), original.find('\n') + 1, SourceSyntaxRole::Comment},
                               SourceSyntaxSpan{3, original.size() + 1, SourceSyntaxRole::Text}}) {
        SourceSyntax malformed{.spans = {invalid}};
        REQUIRE_FALSE(ProjectSourceSyntaxForDisplay(source, malformed));
    }
    REQUIRE(ProjectSourceSyntaxForDisplay({}, SourceSyntax{}));
}

TEST_CASE("Source foreground normalization keeps malformed extension channels readable", "[editor][source][theme]") {
    auto tokens = DesignSystem::DefaultDesignTokens();
    tokens.sourceSyntax.text = ImVec4{-1.0F, 2.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    const auto theme = ResolveSourceEditorTheme(tokens);
    const auto &color = theme.styles[static_cast<std::size_t>(SourceSyntaxRole::Text)].color;
    for (const auto channel : color) {
        REQUIRE(std::isfinite(channel));
        REQUIRE(channel >= 0.0F);
        REQUIRE(channel <= 1.0F);
    }
    REQUIRE(SourceContrast(color, theme.background) >= 4.5F);
}
