#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "NativeCacheFiles.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Assets;
    using namespace Horo::Assets::ServiceTestSupport;
    using namespace Horo::Application::PrefabCookTestSupport;

    /** @brief Exercises complete, unavailable and cyclic source dependency declarations through the real host. */
    class DependencyCooker final : public ICookerStrategy {
    public:
        std::optional<std::vector<AssetId>> dependencies;
        std::shared_ptr<std::atomic<unsigned>> calls;
        std::optional<AssetId> dependencySource;
        bool throwDeclaration{};

        DependencyCooker(std::optional<std::vector<AssetId>> values, std::shared_ptr<std::atomic<unsigned>> counter)
            : dependencies(std::move(values)), calls(std::move(counter)) {}

        Result<std::optional<std::vector<AssetId>>> InputDependencies(const CookSourceView &source,
                                                                      const CancellationToken &) const override {
            if (throwDeclaration)
                throw std::runtime_error("test dependency provider");
            if (dependencySource && source.id != *dependencySource)
                return Result<std::optional<std::vector<AssetId>>>::Success(std::vector<AssetId>{});
            return Result<std::optional<std::vector<AssetId>>>::Success(dependencies);
        }

        Result<CookOutputSink> Cook(const CookSourceView &, const CancellationToken &) const override {
            ++*calls;
            return Result<CookOutputSink>::Success({{1}, {}, {}});
        }
    };

    /** @brief Reads actual published envelope bytes without treating a cache directory as authority. */
    Result<AssetCookArtifact> GenerationArtifact(const AssetCookGeneration &generation, const AssetId id) {
        auto inventory = ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, generation, 1024U * 1024U);
        if (inventory.HasError())
            return Result<AssetCookArtifact>::Failure(inventory.ErrorValue());
        for (const auto &bytes : inventory.Value().artifacts) {
            auto artifact = DecodeCookedArtifact(bytes);
            if (artifact.HasError())
                return artifact;
            if (artifact.Value().id == id)
                return artifact;
        }
        return Result<AssetCookArtifact>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
    }

    /** @brief Produces another structurally valid template while retaining the original requested source/cache identity. */
    void ReplaceTemplatePayload(AssetCookArtifact &artifact) {
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        auto decoded = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.payload}), artifact.id, limits);
        REQUIRE(decoded.HasValue());
        auto data = decoded.Value().Data();
        data.entities[1].localTransform.translation.y = 99;
        auto replacement = Prefab::CookedPrefab::Create(std::move(data), limits);
        REQUIRE(replacement.HasValue());
        artifact.payload.clear();
        for (const auto byte : replacement.Value().Bytes())
            artifact.payload.push_back(std::to_integer<std::uint8_t>(byte));
    }

    /** @brief Prepares the host-produced template through the existing generation and asynchronous provider path. */
    void AssertProviderTemplate(HostFixture &fixture, const AssetCookGeneration &generation, const Sha256Digest &digest) {
        FilesystemAssetProvider bytes{generation.generationRoot};
        AssetLoadService loads{fixture.jobs, bytes};
        Runtime::RuntimeSceneService scenes{fixture.registry, loads};
        REQUIRE(scenes.Startup({}).HasValue());
        Runtime::SceneDefinitionBuilder builder{{7}, {1}};
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        REQUIRE(scenes.QueuePreparation(std::move(definition).Value()).HasValue());
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {1, {}, 0.0, 0, {}, false, {}}).HasValue());
        Prefab::PrefabTemplateProvider provider{fixture.registry, loads, scenes, Prefab::PrefabLimitProfile::Create({}).Value()};
        REQUIRE(provider.Startup({}).HasValue());
        auto loading = provider.LoadAsync({fixture.prefabId, digest, fixture.request.assets.target});
        REQUIRE(loading.HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (loading.Value().State() == Prefab::PrefabTemplateLoadState::Loading && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(provider.Advance().HasValue());
            std::this_thread::yield();
        }
        auto lease = provider.TakeResult(loading.Value());
        REQUIRE(lease.HasValue());
        CHECK(lease.Value().Template()->GetObjectCount() == 2);
        CHECK(lease.Value().Dependencies().size() == 1);
        provider.Shutdown();
        CHECK(lease.Value().Template()->Data().entities[1].localTransform.translation.y == 4);
    }
}  // namespace

TEST_CASE("Host publishes dynamic templates resources and expanded scenes once and loads the generation provider",
          "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    auto files = std::make_shared<HostPublicationFiles>();
    fixture.request.assets.publicationFiles = files;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CHECK(first.Value().generation.artifactCount == 3);
    CHECK(files->observed);
    CHECK(files->selectorWrites == 1);
    const auto artifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    auto cooked = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.Value().payload}), fixture.prefabId,
                                              Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(cooked.HasValue());
    REQUIRE(cooked.Value().Data().dependencies.size() == 1);
    const auto inventory = ReadCookGenerationContents(Horo::NativeDurableFileSystem{}, first.Value().generation, 1024U * 1024U);
    REQUIRE(inventory.HasValue());
    const auto resource = std::ranges::find(inventory.Value().entries, TestMeshRecord().id, &AssetCookManifestEntry::assetId);
    REQUIRE(resource != inventory.Value().entries.end());
    CHECK(cooked.Value().Data().dependencies[0].artifactDigest == resource->artifactHash);
    const auto root = std::ranges::find(inventory.Value().entries, fixture.prefabId, &AssetCookManifestEntry::assetId);
    REQUIRE(root != inventory.Value().entries.end());
    AssertProviderTemplate(fixture, first.Value().generation, root->artifactHash);
    const auto repeated = fixture.Cook();
    REQUIRE(repeated.HasValue());
    CHECK(repeated.Value().cacheHits == 3);
    CHECK(repeated.Value().generation.manifestDigest == first.Value().generation.manifestDigest);
}

TEST_CASE("Dynamic root selection is canonical and excludes unselected templates under the same registry revision",
          "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    const auto other = fixture.AddSpawnableRoot();
    const auto revision = fixture.registry.Snapshot().Revision();
    fixture.request.runtimePrefabRoots = {other, fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    CHECK(first.Value().generation.artifactCount == 4);
    std::ranges::reverse(fixture.request.runtimePrefabRoots);
    const auto reordered = fixture.Cook();
    REQUIRE(reordered.HasValue());
    CHECK(reordered.Value().cacheHits == 4);
    CHECK(reordered.Value().generation.manifestDigest == first.Value().generation.manifestDigest);
    fixture.request.runtimePrefabRoots = {other};
    const auto selected = fixture.Cook();
    REQUIRE(selected.HasValue());
    CHECK(selected.Value().generation.artifactCount == 3);
    CHECK(GenerationArtifact(selected.Value().generation, fixture.prefabId).HasError());
    CHECK(GenerationArtifact(selected.Value().generation, other).HasValue());
    CHECK(fixture.registry.Snapshot().Revision() == revision);
}

TEST_CASE("Invalid dynamic root selection preserves the current complete generation", "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    SECTION("duplicate") {
        fixture.request.runtimePrefabRoots.push_back(fixture.prefabId);
    }
    SECTION("missing") {
        fixture.request.runtimePrefabRoots = {Id("00000000-0000-0000-0000-000000000099")};
    }
    SECTION("wrong type") {
        fixture.request.runtimePrefabRoots = {TestMeshRecord().id};
    }
    SECTION("too many roots") {
        fixture.request.runtimePrefabRoots.assign(fixture.request.assets.limits.maximumAssets + 1, fixture.prefabId);
    }
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Host flattens nested and variant source closure without publishing source-only templates",
          "[native][prefab-cook][host][template][composition]") {
    HostFixture fixture;
    const auto base = fixture.AddSpawnableRoot();
    const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
    const auto baseDocument = Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                              .assetId = base,
                                                              .objects = {{.localId = {0}, .name = "Second root"}}},
                                                             limits);
    REQUIRE(baseDocument.HasValue());
    const auto canonical = baseDocument.Value().SerializeCanonical().Value();
    const Prefab::PrefabSourceRevision revision{fixture.version, ComputeSha256(std::as_bytes(std::span{canonical}))};
    Prefab::PrefabDocumentData root{.projectVersion = fixture.version,
                                    .assetId = fixture.prefabId,
                                    .objects = {{.localId = {0}, .name = "Dynamic root"}},
                                    .referencedAssets = {base}};
    std::size_t count{};
    SECTION("nested") {
        root.composition =
            Prefab::PrefabComposition{.nestedPlacements = {{.placementLocalId = {7},
                                                            .sourcePrefab = Prefab::PrefabAssetReference::Create(base).Value(),
                                                            .authoredAgainst = revision}}};
        count = 2;
    }
    SECTION("variant") {
        root.objects.clear();
        root.composition = Prefab::PrefabComposition{.variantParent = Prefab::PrefabAssetReference::Create(base).Value(),
                                                     .variantAuthoredAgainst = revision};
        count = 1;
    }
    const auto document = Prefab::PrefabDocument::Create(std::move(root), limits);
    REQUIRE(document.HasValue());
    fixture.WriteText("assets/hierarchy.prefab", document.Value().SerializeCanonical().Value());
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto cooked = fixture.Cook();
    REQUIRE(cooked.HasValue());
    CHECK(cooked.Value().generation.artifactCount == 3);
    CHECK(GenerationArtifact(cooked.Value().generation, base).HasError());
    const auto artifact = GenerationArtifact(cooked.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    const auto prefab = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.Value().payload}), fixture.prefabId, limits);
    REQUIRE(prefab.HasValue());
    CHECK(prefab.Value().GetObjectCount() == count);
    CHECK(prefab.Value().Data().entities.back().provenance.sourceAsset == base);
    CHECK(prefab.Value().Data().dependencies.empty());
}

TEST_CASE("Dynamic template cache binds actual changed resource envelopes without a registry replacement",
          "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto revision = fixture.registry.Snapshot().Revision();
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto oldArtifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(oldArtifact.HasValue());
    WriteFile(fixture.project.sourceFile, std::array<std::uint8_t, 3>{7, 8, 9});
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 0);
    const auto newArtifact = GenerationArtifact(changed.Value().generation, fixture.prefabId);
    REQUIRE(newArtifact.HasValue());
    CHECK(oldArtifact.Value().payloadDigest != newArtifact.Value().payloadDigest);
    CHECK(oldArtifact.Value().sourceDigest == newArtifact.Value().sourceDigest);
    CHECK(oldArtifact.Value().cacheKeyDigest != newArtifact.Value().cacheKeyDigest);
    CHECK(fixture.registry.Snapshot().Revision() == revision);
}

TEST_CASE("Dynamic template keys bind changed resource cooker settings with identical source and registry inputs",
          "[native][prefab-cook][host][template][cache]") {
    HostFixture fixture;
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto revision = fixture.registry.Snapshot().Revision();
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto oldArtifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(oldArtifact.HasValue());
    const auto changed = fixture.MakeHost(2).Cook(fixture.request);
    REQUIRE(changed.HasValue());
    const auto newArtifact = GenerationArtifact(changed.Value().generation, fixture.prefabId);
    REQUIRE(newArtifact.HasValue());
    CHECK(oldArtifact.Value().sourceDigest == newArtifact.Value().sourceDigest);
    CHECK(oldArtifact.Value().cacheKeyDigest != newArtifact.Value().cacheKeyDigest);
    CHECK(oldArtifact.Value().payloadDigest != newArtifact.Value().payloadDigest);
    CHECK(fixture.registry.Snapshot().Revision() == revision);
}

TEST_CASE("Dynamic candidate failures retain the prior generation at the common selector fence", "[native][prefab-cook][host][template]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    fixture.WritePrefab(8);
    auto files = std::make_shared<HostPublicationFiles>();
    CancellationSource cancelled;
    SECTION("cancelled after candidate staging") {
        files->staged = [&cancelled] {
            cancelled.RequestCancellation();
        };
    }
    SECTION("source drift after staging") {
        files->staged = [&fixture] {
            fixture.WritePrefab(9);
        };
    }
    SECTION("selector replacement failure") {
        files->failReplacement = true;
    }
    fixture.request.assets.publicationFiles = files;
    REQUIRE(fixture.Cook(cancelled.Token()).HasError());
    CHECK(files->observed);
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Dynamic cache admission rejects corrupt and valid but different HPFB payloads under the requested key",
          "[native][prefab-cook][host][template][cache]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    auto original = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(original.HasValue());
    auto poisoned = std::move(original).Value();
    SECTION("HPFB integrity differs despite a valid outer envelope") {
        std::as_writable_bytes(std::span{poisoned.payload}).back() ^= std::byte{1};
    }
    SECTION("another valid template is not the expected output") {
        ReplaceTemplatePayload(poisoned);
    }
    poisoned.payloadDigest = ComputeSha256(std::as_bytes(std::span{poisoned.payload}));
    const auto envelope = EncodeCookedArtifact(poisoned);
    REQUIRE(envelope.HasValue());
    TempDir poisonedCache;
    const auto poisonedFiles = Horo::TestSupport::NativeCacheFiles(poisonedCache.path);
    AssetCookCache cache{*poisonedFiles};
    REQUIRE(cache.Store({poisoned.cacheKeyDigest}, envelope.Value(), {}).HasValue());
    fixture.request.assets.cacheFiles = poisonedFiles;
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Dynamic host captures required portable schemas and rejects their absence without replacing the generation",
          "[native][prefab-cook][host][template][schema]") {
    HostFixture fixture;
    const auto type = Gameplay::ComponentTypeId::Parse("game.tests.data").Value();
    Gameplay::ComponentRegistry components;
    REQUIRE(components.Register({.typeId = type, .schemaVersion = 1, .displayName = "Data", .category = "Tests"}).HasValue());
    REQUIRE(components.Freeze().HasValue());
    auto schemas = PrefabCookSchemaContext::Capture(components, {});
    REQUIRE(schemas.HasValue());
    fixture.request.schemas = schemas.Value();
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    Prefab::PrefabObjectNode root{.localId = {0}, .name = "Portable root"};
    root.components.push_back({.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                               .component = {.typeId = type, .schemaVersion = 1, .payload = {std::byte{'{'}, std::byte{'}'}}}});
    auto document = Prefab::PrefabDocument::Create({.projectVersion = fixture.version, .assetId = fixture.prefabId, .objects = {root}},
                                                   Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(document.HasValue());
    fixture.WriteText("assets/hierarchy.prefab", document.Value().SerializeCanonical().Value());
    // Static scene projection uses a plain prefab; portable members belong to the selected runtime template.
    const auto staticPrefab = fixture.AddSpawnableRoot();
    const std::vector<SceneSource::ScenePrefabInstance> placements{
        {.instanceId = Prefab::PrefabInstanceId::Create(7).Value(),
         .sourcePrefab = Prefab::PrefabAssetReference::Create(staticPrefab).Value()}};
    fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({{}, placements}));
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto artifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    const auto cooked = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.Value().payload}), fixture.prefabId,
                                                    Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(cooked.HasValue());
    CHECK(std::get<Prefab::RawComponentPayload>(cooked.Value().Data().entities[0].members[0]).component.typeId == type);
    Gameplay::ComponentRegistry changedRegistry;
    REQUIRE(changedRegistry
                .Register({.typeId = type,
                           .schemaVersion = 1,
                           .displayName = "Data",
                           .category = "Tests",
                           .properties = {{Gameplay::ComponentPropertyId::Parse("optional").Value(), "Optional",
                                           Gameplay::ComponentPropertyKind::String, false}}})
                .HasValue());
    REQUIRE(changedRegistry.Freeze().HasValue());
    const auto changedSchemas = PrefabCookSchemaContext::Capture(changedRegistry, {});
    REQUIRE(changedSchemas.HasValue());
    fixture.request.schemas = changedSchemas.Value();
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 2);  // Resource and scene do not consume this component schema.
    const auto changedArtifact = GenerationArtifact(changed.Value().generation, fixture.prefabId);
    REQUIRE(changedArtifact.HasValue());
    CHECK(changedArtifact.Value().cacheKeyDigest != artifact.Value().cacheKeyDigest);
    fixture.request.schemas.reset();
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(changed.Value().generation);
}

TEST_CASE("Dynamic template payload ceilings accept the exact encoded boundary and reject the next byte",
          "[native][prefab-cook][host][template][limits]") {
    HostFixture fixture;
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    const auto artifact = GenerationArtifact(first.Value().generation, fixture.prefabId);
    REQUIRE(artifact.HasValue());
    fixture.request.prefabPolicy.maximumCookedPayloadBytes = artifact.Value().payload.size() - Prefab::CookedPrefabHeaderBytes;
    const auto exact = fixture.Cook();
    REQUIRE(exact.HasValue());
    --fixture.request.prefabPolicy.maximumCookedPayloadBytes;
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(exact.Value().generation);
}

TEST_CASE("Dependency local cook keys retain unrelated template scene and resource hits", "[native][prefab-cook][host][closure-key]") {
    HostFixture fixture;
    const auto unrelated = fixture.AddSpawnableRoot();
    fixture.request.runtimePrefabRoots = {fixture.prefabId, unrelated};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    fixture.WritePrefab(19);
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 2);  // Independent template and resource.
    for (const auto id : {unrelated, TestMeshRecord().id}) {
        const auto before = GenerationArtifact(first.Value().generation, id);
        const auto after = GenerationArtifact(changed.Value().generation, id);
        REQUIRE(before.HasValue());
        REQUIRE(after.HasValue());
        CHECK(before.Value().cacheKeyDigest == after.Value().cacheKeyDigest);
        CHECK(before.Value().payload == after.Value().payload);
    }
    for (const auto id : {fixture.prefabId, fixture.sceneId}) {
        const auto before = GenerationArtifact(first.Value().generation, id);
        const auto after = GenerationArtifact(changed.Value().generation, id);
        REQUIRE(before.HasValue());
        REQUIRE(after.HasValue());
        CHECK(before.Value().cacheKeyDigest != after.Value().cacheKeyDigest);
        CHECK(before.Value().payload != after.Value().payload);
    }
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto reduced = fixture.Cook();
    REQUIRE(reduced.HasValue());
    CHECK(reduced.Value().cacheHits == 3);
    fixture.request.runtimePrefabRoots = {unrelated, fixture.prefabId};
    const auto restored = fixture.Cook();
    REQUIRE(restored.HasValue());
    CHECK(restored.Value().cacheHits == 4);
}

TEST_CASE("Unreferenced source and unused schema changes preserve semantic cook keys", "[native][prefab-cook][host][closure-key]") {
    HostFixture fixture;
    const auto unused = fixture.AddSpawnableRoot();
    fixture.request.runtimePrefabRoots = {fixture.prefabId};
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    auto document = Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                    .assetId = unused,
                                                    .objects = {{.localId = {0},
                                                                 .name = "Unrelated changed object",
                                                                 .localTransform = {.translation = {0, 7, 0}}}}},
                                                   Prefab::PrefabLimitProfile::Create({}).Value());
    REQUIRE(document.HasValue());
    fixture.WriteText("assets/second.prefab", document.Value().SerializeCanonical().Value());
    Gameplay::ComponentRegistry components;
    REQUIRE(components
                .Register({.typeId = Gameplay::ComponentTypeId::Parse("game.unused.schema").Value(),
                           .schemaVersion = 1,
                           .displayName = "Unused",
                           .category = "Tests"})
                .HasValue());
    REQUIRE(components.Freeze().HasValue());
    const auto schemas = PrefabCookSchemaContext::Capture(components, {});
    REQUIRE(schemas.HasValue());
    fixture.request.schemas = schemas.Value();
    const auto changed = fixture.Cook();
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 3);
    CHECK(first.Value().generation.manifestDigest == changed.Value().generation.manifestDigest);
    const auto revision = fixture.registry.Snapshot().Revision();
    fixture.PublishRegistry();  // Publication counters are freshness evidence, never content identity.
    CHECK(fixture.registry.Snapshot().Revision() != revision);
    const auto republished = fixture.Cook();
    REQUIRE(republished.HasValue());
    CHECK(republished.Value().cacheHits == 3);
}

TEST_CASE("Incomplete missing and cyclic resource closures fail before output and retain the prior generation",
          "[native][prefab-cook][host][closure-key]") {
    HostFixture fixture;
    const auto first = fixture.Cook();
    REQUIRE(first.HasValue());
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    std::optional<std::vector<AssetId>> dependencies;
    SECTION("undeclared producer") {
        dependencies = std::nullopt;
    }
    SECTION("missing dependency") {
        dependencies = std::vector{Id("00000000-0000-0000-0000-000000000099")};
    }
    SECTION("cyclic dependency") {
        dependencies = std::vector{TestMeshRecord().id};
    }
    CookerCatalog catalog;
    REQUIRE(catalog
                .Register({"test.dependency",
                           Type("core.mesh"),
                           {fixture.request.assets.target},
                           std::make_shared<const DependencyCooker>(dependencies, calls)})
                .HasValue());
    const auto published = catalog.Publish();
    REQUIRE(published.HasValue());
    PrefabSceneCookHost host{fixture.jobs, published.Value(), fixture.registry, fixture.inspector, fixture.mutations, fixture.migrations};
    const auto rejected = host.Cook(fixture.request);
    REQUIRE(rejected.HasError());
    CHECK(calls->load() == 0);
    fixture.AssertRetained(first.Value().generation);
}

TEST_CASE("Transitive resource source changes invalidate only their dependent cook branch", "[native][prefab-cook][host][closure-key]") {
    HostFixture fixture;
    const auto unrelated = fixture.AddSpawnableRoot();
    const auto second = AddSecondMesh(fixture.project);
    auto records = std::vector<AssetRecord>{fixture.registry.Snapshot().Records().begin(), fixture.registry.Snapshot().Records().end()};
    records.push_back(second);
    REQUIRE(fixture.registry.Publish(std::move(records)).status == AssetRegistryBuildStatus::Complete);
    fixture.WritePrefab(4, {TestMeshRecord().id});
    fixture.request.runtimePrefabRoots = {fixture.prefabId, unrelated};
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto strategy = std::make_shared<DependencyCooker>(std::vector{second.id}, calls);
    strategy->dependencySource = TestMeshRecord().id;
    CookerCatalog catalog;
    REQUIRE(catalog.Register({"test.transitive", Type("core.mesh"), {fixture.request.assets.target}, strategy}).HasValue());
    const auto published = catalog.Publish();
    REQUIRE(published.HasValue());
    PrefabSceneCookHost host{fixture.jobs, published.Value(), fixture.registry, fixture.inspector, fixture.mutations, fixture.migrations};
    const auto first = host.Cook(fixture.request);
    REQUIRE(first.HasValue());
    WriteFile(fixture.project.assetsDir / "second_mesh.fbx", std::array<std::uint8_t, 2>{8, 9});
    const auto changed = host.Cook(fixture.request);
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().cacheHits == 1);
    for (const auto id : {TestMeshRecord().id, second.id, fixture.prefabId, fixture.sceneId}) {
        const auto before = GenerationArtifact(first.Value().generation, id);
        const auto after = GenerationArtifact(changed.Value().generation, id);
        REQUIRE(before.HasValue());
        REQUIRE(after.HasValue());
        CHECK(before.Value().cacheKeyDigest != after.Value().cacheKeyDigest);
    }
    const auto before = GenerationArtifact(first.Value().generation, unrelated);
    const auto after = GenerationArtifact(changed.Value().generation, unrelated);
    REQUIRE(before.HasValue());
    REQUIRE(after.HasValue());
    CHECK(before.Value().cacheKeyDigest == after.Value().cacheKeyDigest);
    auto throwing = std::make_shared<DependencyCooker>(std::vector<AssetId>{}, calls);
    throwing->throwDeclaration = true;
    CookerCatalog failedCatalog;
    REQUIRE(failedCatalog.Register({"test.throwing", Type("core.mesh"), {fixture.request.assets.target}, throwing}).HasValue());
    const auto failedPublished = failedCatalog.Publish();
    REQUIRE(failedPublished.HasValue());
    PrefabSceneCookHost failing{fixture.jobs,      failedPublished.Value(), fixture.registry,
                                fixture.inspector, fixture.mutations,       fixture.migrations};
    REQUIRE(failing.Cook(fixture.request).HasError());
    fixture.AssertRetained(changed.Value().generation);
}
