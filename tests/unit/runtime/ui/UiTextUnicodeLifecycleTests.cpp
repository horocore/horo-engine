#include "Horo/Runtime/Ui/UiTextUnicode.h"
#include "support/FixtureFileRead.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

TEST_CASE("Host Unicode shutdown waits for native paragraph leases and is terminal", "[runtime_ui][unicode][shutdown]") {
    using namespace Horo::Runtime::Ui;
    auto bytes = Horo::Tests::ReadFixtureBytes(HORO_UNICODE_DATA_FILE);
    auto created = UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes)));
    REQUIRE(created.HasValue());
    auto runtime = std::move(created).Value();
    // Registration must retain the runtime's aligned copy, not the host input.
    std::string{}.swap(bytes);
    auto admitted = UiTextUnicodeAnalyzer::Create(runtime, {128, 8, 64, 64, 16, 2});
    REQUIRE(admitted.HasValue());
    std::optional<UiTextUnicodeAnalyzer> analyzer(std::move(admitted).Value());
    const auto content = UiTextContentRevision::Create(1).Value();
    const auto locale = UiTextLanguage::Create("en").Value();
    auto prepared = analyzer->Analyze("kept אב", content, locale, UiTextParagraphDirection::Auto);
    REQUIRE(prepared.HasValue());
    std::optional<UiTextUnicodeAnalysis> lease(std::move(prepared).Value());
    REQUIRE(runtime.Shutdown().HasError());
    REQUIRE_FALSE(runtime.IsActive());
    runtime.Close();
    runtime.Close();
    REQUIRE(UiTextUnicodeAnalyzer::Create(runtime, {128, 8, 64, 64, 16, 2}).HasError());
    REQUIRE(analyzer->Analyze("new", content, locale, UiTextParagraphDirection::Auto).HasError());
    analyzer.reset();
    REQUIRE(runtime.Shutdown().HasError());
    REQUIRE(lease->Text() == "kept אב");
    lease.reset();
    REQUIRE(runtime.Shutdown().HasValue());
    REQUIRE(runtime.Shutdown().HasValue());
    REQUIRE(UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes))).HasError());
}
