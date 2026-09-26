#include "Horo/Application/CompilerDiagnosticParser.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

using namespace Horo;
using namespace Horo::Application;

TEST_CASE("Compiler diagnostic parser supports GCC Clang and MSVC output", "[unit][gameplay][build][diagnostics]") {
    const std::filesystem::path projectRoot = std::filesystem::temp_directory_path() / "horo parser project";

    SECTION("relative GCC warning preserves source and warning code") {
        const auto diagnostic =
            ParseCompilerDiagnostic("source/gameplay/Player Move.cpp:12:7: warning: deprecated declaration [-Wdeprecated-declarations]",
                                    projectRoot);
        REQUIRE(diagnostic.has_value());
        REQUIRE((diagnostic->format == CompilerDiagnosticFormat::ClangOrGcc));
        REQUIRE((diagnostic->severity == DiagnosticSeverity::Warning));
        REQUIRE((diagnostic->source.absolutePath == (projectRoot / "source/gameplay/Player Move.cpp").lexically_normal().string()));
        REQUIRE((diagnostic->source.line == 12U));
        REQUIRE((diagnostic->source.column == 7U));
        REQUIRE((diagnostic->message == "deprecated declaration [-Wdeprecated-declarations]"));
        REQUIRE((diagnostic->compilerCode == "-Wdeprecated-declarations"));
    }

    SECTION("absolute Clang error preserves non-ASCII source") {
        const std::filesystem::path source = projectRoot / "kaynak/oyuncu_hareketi_ş.cpp";
        const auto diagnostic = ParseCompilerDiagnostic(source.string() + ":42:3: error: expected expression", projectRoot);
        REQUIRE(diagnostic.has_value());
        REQUIRE((diagnostic->severity == DiagnosticSeverity::Error));
        REQUIRE((diagnostic->source.absolutePath == source.lexically_normal().string()));
        REQUIRE((diagnostic->source.line == 42U));
        REQUIRE((diagnostic->source.column == 3U));
        REQUIRE_FALSE(diagnostic->compilerCode.has_value());
    }

    SECTION("MSVC error supports drive letters spaces and compiler codes") {
        const auto diagnostic =
            ParseCompilerDiagnostic("C:\\Horo Project\\source\\Player.cpp(27,11): error C2143: syntax error: missing ';' before '}'",
                                    projectRoot);
        REQUIRE(diagnostic.has_value());
        REQUIRE((diagnostic->format == CompilerDiagnosticFormat::Msvc));
        REQUIRE((diagnostic->severity == DiagnosticSeverity::Error));
        REQUIRE((diagnostic->source.line == 27U));
        REQUIRE((diagnostic->source.column == 11U));
        REQUIRE((diagnostic->compilerCode == "C2143"));
        REQUIRE((diagnostic->message == "syntax error: missing ';' before '}'"));
    }
}

TEST_CASE("Compiler diagnostic parser accepts omitted columns", "[unit][gameplay][build][diagnostics]") {
    const std::filesystem::path projectRoot = std::filesystem::temp_directory_path() / "horo parser project";

    const auto gcc = ParseCompilerDiagnostic("source/gameplay/Player.cpp:31: error: expected declaration", projectRoot);
    REQUIRE(gcc.has_value());
    REQUIRE((gcc->source.line == 31U));
    REQUIRE((gcc->source.column == 0U));

    const auto msvc = ParseCompilerDiagnostic("source\\Player.cpp(19): warning C4100: unreferenced parameter", projectRoot);
    REQUIRE(msvc.has_value());
    REQUIRE((msvc->source.line == 19U));
    REQUIRE((msvc->source.column == 0U));
    REQUIRE((msvc->compilerCode == "C4100"));
}

TEST_CASE("Compiler diagnostic parser rejects malformed and oversized input safely", "[unit][gameplay][build][diagnostics]") {
    const std::filesystem::path projectRoot = std::filesystem::temp_directory_path();

    REQUIRE_FALSE(ParseCompilerDiagnostic("not a diagnostic", projectRoot).has_value());
    REQUIRE_FALSE(ParseCompilerDiagnostic("file.cpp:0:2: error: invalid line", projectRoot).has_value());
    REQUIRE_FALSE(ParseCompilerDiagnostic("file.cpp:2:nope: warning: invalid column", projectRoot).has_value());
    REQUIRE_FALSE(ParseCompilerDiagnostic("file.cpp(nope): error C1000: invalid line", projectRoot).has_value());
    REQUIRE_FALSE(ParseCompilerDiagnostic(std::string(MaximumCompilerDiagnosticLineBytes + 1U, 'x'), projectRoot).has_value());

    const std::string prefix = "file.cpp:1:1: warning: ";
    const std::string longMessage(MaximumCompilerDiagnosticMessageBytes + 5U, 'm');
    const auto truncated = ParseCompilerDiagnostic(prefix + longMessage, projectRoot, true);
    REQUIRE(truncated.has_value());
    REQUIRE(truncated->inputTruncated);
    REQUIRE(truncated->messageTruncated);
    REQUIRE((truncated->message.size() == MaximumCompilerDiagnosticMessageBytes));
}
