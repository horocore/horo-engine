#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Application;
    using namespace Application::PrefabCookTestSupport;

    /** @brief Keeps actual native cook preparation errors visible before testing publication behavior. */
    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual canonical operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Retains canonical sources/revisions and typed delta intent used by the actual project cook host. */
    struct LayerSource final {
        Assets::AssetId base;
        Prefab::PrefabDocument document;
        std::string encoded;
    };

    /** @brief Creates real persisted base/variant files; transform-only edits require no invented project component policy. */
    LayerSource Prepare(HostFixture &fixture) {
        const auto limits = Value(Prefab::PrefabLimitProfile::Create({}));
        const auto base = fixture.AddSpawnableRoot();
        auto document = Value(
            Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                            .assetId = base,
                                            .objects = {{.localId = {0}, .name = "Root"},
                                                        {.localId = {3}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "Child"}}},
                                           limits));
        const auto canonical = Value(document.SerializeCanonical());
        fixture.WriteText("assets/second.prefab", canonical);
        const Prefab::PrefabSourceRevision revision{fixture.version, ComputeSha256(std::as_bytes(std::span{canonical}))};
        auto sources = Value(Prefab::BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {{document, revision}}, limits));
        auto source = Value(sources.Resolve(base, Prefab::PrefabInstanceId::Create(7).Value(), limits));
        const auto root = Value(Prefab::PrefabObjectAddress::Create({}, {0}));
        const auto child = Value(Prefab::PrefabObjectAddress::Create({}, {3}));
        auto first =
            Value(Prefab::PreparePrefabValueEdit(source, {}, {Prefab::PrefabValueEditKind::Assign, root, Math::Transform{{1, 0, 0}}},
                                                 nullptr, limits));
        auto second = Value(Prefab::PreparePrefabValueEdit(source, first.rebased,
                                                           {Prefab::PrefabValueEditKind::Assign, child, Math::Transform{{0, 2, 0}}},
                                                           nullptr, limits));
        auto variant =
            Value(Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                  .assetId = fixture.prefabId,
                                                  .composition = Prefab::PrefabComposition{.variantParent = Value(
                                                                                               Prefab::PrefabAssetReference::Create(base)),
                                                                                           .variantAuthoredAgainst = revision,
                                                                                           .variantOverrides = second.rebased},
                                                  .referencedAssets = {base}},
                                                 limits));
        auto encoded = Value(variant.SerializeCanonical());
        fixture.WriteText("assets/hierarchy.prefab", encoded);
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
        return {base, std::move(document), std::move(encoded)};
    }

    /** @brief Reads a committed asset envelope from the actual selected/pinned generation. */
    Assets::AssetCookArtifact Artifact(const Assets::AssetCookGeneration &generation, const Assets::AssetId id) {
        const auto contents = Value(Assets::ReadCookGenerationContents(NativeDurableFileSystem{}, generation, 1024U * 1024U));
        for (const auto &bytes : contents.artifacts) {
            auto artifact = Value(Assets::DecodeCookedArtifact(bytes));
            if (artifact.id == id)
                return artifact;
        }
        FAIL("Expected asset was absent from the published cook generation.");
        return {};
    }

    /** @brief Uses a configurable inert component contract with no provider callbacks. */
    Gameplay::ComponentDescriptor LayerDescriptor(const bool optionalProperty = false) {
        Gameplay::ComponentDescriptor descriptor{.typeId = Value(Gameplay::ComponentTypeId::Parse("game.layer.data")),
                                                 .schemaVersion = 1,
                                                 .displayName = "Layer data",
                                                 .category = "Tests"};
        if (optionalProperty)
            descriptor.properties.push_back({Value(Gameplay::ComponentPropertyId::Parse("optional")), "Optional",
                                             Gameplay::ComponentPropertyKind::String, false});
        return descriptor;
    }

    /** @brief Captures the authoritative project component registry independently of authoring topology permission. */
    std::shared_ptr<const PrefabCookSchemaContext> ProjectSchemas(const bool optionalProperty) {
        Gameplay::ComponentRegistry registry;
        REQUIRE(registry.Register(LayerDescriptor(optionalProperty)).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        return Value(PrefabCookSchemaContext::Capture(registry, {}));
    }

    /** @brief Persists a plain base plus a selected derived template that introduces a component through typed topology. */
    Assets::AssetId PrepareTopologyVariant(HostFixture &fixture, const Prefab::RawComponentPayload &payload) {
        const auto limits = Value(Prefab::PrefabLimitProfile::Create({}));
        const auto base = fixture.AddSpawnableRoot();
        const auto document = Value(Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                                    .assetId = base,
                                                                    .objects = {{.localId = {0}, .name = "Base"}}},
                                                                   limits));
        const auto source = Value(document.SerializeCanonical());
        fixture.WriteText("assets/second.prefab", source);
        const Prefab::PrefabSourceRevision revision{fixture.version, ComputeSha256(std::as_bytes(std::span{source}))};
        const auto resolver = Value(Prefab::BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(), {{document, revision}}, limits));
        const auto original = Value(resolver.Resolve(base, Value(Prefab::PrefabInstanceId::Create(7)), limits));
        auto schemas = Value(Prefab::PrefabValueSchemaSnapshot::Create({}, limits, {{.component = LayerDescriptor(), .allowAddition = true}}));
        const auto root = Value(Prefab::PrefabObjectAddress::Create({}, {0}));
        const auto edited = Value(Prefab::PreparePrefabComponentEdit(
            original, {}, Prefab::PrefabAddComponentOverride{root, payload}, &schemas, limits));
        REQUIRE(edited.effective.has_value());
        const auto variant = Value(Prefab::PrefabDocument::Create(
            {.projectVersion = fixture.version,
             .assetId = fixture.prefabId,
             .composition = Prefab::PrefabComposition{.variantParent = Value(Prefab::PrefabAssetReference::Create(base)),
                                                      .variantAuthoredAgainst = revision,
                                                      .variantOverrides = edited.rebased},
             .referencedAssets = {base}},
            limits));
        fixture.WriteText("assets/hierarchy.prefab", Value(variant.SerializeCanonical()));
        fixture.request.runtimePrefabRoots = {fixture.prefabId, base};
        fixture.request.overrideSchemas = std::make_shared<const Prefab::PrefabValueSchemaSnapshot>(std::move(schemas));
        return base;
    }
}  // namespace

TEST_CASE("Actual scene and template cook normalizes layer record order but preserves strict lexical and revision admission",
          "[native][prefab-cook][host][layer]") {
    HostFixture fixture;
    const auto layer = Prepare(fixture);
    INFO("Canonical layered source cook");
    const auto first = Value(fixture.Cook());
    const auto expected = Artifact(first.generation, fixture.prefabId).payload;
    auto wire = nlohmann::ordered_json::parse(layer.encoded);
    std::ranges::reverse(wire["composition"]["variantOverrides"]["records"]);
    const auto permutation = wire.dump(2) + "\n";
    REQUIRE(ComputeSha256(std::as_bytes(std::span{permutation})) != ComputeSha256(std::as_bytes(std::span{layer.encoded})));
    const auto limits = Value(Prefab::PrefabLimitProfile::Create({}));
    REQUIRE(Value(Value(Prefab::PrefabDocument::Parse(permutation, limits)).MatchesCanonicalInput(permutation)));
    fixture.WriteText("assets/hierarchy.prefab", permutation);
    INFO("Same semantic source under distinct actual raw record-array bytes");
    const auto reordered = Value(fixture.Cook());
    CHECK(Artifact(reordered.generation, fixture.prefabId).payload == expected);
    SECTION("noncanonical scalar/source spelling") {
        fixture.WriteText("assets/hierarchy.prefab", wire.dump(2) + "\n ");
    }
    SECTION("unknown record fields") {
        wire["composition"]["variantOverrides"]["records"][0]["extra"] = true;
        fixture.WriteText("assets/hierarchy.prefab", wire.dump(2) + "\n");
    }
    SECTION("source revision changed without accepted rebase") {
        auto changed = layer.document.Data();
        changed.objects[0].localTransform.translation.x = 9;
        const auto document = Value(Prefab::PrefabDocument::Create(std::move(changed), Value(Prefab::PrefabLimitProfile::Create({}))));
        fixture.WriteText("assets/second.prefab", Value(document.SerializeCanonical()));
    }
    const auto rejected = fixture.Cook();
    REQUIRE(rejected.HasError());
    fixture.AssertRetained(reordered.generation);
}

TEST_CASE("Layer-introduced gameplay schema invalidates its template while unrelated cook inputs retain hits",
          "[native][prefab-cook][host][layer][closure-key]") {
    HostFixture fixture;
    const Prefab::RawComponentPayload payload{
        .instance = Value(Prefab::PrefabComponentInstanceId::Create(1)),
        .component = {.typeId = LayerDescriptor().typeId, .schemaVersion = 1, .payload = {std::byte{'{'}, std::byte{'}'}}}};
    const auto base = PrepareTopologyVariant(fixture, payload);
    const std::vector<SceneSource::ScenePrefabInstance> placements{{.instanceId = Value(Prefab::PrefabInstanceId::Create(7)),
                                                                   .sourcePrefab = Value(Prefab::PrefabAssetReference::Create(base))}};
    const std::vector<SceneSource::SceneObjectSnapshot> objects{
        {.id = {900}, .name = "Scene-owned data", .components = {.gameplayComponents = {payload.component}}}};
    fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, placements}));
    fixture.request.schemas = ProjectSchemas(false);
    const auto first = Value(fixture.Cook());
    const auto templateArtifact = Artifact(first.generation, fixture.prefabId);
    const auto cooked = Value(Prefab::CookedPrefab::Parse(std::as_bytes(std::span{templateArtifact.payload}), fixture.prefabId,
                                                         Value(Prefab::PrefabLimitProfile::Create({}))));
    REQUIRE(cooked.Data().entities.size() == 1);
    REQUIRE(cooked.Data().entities.front().members.size() == 1);
    CHECK(std::get<Prefab::RawComponentPayload>(cooked.Data().entities.front().members.front()) == payload);

    fixture.request.schemas = ProjectSchemas(true);
    const auto changed = Value(fixture.Cook());
    CHECK(changed.cacheHits == 2);
    for (const auto id : {fixture.prefabId, fixture.sceneId})
        CHECK(Artifact(changed.generation, id).cacheKeyDigest != Artifact(first.generation, id).cacheKeyDigest);
    for (const auto id : {base, TestMeshRecord().id})
        CHECK(Artifact(changed.generation, id).cacheKeyDigest == Artifact(first.generation, id).cacheKeyDigest);

    // The selected template now has the only use of the missing project descriptor.
    fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({{}, placements}));
    fixture.request.schemas.reset();
    BuildOutputStore output{8};
    fixture.request.assets.buildOutputStore = &output;
    REQUIRE(fixture.Cook().HasError());
    const auto diagnostics = output.SnapshotIfChanged(0);
    REQUIRE(diagnostics);
    REQUIRE(diagnostics->records.size() == 1);
    CHECK(diagnostics->records.front().stage == "prefab.validate_sources");
    fixture.AssertRetained(changed.generation);
}

TEST_CASE("An unused override schema leaves transform-only scenes templates and resources cached",
          "[native][prefab-cook][host][layer][closure-key]") {
    HostFixture fixture;
    const auto layer = Prepare(fixture);
    fixture.request.runtimePrefabRoots.push_back(layer.base);
    const auto first = Value(fixture.Cook());
    auto schemas = Value(Prefab::PrefabValueSchemaSnapshot::Create(
        {}, Value(Prefab::PrefabLimitProfile::Create({})), {{.component = LayerDescriptor(), .allowAddition = true}}));
    fixture.request.overrideSchemas = std::make_shared<const Prefab::PrefabValueSchemaSnapshot>(std::move(schemas));
    const auto second = Value(fixture.Cook());
    CHECK(second.totalAssets == 4);
    CHECK(second.cacheHits == second.totalAssets);
    CHECK(second.generation.manifestDigest == first.generation.manifestDigest);
}
