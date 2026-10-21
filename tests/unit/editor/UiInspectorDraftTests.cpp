#include "UiInspectorDraft.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Stable(const std::uint8_t value) {
        SerializedUiId bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    UiInspectorContext Context() {
        return {.document = DocumentInstanceId::Create(1).Value(),
                .state = {1, DocumentInstanceId::Create(1).Value()},
                .revision = UiDocumentRevision::Create(1).Value(),
                .element = Stable<UiElementId>(1),
                .selectionRevision = 1,
                .styles = RuntimeStyleGeneration::Create(1).Value()};
    }

    UiInspectorValues Values() {
        return {.layout = {},
                .style =
                    UiStyleAssignment{.property = Stable<UiStylePropertyId>(1), .value = UiStyleValueSource::Literal(UiStyleScalar{0.5F})}};
    }

    RuntimeStyleRegistry Registry(UiStyleValue literal = UiStyleScalar{0.5F}) {
        UiStyleRegistryDefinition definition;
        definition.properties.push_back({.id = Stable<UiStylePropertyId>(1),
                                         .category = UiStyleValueCategoryOf(literal),
                                         .defaultValue = literal,
                                         .minimumScalar = 0,
                                         .maximumScalar = 1});
        definition.assets.push_back({.id = Stable<RuntimeStyleAssetId>(1),
                                     .tokens = {{.id = Stable<UiStyleTokenId>(1),
                                                 .category = UiStyleValueCategory::Scalar,
                                                 .value = UiStyleValueSource::Literal(UiStyleScalar{0.75F})},
                                                {.id = Stable<UiStyleTokenId>(2),
                                                 .category = UiStyleValueCategory::Color,
                                                 .value = UiStyleValueSource::Literal(UiStyleColor{})}}});
        auto result = RuntimeStyleRegistry::Create(std::move(definition), Context().styles);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    void Text(UiInspectorInteger &target, const std::string_view text) {
        REQUIRE(text.size() < target.bytes.size());
        target.bytes.fill('\0');
        std::copy(text.begin(), text.end(), target.bytes.begin());
    }
}  // namespace

TEST_CASE("UI inspector integer input preserves invalid text and the last typed value", "[editor][ui][inspector]") {
    UiInspectorInteger text;
    std::int32_t value = 64;
    for (const auto invalid : {"", "-", "1.5", "64dip", "2147483648", "-2147483649", " 1", "1 "}) {
        Text(text, invalid);
        CHECK_FALSE(text.Parse(value));
        CHECK(value == 64);
        CHECK(std::string_view{text.bytes.data()} == invalid);
    }
    text.bytes.fill('9');
    CHECK_FALSE(text.Parse(value));
    CHECK(value == 64);
    for (const auto exact : {std::numeric_limits<std::int32_t>::min(), 0, std::numeric_limits<std::int32_t>::max()}) {
        text.Capture(exact);
        REQUIRE(text.Parse(value));
        CHECK(value == exact);
    }
    std::uint32_t flags{};
    text.Capture(std::numeric_limits<std::uint32_t>::max());
    REQUIRE(text.Parse(flags));
    CHECK(flags == std::numeric_limits<std::uint32_t>::max());
    std::uint8_t narrow = 4;
    Text(text, "256");
    CHECK_FALSE(text.Parse(narrow));
    CHECK(narrow == 4);
}

TEST_CASE("UI inspector retains staged values on unchanged frames and cancels without publication", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto registry = Registry();
    REQUIRE(draft.Begin(Context(), Values()));
    draft.Values().layout.width = UiLength::Dip(321);
    CHECK_FALSE(draft.Begin(Context(), Values()));
    CHECK(draft.Values().layout.width == UiLength::Dip(321));
    auto request = draft.Prepare(Context(), &registry, true);
    REQUIRE(request.HasValue());
    CHECK(request.Value().values.layout.width == UiLength::Dip(321));
    CHECK(Values().layout.width == UiLength::Auto());
    draft.Cancel();
    CHECK(draft.Values().layout.width == UiLength::Auto());
    CHECK(draft.Prepare(Context(), &registry, true).HasValue());
}

TEST_CASE("UI inspector rejects invalid layout, category, scalar range and retained text before preparing", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto registry = Registry();
    REQUIRE(draft.Begin(Context(), Values()));
    draft.Values().layout.padding.left = -1;
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Cancel();
    draft.Values().layout.width = {static_cast<UiLengthKind>(255), 64};
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Cancel();
    CHECK(draft.Prepare(Context(), &registry, false).HasError());
    for (const float invalid : {-1.0F, 2.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        draft.Values().style->value = UiStyleValueSource::Literal(UiStyleScalar{invalid});
        CHECK(draft.Prepare(Context(), &registry, true).HasError());
    }
    draft.Values().style->value = UiStyleValueSource::Literal(UiStyleDimension{64});
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Cancel();
    draft.Values().style->property = Stable<UiStylePropertyId>(2);
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Cancel();
    draft.Values().style->sealed = true;
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Cancel();
    CHECK(draft.Prepare(Context(), &registry, true).HasValue());
}

TEST_CASE("UI inspector fences document identity, reload/history state, selection and style generation", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto registry = Registry();
    const auto original = Context();
    REQUIRE(draft.Begin(original, Values()));
    auto current = original;
    current.document = DocumentInstanceId::Create(2).Value();
    CHECK(draft.Prepare(current, &registry, true).HasError());
    current = original;
    ++current.state.value;
    CHECK(draft.Prepare(current, &registry, true).HasError());
    current = original;
    current.revision = UiDocumentRevision::Create(2).Value();
    CHECK(draft.Prepare(current, &registry, true).HasError());
    current = original;
    ++current.selectionRevision;
    CHECK(draft.Prepare(current, &registry, true).HasError());
    current = original;
    current.element = Stable<UiElementId>(2);
    CHECK(draft.Prepare(current, &registry, true).HasError());
    current = original;
    current.styles = RuntimeStyleGeneration::Create(2).Value();
    CHECK(draft.Prepare(current, &registry, true).HasError());
    draft.Values().layout.width = UiLength::Dip(123);
    REQUIRE(draft.Begin(current, Values()));
    CHECK(draft.Values().layout.width == UiLength::Auto());
    CHECK(draft.Prepare(current, &registry, true).HasError());
}

TEST_CASE("UI inspector closes admission on detach and registry retirement or shutdown", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto registry = Registry();
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    REQUIRE(draft.Begin(Context(), Values()));
    REQUIRE(registry.BeginRetirement().HasValue());
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    registry.Shutdown();
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Clear();
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Cancel();
    auto replacement = Registry();
    REQUIRE(draft.Begin(Context(), Values()));
    CHECK(draft.Prepare(Context(), &replacement, true).HasValue());
    CHECK(draft.Begin({}, Values()));
    CHECK(draft.Prepare(Context(), &replacement, true).HasError());
}

TEST_CASE("UI inspector resolves token values through the supplied registry", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto registry = Registry();
    REQUIRE(draft.Begin(Context(), Values()));
    draft.Values().style->value = UiStyleValueSource::Token({Stable<RuntimeStyleAssetId>(1), Stable<UiStyleTokenId>(1)});
    CHECK(draft.Prepare(Context(), &registry, true).HasValue());
    draft.Values().style->value = UiStyleValueSource::Token({Stable<RuntimeStyleAssetId>(1), Stable<UiStyleTokenId>(2)});
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
    draft.Values().style->value = UiStyleValueSource::Token({Stable<RuntimeStyleAssetId>(2), Stable<UiStyleTokenId>(1)});
    CHECK(draft.Prepare(Context(), &registry, true).HasError());
}

TEST_CASE("UI inspector allows layout-only edits without inventing a style override", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto values = Values();
    values.style.reset();
    auto context = Context();
    context.styles = {};
    REQUIRE(draft.Begin(context, values));
    draft.Values().layout.width = UiLength::Dip(500);
    const auto prepared = draft.Prepare(context, nullptr, true);
    REQUIRE(prepared.HasValue());
    CHECK_FALSE(prepared.Value().values.style.has_value());
    CHECK(prepared.Value().values.layout.width == UiLength::Dip(500));
    draft.Values().layout.padding.left = -1;
    CHECK(draft.Prepare(context, nullptr, true).HasError());
    draft.Cancel();
    CHECK(draft.Prepare(context, nullptr, true).HasValue());
}

TEST_CASE("UI inspector prepares every canonical style category using its registered descriptor", "[editor][ui][inspector]") {
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 9;
    const auto asset = Horo::Assets::AssetId::FromBytes(bytes);
    const std::array<UiStyleValue, 8> literals{UiStyleColor{},
                                               UiStyleDimension{64},
                                               UiStyleTypography{.family = asset},
                                               UiStyleImage{.asset = asset},
                                               UiStyleShape{},
                                               UiStyleScalar{0.5F},
                                               UiStyleEnumValue{std::numeric_limits<std::uint32_t>::max()},
                                               UiStyleMotionReference{}};
    for (const auto &literal : literals) {
        auto registry = Registry(literal);
        auto values = Values();
        values.style->value = UiStyleValueSource::Literal(literal);
        UiInspectorDraft draft;
        REQUIRE(draft.Begin(Context(), values));
        const auto prepared = draft.Prepare(Context(), &registry, true);
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().values.style->value.literal == literal);
    }
}

TEST_CASE("UI inspector rejects wrong intrinsic literal shape before preparing", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto registry = Registry();
    REQUIRE(draft.Begin(Context(), Values()));
    const std::array<UiStyleValue, 5> malformed{UiStyleColor{.red = 2}, UiStyleTypography{}, UiStyleImage{}, UiStyleShape{.radius = -1},
                                                UiStyleMotionReference{.durationMicroseconds = 60'000'001}};
    for (const auto &literal : malformed) {
        draft.Values().style->value = UiStyleValueSource::Literal(literal);
        CHECK(draft.Prepare(Context(), &registry, true).HasError());
    }
}

TEST_CASE("UI inspector property selection replaces a stale staged property without mutating the baseline", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    const auto original = Values();
    REQUIRE(draft.Begin(Context(), original));
    draft.Values().layout.width = UiLength::Dip(500);
    auto selected = Values();
    selected.style->property = Stable<UiStylePropertyId>(2);
    REQUIRE(draft.Begin(Context(), selected));
    CHECK(draft.Values().layout.width == UiLength::Auto());
    CHECK(draft.Values().style->property == selected.style->property);
    draft.Values().layout.width = UiLength::Dip(800);
    draft.Cancel();
    CHECK(draft.Values().layout.width == UiLength::Auto());
    CHECK(original.style->property == Stable<UiStylePropertyId>(1));
}

TEST_CASE("UI inspector rejects invalid bounded layout relationships without a style registry", "[editor][ui][inspector]") {
    auto values = Values();
    values.style.reset();
    UiInspectorDraft draft;
    REQUIRE(draft.Begin(Context(), values));
    SECTION("oversized authored columns") {
        draft.Values().layout.container.columnCount = MaximumUiGridTracks + 1;
    }
    SECTION("oversized authored rows") {
        draft.Values().layout.container.rowCount = MaximumUiGridTracks + 1;
    }
    SECTION("zero track fraction") {
        draft.Values().layout.container.columnCount = 1;
        draft.Values().layout.container.columns[0] = UiGridTrack::Fraction(0);
    }
    SECTION("fraction over normalized percent") {
        draft.Values().layout.container.rowCount = 1;
        draft.Values().layout.container.rows[0] = UiGridTrack::Percent(UiScalarUnitsPerDip + 1);
    }
    SECTION("zero grid span") {
        draft.Values().layout.grid.columnSpan = 0;
    }
    SECTION("oversized flex weight") {
        draft.Values().layout.flex.grow = MaximumUiFlexFactor + 1;
    }
    SECTION("anchor outside normalized parent") {
        draft.Values().layout.anchors.horizontal.start = UiScalarUnitsPerDip + 1;
    }
    CHECK(draft.Prepare(Context(), nullptr, true).HasError());
    draft.Cancel();
    CHECK(draft.Prepare(Context(), nullptr, true).HasValue());
}

TEST_CASE("UI inspector preserves the canonical minimum-wins layout policy", "[editor][ui][inspector]") {
    UiInspectorDraft draft;
    auto values = Values();
    values.style.reset();
    REQUIRE(draft.Begin(Context(), values));
    draft.Values().layout.minimumWidth = UiLength::Dip(200);
    draft.Values().layout.maximumWidth = UiLength::Dip(100);
    // ADR-074 permits this input; the layout owner resolves minimum precedence and emits its diagnostic.
    const auto prepared = draft.Prepare(Context(), nullptr, true);
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().values.layout.minimumWidth == UiLength::Dip(200));
    CHECK(prepared.Value().values.layout.maximumWidth == UiLength::Dip(100));
}
