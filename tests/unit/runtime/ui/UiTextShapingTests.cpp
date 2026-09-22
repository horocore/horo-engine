#include "Horo/Runtime/Ui/UiTextShaping.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Value> Value RequireValue(Result<Value> result) {
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        template <typename Id> Id StableId(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return RequireValue(Id::Create(bytes));
        }

        UiOwnershipGeneration Ownership(const std::uint64_t value) {
            return RequireValue(UiOwnershipGeneration::Create(value));
        }

        template <typename Revision> Revision RevisionValue(const std::uint64_t value) {
            return RequireValue(Revision::Create(value));
        }

        std::vector<std::uint8_t> ReadFont(const std::string_view relativePath) {
            const auto path = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / relativePath;
            std::ifstream input(path, std::ios::binary);
            REQUIRE(input.good());
            return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        }

        UiFontFace LoadFont(const std::uint8_t marker, const std::string_view relativePath) {
            auto bytes = ReadFont(relativePath);
            const auto face = UiFontFace::Create(UiFontFaceDescriptor{StableId<UiFontFaceId>(marker), 0}, bytes);
            REQUIRE(face.HasValue());
            bytes.clear();
            return std::move(face).Value();
        }

        UiFontFallbackChain Chain(const std::span<const UiFontFace> faces, const UiMissingGlyphPolicy policy) {
            return RequireValue(UiFontFallbackChain::Create(faces, policy));
        }

        UiTextShaper MakeShaper(UiFontFallbackChain chain, const std::uint64_t ownerValue = 1, const std::uint64_t fontRevisionValue = 1,
                                const std::uint32_t concurrentShapes = 2) {
            UiTextShaperLimits limits;
            limits.maxInputBytes = 512;
            limits.maxFeatures = 8;
            limits.maxGlyphs = 512;
            limits.maxClusters = 512;
            limits.maxRuns = 32;
            limits.concurrentShapes = concurrentShapes;
            return RequireValue(
                UiTextShaper::Create(UiTextShaperDescriptor{Ownership(ownerValue), RevisionValue<UiTextFontRevision>(fontRevisionValue),
                                                            std::move(chain), limits}));
        }

        UiTextShapingRequest Request(const std::string_view text, const UiTextScript script = UiTextScript::Auto(),
                                     const UiTextLanguage language = UiTextLanguage::Auto(),
                                     const UiTextDirection direction = UiTextDirection::Auto,
                                     const std::span<const UiTextFeature> features = {}) {
            return UiTextShapingRequest{text,    RevisionValue<UiTextContentRevision>(1), script, language, direction, UiTextFontSize{},
                                        features};
        }

        void RequireCode(const Result<UiTextShape> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }
    }  // namespace

    TEST_CASE("Runtime UI text inputs and font fallback reject malformed evidence", "[runtime_ui][text][validation]") {
        const auto invalidBytes = std::vector<std::uint8_t>{0x00, 0x01, 0x02};
        REQUIRE(UiFontFace::Create(UiFontFaceDescriptor{}, invalidBytes).HasError());
        REQUIRE(UiTextScript::Create("Latin").HasError());
        REQUIRE(UiTextScript::Create("la1n").HasError());
        REQUIRE(UiTextLanguage::Create("en--US").HasError());
        REQUIRE(UiTextLanguage::Create("en_US").HasError());
        REQUIRE(UiTextFeature::Create("lig").HasError());
        REQUIRE(UiTextFeature::Create("liga", 1, 5, 4).HasError());
        REQUIRE(UiTextFontSize::Create(0).HasError());

        const auto face = LoadFont(1, "assets/fonts/inter/InterVariable.ttf");
        const std::vector<UiFontFace> empty;
        REQUIRE(UiFontFallbackChain::Create(empty, UiMissingGlyphPolicy::Replacement).HasError());
        const std::vector<UiFontFace> duplicate{face, face};
        REQUIRE(UiFontFallbackChain::Create(duplicate, UiMissingGlyphPolicy::Replacement).HasError());
        REQUIRE(UiFontFallbackChain::Create(std::array{face}, UiMissingGlyphPolicy::Count).HasError());

        auto shaper = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::Replacement));
        const std::string malformed{"\xC3\x28", 2};
        const auto malformedResult = shaper.Shape(Request(malformed));
        RequireCode(malformedResult, UiErrors::TextInputInvalid);
        const std::string overlong{"\xC0\xAF", 2};
        const auto overlongResult = shaper.Shape(Request(overlong));
        RequireCode(overlongResult, UiErrors::TextInputInvalid);
    }

    TEST_CASE("Runtime UI shaping returns stable logical metrics and features", "[runtime_ui][text][shaping]") {
        const auto autoFace = LoadFont(2, "assets/fonts/inter/InterVariable.ttf");
        const auto script = RequireValue(UiTextScript::Create("Latn"));
        const auto language = RequireValue(UiTextLanguage::Create("en-US"));
        const auto feature = RequireValue(UiTextFeature::Create("liga"));
        const std::array features{feature};
        auto shaper = MakeShaper(Chain(std::array{autoFace}, UiMissingGlyphPolicy::Replacement), 11, 7, 3);

        const auto result = shaper.Shape(Request("office", script, language, UiTextDirection::LeftToRight, features));
        REQUIRE(result.HasValue());
        const auto shape = std::move(result).Value();
        REQUIRE(shape.IsValid());
        REQUIRE(shape.Text() == "office");
        REQUIRE(shape.Features().size() == 1);
        REQUIRE(shape.Descriptor().ownership == Ownership(11));
        REQUIRE(shape.Descriptor().font == RevisionValue<UiTextFontRevision>(7));
        REQUIRE(shape.Descriptor().content == RevisionValue<UiTextContentRevision>(1));
        REQUIRE(shape.Metrics().IsValid());
        REQUIRE(shape.Metrics().advance.x > 0);
        REQUIRE_FALSE(shape.Runs().empty());
        REQUIRE_FALSE(shape.Glyphs().empty());
        REQUIRE_FALSE(shape.Clusters().empty());
        for (const auto &run : shape.Runs()) {
            REQUIRE(run.IsValid());
            REQUIRE(run.face == autoFace.Id());
            REQUIRE(run.byteEnd <= shape.Text().size());
        }
        for (const auto &glyph : shape.Glyphs())
            REQUIRE(glyph.IsValid());
        for (const auto &cluster : shape.Clusters()) {
            REQUIRE(cluster.IsValid());
            REQUIRE(cluster.byteEnd <= shape.Text().size());
        }
    }

    TEST_CASE("Runtime UI shaping preserves repeatable glyph and cluster evidence", "[runtime_ui][text][shaping]") {
        const auto face = LoadFont(6, "assets/fonts/inter/InterVariable.ttf");
        const auto script = RequireValue(UiTextScript::Create("Latn"));
        const auto language = RequireValue(UiTextLanguage::Create("en-US"));
        const auto feature = RequireValue(UiTextFeature::Create("liga"));
        const std::array features{feature};
        auto shaper = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::Replacement), 12, 8, 3);

        const auto firstResult = shaper.Shape(Request("office", script, language, UiTextDirection::LeftToRight, features));
        REQUIRE(firstResult.HasValue());
        auto first = std::move(firstResult).Value();
        const auto secondResult = shaper.Shape(Request("office", script, language, UiTextDirection::LeftToRight, features));
        REQUIRE(secondResult.HasValue());
        const auto second = std::move(secondResult).Value();
        REQUIRE(first.IsValid());
        REQUIRE(second.IsValid());
        REQUIRE(second.Descriptor().shape != first.Descriptor().shape);
        REQUIRE(second.Metrics().advance == first.Metrics().advance);
        REQUIRE(second.Glyphs().size() == first.Glyphs().size());
        REQUIRE(second.Clusters().size() == first.Clusters().size());
        for (std::size_t index = 0; index < first.Glyphs().size(); ++index) {
            REQUIRE(second.Glyphs()[index].face == first.Glyphs()[index].face);
            REQUIRE(second.Glyphs()[index].glyph == first.Glyphs()[index].glyph);
            REQUIRE(second.Glyphs()[index].cluster == first.Glyphs()[index].cluster);
            REQUIRE(second.Glyphs()[index].offset == first.Glyphs()[index].offset);
            REQUIRE(second.Glyphs()[index].advance == first.Glyphs()[index].advance);
        }

        const auto combining = shaper.Shape(Request("e\xCC\x81", script, language, UiTextDirection::LeftToRight, features));
        REQUIRE(combining.HasValue());
        REQUIRE(combining.Value().IsValid());
        REQUIRE(combining.Value().Clusters().size() == 1);
        REQUIRE(combining.Value().Clusters().front().byteStart == 0);
        REQUIRE(combining.Value().Clusters().front().byteEnd == 3);
    }

    TEST_CASE("Runtime UI shaping preserves ZWJ and variation-selector clusters", "[runtime_ui][text][clusters]") {
        const auto face = LoadFont(7, "assets/fonts/inter/InterVariable.ttf");
        auto shaper = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::Replacement), 13, 9, 2);

        const std::string zwjSequence{"\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB", 11};
        const auto zwjResult = shaper.Shape(Request(zwjSequence));
        REQUIRE(zwjResult.HasValue());
        const auto zwjShape = std::move(zwjResult).Value();
        REQUIRE(zwjShape.Clusters().size() == 1);
        REQUIRE(zwjShape.Clusters().front().byteStart == 0);
        REQUIRE(zwjShape.Clusters().front().byteEnd == zwjSequence.size());
        REQUIRE(zwjShape.Clusters().front().missing);

        const std::string variationSequence{"\xE2\x9D\xA4\xEF\xB8\x8F", 6};
        const auto variationResult = shaper.Shape(Request(variationSequence));
        REQUIRE(variationResult.HasValue());
        const auto variationShape = std::move(variationResult).Value();
        REQUIRE(variationShape.Clusters().size() == 1);
        REQUIRE(variationShape.Clusters().front().byteStart == 0);
        REQUIRE(variationShape.Clusters().front().byteEnd == variationSequence.size());
        REQUIRE(variationShape.Clusters().front().missing);
    }

    TEST_CASE("Runtime UI shaping resolves direction and complete-cluster fallback", "[runtime_ui][text][fallback]") {
        const auto primary = LoadFont(3, "assets/fonts/ibm-plex-mono/IBMPlexMono-Regular.ttf");
        const auto symbols = LoadFont(4, "assets/fonts/MaterialSymbolsOutlined.ttf");
        auto fallback = MakeShaper(Chain(std::array{primary, symbols}, UiMissingGlyphPolicy::FailStrict), 21, 9);

        const std::string materialHome{"\xEE\xA2\x8A", 3};
        const auto fallbackResult = fallback.Shape(Request(materialHome));
        REQUIRE(fallbackResult.HasValue());
        const auto fallbackShape = std::move(fallbackResult).Value();
        REQUIRE(fallbackShape.IsValid());
        REQUIRE(fallbackShape.Runs().size() == 1);
        REQUIRE(fallbackShape.Runs().front().face == symbols.Id());
        REQUIRE_FALSE(fallbackShape.Clusters().front().missing);

        const auto rtlResult = fallback.Shape(Request("abc", RequireValue(UiTextScript::Create("Latn")),
                                                      RequireValue(UiTextLanguage::Create("en")), UiTextDirection::RightToLeft));
        REQUIRE(rtlResult.HasValue());
        const auto rtlShape = std::move(rtlResult).Value();
        REQUIRE(rtlShape.IsValid());
        REQUIRE(rtlShape.Descriptor().requestedDirection == UiTextDirection::RightToLeft);
        REQUIRE(rtlShape.Runs().front().direction == UiTextDirection::RightToLeft);
        REQUIRE(rtlShape.Runs().front().script == RequireValue(UiTextScript::Create("Latn")));
    }

    TEST_CASE("Runtime UI missing-glyph policies and lifecycle preserve typed outcomes", "[runtime_ui][text][lifecycle]") {
        const auto face = LoadFont(5, "assets/fonts/ibm-plex-mono/IBMPlexMono-Regular.ttf");
        const std::string unsupported{"\xF4\x8F\xBF\xBF", 4};

        auto strict = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::FailStrict), 31, 1, 1);
        RequireCode(strict.Shape(Request(unsupported)), UiErrors::TextMissingCoverage);

        auto omit = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::OmitWithAdvance), 32, 2, 1);
        const auto omittedResult = omit.Shape(Request(unsupported));
        REQUIRE(omittedResult.HasValue());
        const auto omitted = std::move(omittedResult).Value();
        REQUIRE(omitted.IsValid());
        REQUIRE(omitted.Glyphs().empty());
        REQUIRE(omitted.Clusters().size() == 1);
        REQUIRE(omitted.Clusters().front().missing);
        REQUIRE((omitted.Metrics().advance.x != 0 || omitted.Metrics().advance.y != 0));

        auto replacement = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::Replacement), 33, 3, 1);
        const auto replacementResult = replacement.Shape(Request(unsupported));
        REQUIRE(replacementResult.HasValue());
        const auto replaced = std::move(replacementResult).Value();
        REQUIRE(replaced.IsValid());
        REQUIRE_FALSE(replaced.Glyphs().empty());
        REQUIRE(replaced.Clusters().front().missing);

        auto lifecycle = MakeShaper(Chain(std::array{face}, UiMissingGlyphPolicy::Replacement), 34, 4, 1);
        {
            const auto liveResult = lifecycle.Shape(Request("live"));
            REQUIRE(liveResult.HasValue());
            auto live = std::move(liveResult).Value();
            REQUIRE_FALSE(lifecycle.Shape(Request("second")).HasValue());
            lifecycle.Shutdown();
            REQUIRE(lifecycle.State() == UiTextShaperState::Closed);
            REQUIRE_FALSE(lifecycle.IsDrained());
            RequireCode(lifecycle.Shape(Request("closed")), UiErrors::TextLifecycleUnavailable);
            REQUIRE(live.IsValid());
        }
        REQUIRE(lifecycle.IsDrained());
    }
}  // namespace Horo::Runtime::Ui
