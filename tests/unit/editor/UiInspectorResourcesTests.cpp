#include "Horo/Editor/UiInspectorResources.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Id(const std::uint8_t value) {
        SerializedUiId bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    std::shared_ptr<RuntimeStyleRegistry> Registry(const bool sealed = false, const std::optional<Assets::AssetId> image = std::nullopt) {
        UiStyleRegistryDefinition definition;
        definition.properties.push_back({.id = Id<UiStylePropertyId>(1),
                                         .category = image ? UiStyleValueCategory::Imagery : UiStyleValueCategory::Scalar,
                                         .defaultValue = image ? UiStyleValue{UiStyleImage{*image}} : UiStyleValue{UiStyleScalar{0.5F}},
                                         .minimumScalar = 0,
                                         .maximumScalar = 1});
        UiStyleAssetDefinition asset{.id = Id<RuntimeStyleAssetId>(1)};
        if (sealed)
            asset.assignments.push_back({Id<UiStylePropertyId>(1), UiStyleValueSource::Literal(UiStyleScalar{0.5F}), true});
        asset.tokens.push_back({.id = Id<UiStyleTokenId>(1),
                                .category = UiStyleValueCategory::Scalar,
                                .value = UiStyleValueSource::Literal(UiStyleScalar{0.25F})});
        definition.assets.push_back(std::move(asset));
        auto created = RuntimeStyleRegistry::Create(std::move(definition), RuntimeStyleGeneration::Create(1).Value());
        REQUIRE(created.HasValue());
        return std::make_shared<RuntimeStyleRegistry>(std::move(created).Value());
    }

    UiDocument Document(const std::optional<UiAssetDependency> dependency = std::nullopt) {
        UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
        REQUIRE(
            builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .referenceResolution = {800, 600}}).HasValue());
        REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value()).HasValue());
        if (dependency)
            REQUIRE(builder.RequireAsset(*dependency).HasValue());
        return std::move(builder).Build().Value();
    }

    UiElementPresentation Scalar(const float value = 0.75F) {
        UiElementPresentation result;
        result.inlineStyle.push_back({Id<UiStylePropertyId>(1), UiStyleValueSource::Literal(UiStyleScalar{value}), false});
        return result;
    }

    Assets::AssetRecord ImageRecord() {
        return {Assets::AssetId::FromBytes(Id<UiDocumentId>(7).Bytes()), Assets::AssetTypeId::Parse("game.ui.image").Value(),
                ProjectPath::Parse("Assets/image.horoasset").Value(), ProjectPath::Parse("Assets/image.horoasset.horo").Value()};
    }
}  // namespace

TEST_CASE("Inspector resource validation uses actual registered categories tokens classes and sealed cascade",
          "[editor][ui][inspector][resources]") {
    Assets::AssetRegistry assets;
    UiInspectorResources host{assets};
    const auto document = Document();
    CHECK(host.Capture().Validate(document, Scalar()).HasError());
    auto registry = Registry();
    REQUIRE(host.PublishStyles(registry, Id<RuntimeStyleAssetId>(1)).HasValue());
    auto snapshot = host.Capture();
    CHECK(snapshot.Validate(document, Scalar()).HasValue());
    CHECK(snapshot.Validate(document, Scalar(2)).HasError());
    auto wrong = Scalar();
    wrong.inlineStyle[0].value = UiStyleValueSource::Literal(UiStyleDimension{64});
    CHECK(snapshot.Validate(document, wrong).HasError());
    auto token = Scalar();
    token.inlineStyle[0].value = UiStyleValueSource::Token({Id<RuntimeStyleAssetId>(1), Id<UiStyleTokenId>(1)});
    CHECK(snapshot.Validate(document, token).HasValue());
    token.inlineStyle[0].value.token.id = Id<UiStyleTokenId>(99);
    CHECK(snapshot.Validate(document, token).HasError());
    wrong = Scalar();
    wrong.classes.push_back({Id<RuntimeStyleAssetId>(1), Id<UiStyleClassId>(99)});
    CHECK(snapshot.Validate(document, wrong).HasError());
    REQUIRE(host.PublishStyles(Registry(true), Id<RuntimeStyleAssetId>(1)).HasValue());
    CHECK(host.Capture().Validate(document, Scalar()).HasError());
    REQUIRE(registry->BeginRetirement().HasValue());
    CHECK(snapshot.Validate(document, Scalar()).HasError());
}

TEST_CASE("Inspector validates resources against real immutable project registry snapshots and typed dependencies",
          "[editor][ui][inspector][resources]") {
    Assets::AssetRegistry assets;
    auto record = ImageRecord();
    REQUIRE(assets.Publish(std::vector{record}).status == Assets::AssetRegistryBuildStatus::Complete);
    UiInspectorResources host{assets};
    REQUIRE(host.PublishStyles(Registry(false, record.id), Id<RuntimeStyleAssetId>(1)).HasValue());
    const auto document = Document(UiAssetDependency{record.id, record.type, true});
    auto presentation = Scalar();
    presentation.inlineStyle[0].value = UiStyleValueSource::Literal(UiStyleImage{record.id});
    auto pinned = host.Capture();
    CHECK(pinned.Validate(document, presentation).HasValue());
    CHECK(pinned.Validate(Document(), presentation).HasError());
    record.type = Assets::AssetTypeId::Parse("game.ui.wrong").Value();
    REQUIRE(assets.Publish(std::vector{record}).status == Assets::AssetRegistryBuildStatus::Complete);
    CHECK(host.Capture().Validate(document, presentation).HasError());
    CHECK(pinned.Validate(document, presentation).HasValue());
    REQUIRE(assets.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
    CHECK(host.Capture().Validate(document, presentation).HasError());
    CHECK(pinned.Validate(document, presentation).HasValue());
}

TEST_CASE("Resource publication fences style removal reinstalls and owner retirement without synthetic registries",
          "[editor][ui][inspector][resources]") {
    Assets::AssetRegistry assets;
    UiInspectorResources host{assets};
    auto registry = Registry();
    CHECK(host.PublishStyles(registry).HasError());
    REQUIRE(host.PublishStyles(registry, Id<RuntimeStyleAssetId>(1)).HasValue());
    const auto before = host.Capture();
    REQUIRE(host.PublishStyles(nullptr).HasValue());
    CHECK_FALSE(host.Capture().StyleGeneration().IsValid());
    REQUIRE(host.PublishStyles(registry, Id<RuntimeStyleAssetId>(1)).HasValue());
    const auto after = host.Capture();
    CHECK(after.StyleGeneration() == before.StyleGeneration());
    CHECK(after.publication > before.publication);
    registry->Shutdown();
    CHECK_FALSE(host.Capture().StyleGeneration().IsValid());
    CHECK(host.PublishStyles(registry, Id<RuntimeStyleAssetId>(1)).HasError());
}
