#include "Horo/Runtime/Ui/UiStyleAsset.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;

    Assets::AssetId Asset(const std::string_view text = "13a2bc7e-d668-4def-9c4e-f2267172e846") {
        return Assets::AssetId::Parse(text).Value();
    }

    UiStyleAssetDocument Document() {
        UiStyleAssetDocument document{UiStyleContentRevision::Create(1).Value(),
                                      {.id = RuntimeStyleAssetId::Create(Asset().Bytes()).Value()},
                                      {},
                                      {}};
        document.style.assignments = {
            {CoreUiStylePropertyId(UiCoreStyleProperty::Fill), UiStyleValueSource::Literal(UiStyleColor{0.2F, 0.3F, 0.4F, 1}), false}};
        return document;
    }
    enum class AssignmentContainer {
        Namespace,
        NamespaceState,
        Class,
        ClassState
    };

    /** @brief Inspect authored assignment order through the decoded typed container. */
    const std::vector<UiStyleAssignment> &Assignments(const UiStyleAssetDocument &document, const AssignmentContainer container) {
        if (container == AssignmentContainer::NamespaceState)
            return document.style.states.at(0).assignments;
        if (container == AssignmentContainer::Class)
            return document.style.classes.at(0).assignments;
        if (container == AssignmentContainer::ClassState)
            return document.style.classes.at(0).states.at(0).assignments;
        return document.style.assignments;
    }
}  // namespace

TEST_CASE("Canonical style source and cooked payload preserve full sidecar identity and one explicit schema",
          "[runtime_ui][style][asset]") {
    const auto document = Document();
    const auto schema = CreateCoreUiStyleSchema({}).Value();
    auto source = SerializeUiStyleAsset(document);
    REQUIRE(source.HasValue());
    auto parsed = DeserializeUiStyleAsset(source.Value());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().style.id.Bytes() == Asset().Bytes());
    const std::array documents{parsed.Value()};
    REQUIRE(PrepareUiStyleRegistry(documents, schema, RuntimeStyleGeneration::Create(1).Value()).HasValue());
    auto cooked = CookUiStyleAsset(parsed.Value(), schema);
    REQUIRE(cooked.HasValue());
    auto decoded = DecodeCookedUiStyleAsset(cooked.Value(), Asset(), schema);
    REQUIRE(decoded.HasValue());
    CHECK(SerializeUiStyleAsset(decoded.Value()).Value() == source.Value());
    CHECK(CookUiStyleAsset(decoded.Value(), schema).Value() == cooked.Value());
    CHECK(DecodeCookedUiStyleAsset(cooked.Value(), Asset("02f55810-e86a-434a-9d31-b51c91967eb4"), schema).HasError());
    auto changed = schema;
    changed.fingerprint.bytes[31] ^= 1;
    CHECK(DecodeCookedUiStyleAsset(cooked.Value(), Asset(), changed).HasError());
}

TEST_CASE("Style encoding preserves authored assignment order and rejects repeated property identities", "[runtime_ui][style][asset]") {
    auto document = Document();
    auto *assignments = &document.style.assignments;
    AssignmentContainer container{};
    SECTION("namespace assignments") {
        container = AssignmentContainer::Namespace;
    }
    SECTION("namespace state assignments") {
        document.style.states.push_back({UiVisualStateMask{UiVisualState::Hovered}, {}, UiStateLayer::Pointer, {}});
        assignments = &document.style.states.back().assignments;
        container = AssignmentContainer::NamespaceState;
    }
    SECTION("class assignments") {
        document.style.classes.push_back({.id = UiStyleClassId::Create(Asset().Bytes()).Value()});
        assignments = &document.style.classes.back().assignments;
        container = AssignmentContainer::Class;
    }
    SECTION("class state assignments") {
        document.style.classes.push_back({.id = UiStyleClassId::Create(Asset().Bytes()).Value()});
        document.style.classes.back().states.push_back({UiVisualStateMask{UiVisualState::Hovered}, {}, UiStateLayer::Pointer, {}});
        assignments = &document.style.classes.back().states.back().assignments;
        container = AssignmentContainer::ClassState;
    }
    *assignments = {{CoreUiStylePropertyId(UiCoreStyleProperty::Fill), UiStyleValueSource::Literal(UiStyleColor{0.2F, 0.3F, 0.4F, 1}),
                     false},
                    {CoreUiStylePropertyId(UiCoreStyleProperty::Width), UiStyleValueSource::Literal(UiStyleDimension{42}), true}};
    const auto encoded = SerializeUiStyleAsset(document);
    REQUIRE(encoded.HasValue());
    const auto decoded = DeserializeUiStyleAsset(encoded.Value());
    REQUIRE(decoded.HasValue());
    CHECK(Assignments(decoded.Value(), container) == *assignments);
    auto duplicate = assignments->front();
    duplicate.sealed = true;
    assignments->push_back(duplicate);
    CHECK(SerializeUiStyleAsset(document).HasError());
}

TEST_CASE("Style source rejects unsupported unknown duplicate oversized and malformed declarations", "[runtime_ui][style][asset]") {
    const auto bytes = SerializeUiStyleAsset(Document()).Value();
    const auto source = nlohmann::json::parse(bytes);
    auto bad = source;
    bad["schemaVersion"] = 2;
    CHECK(DeserializeUiStyleAsset(bad.dump()).HasError());
    bad = source;
    bad["properties"] = nlohmann::json::array();
    CHECK(DeserializeUiStyleAsset(bad.dump()).HasError());
    bad = source;
    bad["style"]["assignments"].push_back(bad["style"]["assignments"][0]);
    CHECK(DeserializeUiStyleAsset(bad.dump()).HasError());
    bad = source;
    bad["style"]["states"] = {{{"required", 65535}, {"forbidden", 0}, {"layer", 0}, {"assignments", nlohmann::json::array()}}};
    CHECK(DeserializeUiStyleAsset(bad.dump()).HasError());
    CHECK(DeserializeUiStyleAsset(std::string(MaximumUiStyleAssetBytes + 1, ' ')).HasError());
    auto duplicate = bytes;
    duplicate.insert(duplicate.find('{') + 1, "\"schemaVersion\":1,");
    CHECK(DeserializeUiStyleAsset(duplicate).HasError());
}

TEST_CASE("One selected root policy fingerprints real resource defaults and refuses competing namespace defaults",
          "[runtime_ui][style][asset]") {
    const auto family = Asset("02f55810-e86a-434a-9d31-b51c91967eb4");
    const auto type = Assets::AssetTypeId::Parse("game.font_family").Value();
    const std::string bytes = "actual font dependency envelope";
    const std::array evidence{UiStyleResourceEvidence{{family, type, true}, ComputeSha256(std::as_bytes(std::span{bytes}))}};
    UiCoreStyleDefaults defaults{.typography = UiStyleTypography{.family = family}};
    CHECK(CreateCoreUiStyleSchema(defaults).HasError());
    auto schema = CreateCoreUiStyleSchema(defaults, evidence);
    REQUIRE(schema.HasValue());
    auto document = Document();
    document.defaults = defaults;
    document.dependencies = {{family, type, true}};
    const std::array documents{document};
    CHECK(PrepareUiStyleRegistry(documents, schema.Value(), RuntimeStyleGeneration::Create(1).Value()).HasValue());
    auto changed = evidence;
    changed[0].digest.bytes[31] ^= 1;
    CHECK(CreateCoreUiStyleSchema(defaults, changed).Value().fingerprint != schema.Value().fingerprint);
    document.defaults.typography->weight = 700;
    const std::array conflict{document};
    CHECK(PrepareUiStyleRegistry(conflict, schema.Value(), RuntimeStyleGeneration::Create(1).Value()).HasError());
}

TEST_CASE("Real core property schema validates closed enum ranges at the canonical runtime admission boundary",
          "[runtime_ui][style][asset]") {
    auto document = Document();
    const auto schema = CreateCoreUiStyleSchema({}).Value();
    document.style.assignments.push_back(
        {CoreUiStylePropertyId(UiCoreStyleProperty::ImageFit), UiStyleValueSource::Literal(UiStyleEnumValue{99}), false});
    const std::array invalid{document};
    CHECK(PrepareUiStyleRegistry(invalid, schema, RuntimeStyleGeneration::Create(1).Value()).HasError());
    document.style.assignments.back().value =
        UiStyleValueSource::Literal(UiStyleEnumValue{static_cast<std::uint32_t>(UiStyleImageFit::Cover)});
    const std::array valid{document};
    REQUIRE(PrepareUiStyleRegistry(valid, schema, RuntimeStyleGeneration::Create(1).Value()).HasValue());
    document.style.assignments.back().property = CoreUiStylePropertyId(UiCoreStyleProperty::Image);
    const std::array missingCapability{document};
    CHECK(PrepareUiStyleRegistry(missingCapability, schema, RuntimeStyleGeneration::Create(1).Value()).HasError());
}

TEST_CASE("Style source and cook share every existing literal alternative and real declared resource identity",
          "[runtime_ui][style][asset]") {
    auto document = Document();
    auto bytes = Asset().Bytes();
    bytes.back() ^= 0x10;
    const auto image = Assets::AssetId::FromBytes(bytes);
    bytes.back() ^= 0x20;
    const auto family = Assets::AssetId::FromBytes(bytes);
    const auto imageType = Assets::AssetTypeId::Parse("game.image").Value(),
               fontType = Assets::AssetTypeId::Parse("game.font_family").Value();
    const std::string imageBytes = "image envelope", fontBytes = "font envelope";
    const std::array evidence{UiStyleResourceEvidence{{image, imageType, true}, ComputeSha256(std::as_bytes(std::span{imageBytes}))},
                              UiStyleResourceEvidence{{family, fontType, true}, ComputeSha256(std::as_bytes(std::span{fontBytes}))}};
    document.defaults = {.typography = UiStyleTypography{.family = family}, .image = UiStyleImage{.asset = image}};
    document.dependencies = {{image, imageType, true}, {family, fontType, true}};
    document.style.assignments =
        {{CoreUiStylePropertyId(UiCoreStyleProperty::Width), UiStyleValueSource::Literal(UiStyleDimension{640}), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::Fill), UiStyleValueSource::Literal(UiStyleColor{0.2F, 0.3F, 0.4F, 1}), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::Typography), UiStyleValueSource::Literal(*document.defaults.typography), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::Image), UiStyleValueSource::Literal(*document.defaults.image), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::Shape), UiStyleValueSource::Literal(UiStyleShape{.radius = 64}), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::Opacity), UiStyleValueSource::Literal(UiStyleScalar{0.75F}), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::ImageFit), UiStyleValueSource::Literal(UiStyleEnumValue{2}), false},
         {CoreUiStylePropertyId(UiCoreStyleProperty::Motion), UiStyleValueSource::Literal(UiStyleMotionReference{1000, 1}), false}};
    const auto schema = CreateCoreUiStyleSchema(document.defaults, evidence).Value();
    const std::array documents{document};
    REQUIRE(PrepareUiStyleRegistry(documents, schema, RuntimeStyleGeneration::Create(1).Value()).HasValue());
    const auto source = SerializeUiStyleAsset(document).Value();
    const auto cooked = CookUiStyleAsset(document, schema).Value();
    const auto decoded = DecodeCookedUiStyleAsset(cooked, Asset(), schema);
    REQUIRE(decoded.HasValue());
    CHECK(SerializeUiStyleAsset(decoded.Value()).Value() == source);
    CHECK(decoded.Value().style.assignments.size() == 8);
    auto bad = cooked;
    bad[8] = 2;
    CHECK(DecodeCookedUiStyleAsset(bad, Asset(), schema).HasError());
    bad = cooked;
    bad.push_back(' ');
    CHECK(DecodeCookedUiStyleAsset(bad, Asset(), schema).HasError());
    const std::span sourceBytes{reinterpret_cast<const std::uint8_t *>(source.data()), source.size()};
    CHECK(DecodeCookedUiStyleAsset(sourceBytes, Asset(), schema).HasError());
}

TEST_CASE("Namespace cycles and undeclared qualified token dependencies fail before real registry publication",
          "[runtime_ui][style][asset]") {
    auto document = Document();
    const auto schema = CreateCoreUiStyleSchema({}).Value();
    document.style.base = document.style.id;
    document.dependencies = {{Asset(), Assets::AssetTypeId::Parse(UiStyleAssetTypeName).Value(), true}};
    const std::array cycle{document};
    CHECK(PrepareUiStyleRegistry(cycle, schema, RuntimeStyleGeneration::Create(1).Value()).HasError());
    document = Document();
    auto bytes = Asset().Bytes();
    bytes.back() ^= 0x20;
    document.style.assignments[0].value =
        UiStyleValueSource::Token({RuntimeStyleAssetId::Create(bytes).Value(), UiStyleTokenId::Create(bytes).Value()});
    const std::array undeclared{document};
    CHECK(PrepareUiStyleRegistry(undeclared, schema, RuntimeStyleGeneration::Create(1).Value()).HasError());
}
