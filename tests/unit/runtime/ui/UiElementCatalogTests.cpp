#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiElementCatalog.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Id(const std::uint8_t marker) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return T::Create(bytes).Value();
    }

    UiElementPresentation Presentation() {
        UiElementPresentation result;
        result.layout.width = UiLength::Percent(32);
        result.layout.height = UiLength::Dip(640);
        result.layout.padding = {1, 2, 3, 4};
        result.layout.margin = {-1, 0, 1, 0};
        result.layout.anchors.horizontal = {0, 64, UiLayoutAlignment::Stretch, false};
        result.layout.container.kind = UiLayoutContainerKind::Grid;
        result.layout.container.columnCount = 2;
        result.layout.container.columns[0] = UiGridTrack::Fraction(1);
        result.layout.container.columns[1] = UiGridTrack::Percent(32);
        result.layout.grid.columnSpan = 2;
        result.styleAsset = Id<RuntimeStyleAssetId>(40);
        result.typeClass = {result.styleAsset, Id<UiStyleClassId>(41)};
        result.classes.push_back({result.styleAsset, Id<UiStyleClassId>(42)});
        const auto asset = Assets::AssetId::FromBytes(Id<UiDocumentId>(50).Bytes());
        const std::array<UiStyleValue, 8> values{
            UiStyleColor{0.1F, 0.2F, 0.3F, 0.4F, UiStyleColorRole::Surface},
            UiStyleDimension{123},
            UiStyleTypography{asset},
            UiStyleImage{asset},
            UiStyleShape{1, 2, 3, -4, 5, 6},
            UiStyleScalar{0.25F},
            UiStyleEnumValue{7},
            UiStyleMotionReference{1000, 1},
        };
        for (std::size_t index = 0; index < values.size(); ++index)
            result.inlineStyle.push_back(
                {Id<UiStylePropertyId>(static_cast<std::uint8_t>(index + 60)), UiStyleValueSource::Literal(values[index]), false});
        result.inlineStyle.push_back(
            {Id<UiStylePropertyId>(70), UiStyleValueSource::Token({result.styleAsset, Id<UiStyleTokenId>(71)}), false});
        REQUIRE(result.IsValid());
        return result;
    }

    UiDocument Document(UiElementPresentation presentation) {
        UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
        REQUIRE(
            builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .referenceResolution = {800, 600}}).HasValue());
        auto root = CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value();
        root.presentation = std::move(presentation);
        REQUIRE(builder.AddElement(std::move(root)).HasValue());
        return std::move(builder).Build().Value();
    }
}  // namespace

TEST_CASE("Core catalog has exact stable identities containment and descriptor driven primitive creation", "[runtime_ui][catalog]") {
    REQUIRE(UiElementCatalog::All().size() == 14);
    for (const auto &type : UiElementCatalog::All()) {
        REQUIRE(UiElementCatalog::Find(type.kind) == &type);
        REQUIRE(UiElementCatalog::Find(type.documentType) == &type);
        auto primitive =
            CreateUiElement(type.kind, Id<UiElementId>(3), type.kind == UiElementKind::Canvas ? UiElementId{} : Id<UiElementId>(4));
        REQUIRE(primitive.HasValue());
        REQUIRE(ValidateCoreUiElement(primitive.Value()).HasValue());
        REQUIRE(primitive.Value().properties.empty());
        REQUIRE(primitive.Value().presentation.has_value());
    }
    REQUIRE(UiElementCatalog::Find(static_cast<UiElementKind>(0)) == nullptr);
    REQUIRE(UiElementCatalog::Find("horo.ui.unknown") == nullptr);
    REQUIRE(CreateUiElement(static_cast<UiElementKind>(999), Id<UiElementId>(3), {}).HasError());
    REQUIRE(CreateUiElement(UiElementKind::Panel, {}, Id<UiElementId>(3)).HasError());
}

TEST_CASE("Typed presentation round trips full layout style literals references and cooked values",
          "[runtime_ui][catalog][serialization][cook]") {
    const auto source = Document(Presentation());
    const auto encoded = SerializeUiDocument(source);
    REQUIRE(encoded.HasValue());
    const auto decoded = DeserializeUiDocument(encoded.Value());
    REQUIRE(decoded.HasValue());
    REQUIRE(std::ranges::equal(source.Elements(), decoded.Value().Elements()));
    REQUIRE(SerializeUiDocument(decoded.Value()).Value() == encoded.Value());
    const auto cooked = CookedUiDocument::Cook(source);
    REQUIRE(cooked.HasValue());
    const auto uncooked = CookedUiDocument::Decode(cooked.Value().Payload());
    REQUIRE(uncooked.HasValue());
    REQUIRE(std::ranges::equal(source.Elements(), uncooked.Value().Elements()));
    REQUIRE(CookedUiDocument::Cook(source).Value().Payload().size() == cooked.Value().Payload().size());
    auto truncated = std::vector<std::uint8_t>{cooked.Value().Payload().begin(), cooked.Value().Payload().end() - 1};
    REQUIRE(CookedUiDocument::Decode(truncated).HasError());
}

TEST_CASE("Typed presentation rejects malformed duplicate unknown oversized and conflicting authored data",
          "[runtime_ui][catalog][serialization]") {
    const auto source = Document(Presentation());
    const auto encoded = nlohmann::json::parse(SerializeUiDocument(source).Value());
    auto invalid = encoded;
    invalid["elements"][0]["presentation"]["layout"]["lengths"][0][0] = 255;
    REQUIRE(DeserializeUiDocument(invalid.dump()).HasError());
    invalid = encoded;
    invalid["elements"][0]["presentation"]["layout"]["padding"][0] = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(DeserializeUiDocument(invalid.dump()).HasError());
    invalid = encoded;
    invalid["elements"][0]["presentation"]["inlineStyle"].push_back(invalid["elements"][0]["presentation"]["inlineStyle"][0]);
    REQUIRE(DeserializeUiDocument(invalid.dump()).HasError());
    invalid = encoded;
    invalid["elements"][0]["presentation"]["version"] = 99;
    REQUIRE(DeserializeUiDocument(invalid.dump()).HasError());
    invalid = encoded;
    invalid["elements"][0]["presentation"]["classes"] = nlohmann::json::array();
    for (std::size_t index = 0; index <= MaximumUiStyleClasses; ++index)
        invalid["elements"][0]["presentation"]["classes"].push_back(nullptr);
    REQUIRE(DeserializeUiDocument(invalid.dump()).HasError());
    invalid = encoded;
    invalid["elements"][0]["properties"].push_back({{"key", "width"}, {"value", {{"type", "int64"}, {"value", 2}}}});
    REQUIRE(DeserializeUiDocument(invalid.dump()).HasError());
    auto duplicate = encoded.dump();
    const auto member = duplicate.find("\"presentation\":");
    REQUIRE(member != std::string::npos);
    duplicate.insert(member, "\"presentation\":{},");
    REQUIRE(DeserializeUiDocument(duplicate).HasError());
}

TEST_CASE("Legacy element presentation absence survives source and older cooked formats", "[runtime_ui][catalog][migration]") {
    UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value(), {1, 1}};
    REQUIRE(builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .referenceResolution = {800, 600}}).HasValue());
    REQUIRE(builder.AddElement({Id<UiElementId>(3), {}, Assets::AssetTypeId::Parse("legacy.panel").Value(), {}, {}}).HasValue());
    auto source = std::move(builder).Build().Value();
    auto parsed = DeserializeUiDocument(SerializeUiDocument(source).Value());
    REQUIRE(parsed.HasValue());
    REQUIRE_FALSE(parsed.Value().Elements()[0].presentation.has_value());
    auto cooked = CookedUiDocument::Cook(source).Value();
    auto v2 = std::vector<std::uint8_t>{cooked.Payload().begin(), cooked.Payload().end()};
    // This fixture has no dependencies/routes. Remove format 4's length-prefixed empty
    // profile array and format 3's element-presentation absence flag before decoding as v2.
    const std::array<std::uint8_t, 7> currentTail{0, 2, 0, 0, 0, '[', ']'};
    REQUIRE(v2[8] == 4);
    REQUIRE(v2.size() >= currentTail.size());
    REQUIRE(std::equal(currentTail.begin(), currentTail.end(), v2.end() - currentTail.size()));
    v2.resize(v2.size() - currentTail.size());
    v2[8] = 2;
    auto older = CookedUiDocument::Decode(v2);
    REQUIRE(older.HasValue());
    REQUIRE_FALSE(older.Value().Elements()[0].presentation.has_value());
}
