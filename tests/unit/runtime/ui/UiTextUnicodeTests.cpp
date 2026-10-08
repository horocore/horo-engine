#include "Horo/Runtime/Ui/UiTextLayout.h"
#include "Horo/Runtime/Ui/UiTextUnicode.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> T RequireValue(Result<T> result) {
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiTextUnicodeRuntime LoadUnicode() {
            std::ifstream input(HORO_UNICODE_DATA_FILE, std::ios::binary);
            REQUIRE(input.good());
            const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            return RequireValue(UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes))));
        }

        UiTextUnicodeRuntime &Runtime() {
            // Test host load composition, deliberately outside every algorithm call.
            static auto runtime = LoadUnicode();
            return runtime;
        }

        UiTextShaperLimits Limits() {
            return {512, 8, 256, 256, 32, 3};
        }

        UiTextUnicodeAnalyzer Analyzer() {
            return RequireValue(UiTextUnicodeAnalyzer::Create(Runtime(), Limits()));
        }

        UiTextContentRevision Content(const std::uint64_t value = 1) {
            return RequireValue(UiTextContentRevision::Create(value));
        }

        UiTextLanguage Locale(const std::string_view value = "en-US") {
            return RequireValue(UiTextLanguage::Create(value));
        }

        UiTextUnicodeAnalysis Analyze(UiTextUnicodeAnalyzer &owner, const std::string_view text, const std::string_view locale = "en-US",
                                      const std::uint64_t revision = 1) {
            return RequireValue(owner.Analyze(text, Content(revision), Locale(locale), UiTextParagraphDirection::Auto));
        }

        template <typename T> T Revision(const std::uint64_t value) {
            return RequireValue(T::Create(value));
        }

        template <typename T> T Identity() {
            SerializedUiId bytes{};
            bytes.back() = 1;
            return RequireValue(T::Create(bytes));
        }

        UiTextShaper Shaper() {
            std::ifstream input(std::filesystem::path(HORO_PROJECT_SOURCE_DIR) / "assets/fonts/inter/InterVariable.ttf", std::ios::binary);
            REQUIRE(input.good());
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            const auto face = RequireValue(UiFontFace::Create({Identity<UiFontFaceId>(), 0}, bytes));
            const auto chain = RequireValue(UiFontFallbackChain::Create(std::array{face}, UiMissingGlyphPolicy::Replacement));
            return RequireValue(
                UiTextShaper::Create({Revision<UiOwnershipGeneration>(1), Revision<UiTextFontRevision>(1), chain, Limits()}));
        }

        UiTextShapingRequest ShapeRequest(const UiTextUnicodeAnalysis &analysis) {
            return {analysis.Text(), analysis.Content(), UiTextScript::Auto(), analysis.Locale(), UiTextDirection::Auto, {}, {}, &analysis};
        }

        UiTextLayoutRequest LayoutRequest() {
            const auto owner = Revision<UiOwnershipGeneration>(1);
            const UiLayoutSourceRevisions revisions{Revision<UiDocumentRevision>(1),        Revision<UiRuntimeTreeRevision>(1),
                                                    Revision<UiLayoutContentRevision>(1),   Revision<UiLayoutStyleRevision>(1),
                                                    Revision<UiLayoutIntrinsicRevision>(1), Revision<UiLayoutCanvasRevision>(1),
                                                    Revision<UiLayoutPolicyRevision>(1)};
            return {{{owner, 1, 1}, {owner, 2, 1}, {owner, 3, 1}, Identity<UiDocumentId>(), revisions},
                    {{0, 0}, {8192, 8192}},
                    {{0, 0}, {8192, 8192}},
                    {},
                    {},
                    std::nullopt};
        }

        UiTextLayoutEngine LayoutEngine(const UiTextLayoutRequest &request) {
            return RequireValue(UiTextLayoutEngine::Create({request.source.instance,
                                                            request.source.canvas,
                                                            request.source.document,
                                                            request.source.element,
                                                            {512, 256, 256, 32, 256},
                                                            3,
                                                            Revision<UiTextLayoutRevision>(1)}));
        }
    }  // namespace

    TEST_CASE("Unicode mixed paragraph ordering preserves exact source bytes", "[runtime_ui][unicode][bidi]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "A אב B");
        REQUIRE(result.Scalars().size() == 6);
        REQUIRE(result.Scalars()[2].level == 1);
        REQUIRE(result.Scalars()[3].level == 1);
        std::array<std::uint32_t, 16> visual{};
        const auto count = RequireValue(owner.OrderLine(result, 0, static_cast<std::uint32_t>(result.Text().size()), visual));
        REQUIRE(count == 6);
        const std::array<std::uint32_t, 6> expected{0, 1, 3, 2, 4, 5};
        REQUIRE(std::ranges::equal(std::span(visual).first(count), expected));
        for (const auto index : expected) {
            REQUIRE(result.Scalars()[index].byteStart < result.Scalars()[index].byteEnd);
            REQUIRE(result.Scalars()[index].byteEnd <= result.Text().size());
        }
    }

    TEST_CASE("Unicode paired brackets preserve mixed paragraph visual mapping", "[runtime_ui][unicode][bidi]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "A (אב) B");
        std::array<std::uint32_t, 16> visual{};
        const auto count = RequireValue(owner.OrderLine(result, 0, static_cast<std::uint32_t>(result.Text().size()), visual));
        const std::array<std::uint32_t, 8> expected{0, 1, 2, 4, 3, 5, 6, 7};
        REQUIRE(count == expected.size());
        REQUIRE(std::ranges::equal(std::span(visual).first(count), expected));
        REQUIRE(result.Text() == "A (אב) B");
    }

    TEST_CASE("Unicode visual mapping collapses surrogate units into complete source scalars", "[runtime_ui][unicode][bidi]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "אב😀");
        REQUIRE(result.Scalars().size() == 3);
        REQUIRE(result.Scalars()[2].utf16End - result.Scalars()[2].utf16Start == 2);
        std::array<std::uint32_t, 8> visual{};
        const auto count = RequireValue(owner.OrderLine(result, 0, static_cast<std::uint32_t>(result.Text().size()), visual));
        const std::array<std::uint32_t, 3> expected{2, 1, 0};
        REQUIRE(count == expected.size());
        REQUIRE(std::ranges::equal(std::span(visual).first(count), expected));
        REQUIRE(result.Scalars()[visual[0]].byteEnd - result.Scalars()[visual[0]].byteStart == 4);
    }

    TEST_CASE("Unicode line reset does not reuse paragraph trailing whitespace levels", "[runtime_ui][unicode][bidi]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "אב   A");
        std::array<std::uint32_t, 16> visual{};
        const auto end = result.Scalars()[4].byteEnd;
        const auto count = RequireValue(owner.OrderLine(result, 0, end, visual));
        REQUIRE(count == 5);
        REQUIRE(visual[0] == 4);
        REQUIRE(visual[1] == 3);
        REQUIRE(visual[2] == 2);
        REQUIRE(visual[3] == 1);
        REQUIRE(visual[4] == 0);
    }

    TEST_CASE("Unicode Thai line breaks include real dictionary word boundaries", "[runtime_ui][unicode][locale]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "ภาษาไทย", "th-TH");
        REQUIRE(result.Scalars().size() == 7);
        REQUIRE(result.Scalars()[3].breakAfter == UiTextUnicodeBreak::Optional);
        REQUIRE(result.Scalars()[0].breakAfter == UiTextUnicodeBreak::None);
    }

    TEST_CASE("Unicode keeps CRLF and extended graphemes indivisible", "[runtime_ui][unicode][clusters]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "e\xCC\x81\r\n👩‍💻");
        REQUIRE_FALSE(result.Scalars()[0].graphemeEnd);
        REQUIRE(result.Scalars()[1].graphemeEnd);
        REQUIRE_FALSE(result.Scalars()[2].graphemeEnd);
        REQUIRE(result.Scalars()[3].graphemeEnd);
        REQUIRE(result.Scalars()[3].breakAfter == UiTextUnicodeBreak::Mandatory);
        REQUIRE_FALSE(result.Scalars()[4].graphemeEnd);
        REQUIRE_FALSE(result.Scalars()[5].graphemeEnd);
        REQUIRE(result.Scalars()[6].graphemeEnd);
    }

    TEST_CASE("Unicode rejects malformed input and retains last good cache evidence", "[runtime_ui][unicode][validation]") {
        auto owner = Analyzer();
        const auto old = Analyze(owner, "old");
        REQUIRE(owner.Analyze("\xC0\xAF", Content(2), Locale(), UiTextParagraphDirection::Auto).HasError());
        REQUIRE(owner.Analyze("ok", Content(), UiTextLanguage::Auto(), UiTextParagraphDirection::Auto).HasError());
        REQUIRE(owner.Analyze("", Content(2), UiTextLanguage::Auto(), UiTextParagraphDirection::Auto).HasError());
        const auto cached = Analyze(owner, "old");
        REQUIRE(cached.Scalars().data() == old.Scalars().data());
        std::array<std::uint32_t, 1> tooSmall{};
        REQUIRE(owner.OrderLine(old, 0, 3, tooSmall).HasError());
    }

    TEST_CASE("Prepared Unicode cache hits perform no C++ allocation", "[runtime_ui][unicode][cache]") {
        auto owner = Analyzer();
        const auto old = Analyze(owner, "cached אב");
        const auto locale = Locale();
        Horo::Tests::AllocationProbe::Measurement observed;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement window;
            const auto result = owner.Analyze(old.Text(), old.Content(), locale, UiTextParagraphDirection::Auto);
            observed = window.Snapshot();
            REQUIRE(result.HasValue());
        }
        REQUIRE(observed.requests == 0);
    }

    TEST_CASE("Unicode isolates and numbers retain complete scalar mapping", "[runtime_ui][unicode][bidi]") {
        auto owner = Analyzer();
        const auto result = Analyze(owner, "X \xE2\x81\xA7"
                                           "אב 12"
                                           "\xE2\x81\xA9 Y");
        REQUIRE((result.Scalars()[3].level & 1U) == 1);
        REQUIRE((result.Scalars()[6].level & 1U) == 0);
        REQUIRE(result.Scalars()[6].level > 0);
        std::array<std::uint32_t, 32> visual{};
        const auto count = RequireValue(owner.OrderLine(result, 0, static_cast<std::uint32_t>(result.Text().size()), visual));
        REQUIRE(count == result.Scalars().size());
        const auto digits = std::ranges::find(std::span(visual).first(count), std::uint32_t{6});
        REQUIRE(digits != std::span(visual).first(count).end());
        REQUIRE(std::next(digits) != std::span(visual).first(count).end());
        REQUIRE(*std::next(digits) == 7);
        std::ranges::sort(std::span(visual).first(count));
        for (std::uint32_t index = 0; index < count; ++index)
            REQUIRE(visual[index] == index);
    }

    TEST_CASE("Unicode no-break and explicit-break characters retain locale semantics", "[runtime_ui][unicode][locale]") {
        auto owner = Analyzer();
        const auto nonbreaking = Analyze(owner, "A\xC2\xA0"
                                                "B");
        REQUIRE(nonbreaking.Scalars()[0].breakAfter == UiTextUnicodeBreak::None);
        REQUIRE(nonbreaking.Scalars()[1].breakAfter == UiTextUnicodeBreak::None);
        const auto explicitBreak = Analyze(owner,
                                           "A\xE2\x80\x8B"
                                           "B",
                                           "en-US", 2);
        REQUIRE(explicitBreak.Scalars()[1].breakAfter == UiTextUnicodeBreak::Optional);
    }

    TEST_CASE("Unicode result slots stay bounded across replacement and retirement", "[runtime_ui][unicode][lifecycle]") {
        auto owner = Analyzer();
        const auto first = Analyze(owner, "first");
        const auto second = Analyze(owner, "second", "en-US", 2);
        const auto third = Analyze(owner, "third", "ar", 3);
        REQUIRE(owner.Analyze("fourth", Content(4), Locale(), UiTextParagraphDirection::Auto).HasError());
        owner.Close();
        REQUIRE(owner.Analyze("new", Content(5), Locale(), UiTextParagraphDirection::Auto).HasError());
        REQUIRE(first.Text() == "first");
        REQUIRE(second.Text() == "second");
        REQUIRE(third.Text() == "third");
    }

    TEST_CASE("Unicode paragraph evidence drives the real HarfBuzz directional runs", "[runtime_ui][unicode][shaping]") {
        auto owner = Analyzer();
        const auto analysis = Analyze(owner, "A אב B");
        auto shaper = Shaper();
        const auto request = ShapeRequest(analysis);
        const auto shape = RequireValue(shaper.Shape(request));
        REQUIRE(std::ranges::any_of(shape.Runs(), [](const auto &run) {
            return run.direction == UiTextDirection::RightToLeft;
        }));
        REQUIRE(std::ranges::any_of(shape.Clusters(), [](const auto &cluster) {
            return cluster.bidiLevel == 1;
        }));
        REQUIRE(shape.Text() == analysis.Text());
        auto mismatch = request;
        mismatch.text = "different";
        REQUIRE(shaper.Shape(mismatch).HasError());
        shaper.Shutdown();
        REQUIRE(shape.IsValid());
    }

    TEST_CASE("Real Unicode shape commits wrapped visual layout with retained logical mappings", "[runtime_ui][unicode][layout]") {
        auto analyzer = Analyzer();
        const auto analysis = Analyze(analyzer, "A אב B");
        auto shaper = Shaper();
        const auto shape = RequireValue(shaper.Shape(ShapeRequest(analysis)));
        const auto request = LayoutRequest();
        auto engine = LayoutEngine(request);
        const auto result = RequireValue(engine.LayoutShaped(request, shape, analysis, analyzer));
        // The direct view path must reject a different content generation too,
        // even when byte length and every non-Unicode request field match.
        auto direct = request;
        direct.shaped = {Revision<UiLayoutContentRevision>(1), static_cast<std::uint32_t>(analysis.Text().size()), {}, {}, {}, {}};
        direct.unicode = &analysis;
        direct.unicodeAnalyzer = &analyzer;
        const UiTextLayoutLimits limits{512, 256, 256, 32, 256};
        REQUIRE(direct.IsValid(limits));
        const auto changed = Analyze(analyzer, analysis.Text(), "en-US", 2);
        direct.unicode = &changed;
        REQUIRE_FALSE(direct.IsValid(limits));
        REQUIRE(engine.Layout(direct).HasError());
        REQUIRE(result.Lines().size() == 1);
        REQUIRE(result.Clusters().size() == shape.Clusters().size());
        const auto logical = result.Clusters();
        const auto aleph = std::ranges::find(logical, std::uint32_t{2}, &UiTextLayoutCluster::byteStart);
        const auto bet = std::ranges::find(logical, std::uint32_t{4}, &UiTextLayoutCluster::byteStart);
        REQUIRE(aleph != logical.end());
        REQUIRE(bet != logical.end());
        REQUIRE(bet < aleph);
        REQUIRE(aleph->rightToLeft);
        REQUIRE(bet->rightToLeft);
        REQUIRE(aleph->origin.x >= bet->origin.x);
        engine.Shutdown();
        analyzer.Close();
        shaper.Shutdown();
        REQUIRE(result.Clusters()[aleph - logical.begin()].byteStart == 2);
    }

    TEST_CASE("Real text layout preserves mandatory newline mapping without replacement ink", "[runtime_ui][unicode][layout]") {
        auto analyzer = Analyzer();
        const auto analysis = Analyze(analyzer, "A\r\nB");
        auto shaper = Shaper();
        const auto shape = RequireValue(shaper.Shape(ShapeRequest(analysis)));
        REQUIRE(shape.Clusters().size() == 3);
        REQUIRE(shape.Clusters()[1].glyphCount == 0);
        REQUIRE(shape.Clusters()[1].advance.x == 0);
        const auto request = LayoutRequest();
        auto engine = LayoutEngine(request);
        const auto result = RequireValue(engine.LayoutShaped(request, shape, analysis, analyzer));
        REQUIRE(result.Lines().size() == 2);
        REQUIRE(result.Clusters().size() == 3);
        REQUIRE(result.Clusters()[1].byteStart == 1);
        REQUIRE(result.Clusters()[1].byteEnd == 3);
        REQUIRE(result.Clusters()[1].advance == 0);
    }
}  // namespace Horo::Runtime::Ui
