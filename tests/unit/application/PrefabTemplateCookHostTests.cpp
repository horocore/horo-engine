#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Assets;
    using namespace Horo::Assets::ServiceTestSupport;
    using namespace Horo::Application::PrefabCookTestSupport;

    /** @brief Reads actual published envelope bytes without treating a cache directory as authority. */
    Result<AssetCookArtifact> GenerationArtifact(const AssetCookGeneration &generation, const AssetId id) {
        auto inventory = ReadCookGenerationContents(generation, 1024U * 1024U);
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
    const auto inventory = ReadCookGenerationContents(first.Value().generation, 1024U * 1024U);
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
    AssetCookCache cache{poisonedCache.path};
    REQUIRE(cache.Store({poisoned.cacheKeyDigest}, envelope.Value(), {}).HasValue());
    fixture.request.assets.cacheRoot = poisonedCache.path;
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
    fixture.request.schemas.reset();
    REQUIRE(fixture.Cook().HasError());
    fixture.AssertRetained(first.Value().generation);
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
