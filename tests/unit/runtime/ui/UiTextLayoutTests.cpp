#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiTextLayout.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        SerializedUiId StableBytes(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return bytes;
        }

        template <typename Id> Id Stable(const std::uint8_t marker) {
            return Id::Create(StableBytes(marker)).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 37) {
            return Rev<UiOwnershipGeneration>(value);
        }

        UiTextFaceId Face(const std::uint8_t marker = 1) {
            return Stable<UiTextFaceId>(marker);
        }

        UiTextLayoutSource Source(const std::uint64_t content = 2, const std::uint64_t tree = 3) {
            const auto owner = Owner();
            const auto revisions = UiLayoutSourceRevisions{Rev<UiDocumentRevision>(2),
                                                           Rev<UiRuntimeTreeRevision>(tree),
                                                           Rev<UiLayoutContentRevision>(content),
                                                           Rev<UiLayoutStyleRevision>(4),
                                                           Rev<UiLayoutIntrinsicRevision>(5),
                                                           Rev<UiLayoutCanvasRevision>(6),
                                                           Rev<UiLayoutPolicyRevision>(7)};
            return {{owner, 1, 1}, {owner, 2, 1}, {owner, 3, 1}, Stable<UiDocumentId>(1), revisions};
        }

        UiTextLayoutEngine MakeEngine(const std::uint32_t concurrentResults = 3) {
            const auto source = Source();
            auto result = UiTextLayoutEngine::Create({source.instance,
                                                      source.canvas,
                                                      source.document,
                                                      source.element,
                                                      {64, 64, 64, 32, 64},
                                                      concurrentResults,
                                                      Rev<UiTextLayoutRevision>(1)});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct ShapedText final {
            std::array<UiTextShapedGlyph, 3> glyphs{{{11, {0, 0}, {64, 0}}, {12, {0, 0}, {64, 0}}, {13, {0, 0}, {64, 0}}}};
            std::array<UiTextShapedRun, 1> runs{{{Face(), 0, 3, UiTextFlowDirection::LeftToRight}}};
            std::array<UiTextShapedCluster, 3> clusters{{{0, 1, 0, 1, {64, 0}, UiTextBreakOpportunity::Optional},
                                                         {1, 2, 1, 1, {64, 0}, UiTextBreakOpportunity::Optional},
                                                         {2, 3, 2, 1, {64, 0}, UiTextBreakOpportunity::None}}};

            UiTextShapedTextView View(const std::uint64_t content = 2) const {
                return {Rev<UiLayoutContentRevision>(content), 3, {48, 16, 4}, runs, glyphs, clusters};
            }
        };

        struct TwoClusterText final {
            std::array<UiTextShapedGlyph, 2> glyphs{{{21, {0, 0}, {64, 0}}, {22, {0, 0}, {64, 0}}}};
            std::array<UiTextShapedRun, 1> runs{{{Face(), 0, 2, UiTextFlowDirection::LeftToRight}}};
            std::array<UiTextShapedCluster, 2> clusters{
                {{0, 1, 0, 1, {64, 0}, UiTextBreakOpportunity::Optional}, {1, 2, 1, 1, {64, 0}, UiTextBreakOpportunity::None}}};

            UiTextShapedTextView View(const std::uint64_t content = 2) const {
                return {Rev<UiLayoutContentRevision>(content), 2, {48, 16, 4}, runs, glyphs, clusters};
            }
        };

        struct EllipsisText final {
            std::array<UiTextShapedGlyph, 1> glyphs{{{99, {0, 0}, {64, 0}}}};
            std::array<UiTextShapedRun, 1> runs{{{Face(), 0, 1, UiTextFlowDirection::LeftToRight}}};
            std::array<UiTextShapedCluster, 1> clusters{{{0, 1, 0, 1, {64, 0}, UiTextBreakOpportunity::None}}};

            UiTextShapedTextView View(const std::uint64_t content = 2) const {
                return {Rev<UiLayoutContentRevision>(content), 1, {48, 16, 4}, runs, glyphs, clusters};
            }
        };

        template <typename Text>
        UiTextLayoutRequest Request(const Text &text, const std::int32_t width = 128, const std::uint64_t content = 2) {
            return {Source(content), {{0, 0}, {width, 256}}, {{0, 0}, {width, 256}}, {}, text.View(content), std::nullopt};
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Text layout shares shaped cluster advances with measurement and render-order glyph placement",
                  "[runtime_ui][text_layout]") {
            TwoClusterText text;
            auto engine = MakeEngine();
            auto request = Request(text, 96);
            request.options.horizontal = UiTextHorizontalAlignment::Center;
            const auto result = engine.Layout(request);
            REQUIRE(result.HasValue());
            const auto layout = std::move(result).Value();

            REQUIRE(layout.Measurement().desired == UiLogicalExtent{64, 136});
            REQUIRE(layout.Measurement().dependsOnParentWidth);
            REQUIRE(layout.Lines().size() == 2);
            REQUIRE(layout.Glyphs().size() == 2);
            REQUIRE(layout.Glyphs()[0].glyph == 21);
            REQUIRE(layout.Glyphs()[1].glyph == 22);
            REQUIRE(layout.Lines()[0].origin.x == 16);
            REQUIRE(layout.Lines()[1].origin.y == 68);
            REQUIRE(layout.Runs().size() == 2);
            REQUIRE(layout.Overflow().overflow == UiLogicalExtent{64, 136});
        }

        TEST_CASE("Text layout wraps only at declared cluster opportunities and preserves mandatory breaks",
                  "[runtime_ui][text_layout][wrapping]") {
            ShapedText text;
            auto engine = MakeEngine();
            auto request = Request(text, 96);
            const auto wrapped = engine.Layout(request);
            REQUIRE(wrapped.HasValue());
            REQUIRE(wrapped.Value().Lines().size() == 3);
            REQUIRE(wrapped.Value().Lines()[0].glyphCount == 1);
            REQUIRE(wrapped.Value().Lines()[1].glyphCount == 1);
            REQUIRE(wrapped.Value().Lines()[2].glyphCount == 1);

            text.clusters[1].breakOpportunity = UiTextBreakOpportunity::Mandatory;
            request = Request(text, 256);
            const auto hardBreak = engine.Layout(request);
            REQUIRE(hardBreak.HasValue());
            REQUIRE(hardBreak.Value().Lines().size() == 2);
            REQUIRE(hardBreak.Value().Lines()[0].hardBreak);
            REQUIRE(hardBreak.Value().Lines()[0].glyphCount == 2);
            REQUIRE(hardBreak.Value().Lines()[1].glyphCount == 1);
        }

        TEST_CASE("Text layout clips without deleting source glyphs and scales logical measurement deterministically",
                  "[runtime_ui][text_layout][edge]") {
            TwoClusterText text;
            auto engine = MakeEngine();
            auto request = Request(text, 64);
            request.options.wrap = UiTextWrapMode::NoWrap;
            request.options.overflow = UiTextOverflowMode::Clip;
            request.options.scale = UiTextScale::Create(2048).Value();
            request.constraints.maximum = {256, 256};
            request.assignedContent.extent = {64, 256};
            const auto clipped = engine.Layout(request);
            REQUIRE(clipped.HasValue());
            REQUIRE(clipped.Value().Glyphs().size() == 2);
            REQUIRE(clipped.Value().Overflow().overflow.width == 256);
            REQUIRE(clipped.Value().Overflow().clipped);
            REQUIRE_FALSE(clipped.Value().Overflow().truncated);
            REQUIRE(clipped.Value().Glyphs()[1].origin.x == 128);
        }

        TEST_CASE("Text layout appends a pre-shaped ellipsis only after bounded truncation", "[runtime_ui][text_layout][overflow]") {
            ShapedText text;
            EllipsisText ellipsis;
            auto engine = MakeEngine();
            auto request = Request(text, 128);
            request.options.wrap = UiTextWrapMode::NoWrap;
            request.options.overflow = UiTextOverflowMode::Ellipsis;
            request.options.maxLines = 1;
            request.ellipsis = ellipsis.View();
            const auto result = engine.Layout(request);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Lines().size() == 1);
            REQUIRE(result.Value().Lines()[0].ellipsis);
            REQUIRE(result.Value().Overflow().truncated);
            REQUIRE(result.Value().Glyphs().size() == 2);
            REQUIRE(result.Value().Glyphs()[1].glyph == 99);
            REQUIRE(result.Value().Glyphs()[1].cluster == NoUiTextLayoutCluster);

            request.ellipsis.reset();
            RequireError(engine.Layout(request), UiErrors::TextLayoutEllipsisInvalid);
        }

        TEST_CASE("Text layout rejects malformed shaped evidence and stale source generations", "[runtime_ui][text_layout][validation]") {
            TwoClusterText text;
            auto engine = MakeEngine();
            auto request = Request(text);
            request.options.wrap = UiTextWrapMode::Count;
            RequireError(engine.Layout(request), UiErrors::TextLayoutInputInvalid);

            request = Request(text);
            REQUIRE(engine.Layout(request).HasValue());
            request = Request(text, 128, 1);
            RequireError(engine.Layout(request), UiErrors::TextLayoutSourceStale);

            text.runs[0].glyphCount = 1;
            auto malformed = Request(text);
            RequireError(engine.Layout(malformed), UiErrors::TextLayoutInputInvalid);
        }

        TEST_CASE("Text layout result leases survive replacement, slot pressure, and engine shutdown",
                  "[runtime_ui][text_layout][lifetime]") {
            TwoClusterText text;
            auto engine = MakeEngine(2);
            auto firstResult = engine.Layout(Request(text));
            REQUIRE(firstResult.HasValue());
            std::optional<UiTextLayoutResult> first{std::move(firstResult).Value()};
            auto replacementRequest = Request(text, 96, 3);
            auto secondResult = engine.Layout(replacementRequest);
            REQUIRE(secondResult.HasValue());
            std::optional<UiTextLayoutResult> second{std::move(secondResult).Value()};
            RequireError(engine.Layout(Request(text, 96, 4)), UiErrors::TextLayoutStorageExhausted);
            first.reset();
            REQUIRE(engine.Layout(Request(text, 96, 4)).HasValue());
            second.reset();

            auto retainedResult = engine.Layout(Request(text, 96, 5));
            REQUIRE(retainedResult.HasValue());
            std::optional<UiTextLayoutResult> retained{std::move(retainedResult).Value()};
            engine.Shutdown();
            REQUIRE(engine.State() == UiTextLayoutEngineState::Closed);
            REQUIRE(engine.IsDrained() == false);
            REQUIRE(retained->IsValid());
            RequireError(engine.Layout(Request(text, 96, 6)), UiErrors::TextLayoutLifecycleUnavailable);
            retained.reset();
            REQUIRE(engine.IsDrained());
        }

        TEST_CASE("Text layout reuses all frame-hot storage after creation", "[runtime_ui][text_layout][allocation]") {
            TwoClusterText text;
            auto engine = MakeEngine();
            REQUIRE(engine.Layout(Request(text)).HasValue());
            const auto before = Horo::Tests::AllocationProbe::Count();
            {
                auto request = Request(text, 96, 3);
                const auto result = engine.Layout(request);
                REQUIRE(result.HasValue());
            }
            REQUIRE(Horo::Tests::AllocationProbe::Count() == before);
        }

        static_assert(!std::is_copy_constructible_v<UiTextLayoutEngine>);
        static_assert(std::is_copy_constructible_v<UiTextLayoutResult>);
    }  // namespace
}  // namespace Horo::Runtime::Ui
