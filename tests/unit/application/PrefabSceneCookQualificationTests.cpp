#include "Horo/Scene/CookedSceneDefinition.h"
#include "PrefabSceneCookHostFixture.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application::PrefabCookTestSupport;

    /** @brief Writes real canonical project sources consumed by the production cook capture and owned job composition. */
    void WriteReferenceScene(HostFixture &fixture, const std::size_t instances) {
        const auto child = fixture.AddSpawnableRoot();
        const auto resource = TestMeshRecord().id;
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        auto nested = Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                      .assetId = child,
                                                      .objects = {{.localId = {0}, .name = "Nested root"},
                                                                  {.localId = {99},
                                                                   .parentLocalId = Prefab::LocalObjectId{0},
                                                                   .localTransform = {.translation = {0, 4, 0}}}},
                                                      .referencedAssets = {resource}},
                                                     limits);
        REQUIRE(nested.HasValue());
        const auto nestedBytes = nested.Value().SerializeCanonical();
        REQUIRE(nestedBytes.HasValue());
        fixture.WriteText("assets/second.prefab", nestedBytes.Value());
        const Prefab::PrefabSourceRevision nestedRevision{fixture.version, ComputeSha256(std::as_bytes(std::span{nestedBytes.Value()}))};
        auto root = Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                    .assetId = fixture.prefabId,
                                                    .objects = {{.localId = {0}, .name = "Reference root"}},
                                                    .composition =
                                                        Prefab::PrefabComposition{
                                                            .nestedPlacements =
                                                                {{.placementLocalId = {10},
                                                                  .sourcePrefab = Prefab::PrefabAssetReference::Create(child).Value(),
                                                                  .authoredAgainst = nestedRevision,
                                                                  .localRootTransform = {.translation = {1, 0, 0}}},
                                                                 {.placementLocalId = {20},
                                                                  .sourcePrefab = Prefab::PrefabAssetReference::Create(child).Value(),
                                                                  .authoredAgainst = nestedRevision,
                                                                  .localRootTransform = {.translation = {2, 0, 0}}}}},
                                                    .referencedAssets = {child}},
                                                   limits);
        REQUIRE(root.HasValue());
        fixture.WriteText("assets/hierarchy.prefab", root.Value().SerializeCanonical().Value());
        const std::vector<SceneSource::SceneObjectSnapshot> objects{{.id = {900}, .name = "Authored owner"}};
        std::vector<SceneSource::ScenePrefabInstance> placements;
        for (std::size_t index = 0; index < instances; ++index) {
            placements.push_back({.instanceId = Prefab::PrefabInstanceId::Create(index + 1).Value(),
                                  .sourcePrefab = Prefab::PrefabAssetReference::Create(fixture.prefabId).Value(),
                                  .parent = SceneSource::SceneObjectId{900},
                                  .rootTransform = {.translation = {static_cast<float>(index * 10), 0, 0}}});
        }
        fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, placements}));
    }

    /** @brief Reads the actual published scene envelope and reloads only its source-free runtime bytes. */
    std::vector<std::uint8_t> RequirePublishedReference(const HostFixture &fixture, const AssetCookGeneration &generation,
                                                        const std::size_t instances) {
        const auto inventory = ReadCookGenerationContents(generation, 1024U * 1024U);
        REQUIRE(inventory.HasValue());
        REQUIRE(inventory.Value().artifacts.size() == 2);
        std::vector<std::uint8_t> scene;
        for (const auto &bytes : inventory.Value().artifacts) {
            const auto envelope = Assets::DecodeCookedArtifact(bytes);
            REQUIRE(envelope.HasValue());
            if (envelope.Value().id == fixture.sceneId)
                scene = envelope.Value().payload;
            CHECK(envelope.Value().id != fixture.prefabId);
        }
        REQUIRE_FALSE(scene.empty());
        const auto wire = nlohmann::json::parse(scene);
        const auto decoded = SceneCook::DecodeCookedSceneDefinition(scene, {wire.at("scene").get<std::uint64_t>()},
                                                                    {wire.at("revision").get<std::uint64_t>()});
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().Entities().size() == 1 + 5 * instances);
        REQUIRE(decoded.Value().AssetDependencies().size() == 1);
        CHECK(decoded.Value().AssetDependencies().front().id == TestMeshRecord().id);
        CHECK(decoded.Value().AssetDependencies().front().expectedType == Type("core.mesh"));
        for (std::size_t index = 0; index < instances; ++index) {
            const auto entities = decoded.Value().Entities().subspan(1 + 5 * index, 5);
            CHECK(entities[0].parent == Runtime::SceneObjectId{900});
            CHECK(entities[0].localTransform.translation == Math::Vec3{static_cast<float>(index * 10), 0, 0});
            CHECK(entities[1].parent == entities[0].object);
            CHECK(entities[2].parent == entities[1].object);
            CHECK(entities[3].parent == entities[0].object);
            CHECK(entities[4].parent == entities[3].object);
            CHECK(entities[1].localTransform.translation == Math::Vec3{1, 0, 0});
            CHECK(entities[3].localTransform.translation == Math::Vec3{2, 0, 0});
            CHECK(entities[2].localTransform.translation == Math::Vec3{0, 4, 0});
            CHECK(entities[4].localTransform.translation == Math::Vec3{0, 4, 0});
        }
        const std::string_view text{reinterpret_cast<const char *>(scene.data()), scene.size()};
        CHECK(text.find("assets/") == std::string_view::npos);
        CHECK(text.find("sourcePrefab") == std::string_view::npos);
        CHECK(text.find("prefabInstances") == std::string_view::npos);
        const auto encoded = SceneCook::EncodeCookedSceneDefinition(decoded.Value());
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value() == scene);
        return scene;
    }
}  // namespace

TEST_CASE("Production prefab cook qualifies repeated hierarchy resource closure and cold hit bytes",
          "[native][prefab-cook][qualification][host]") {
    HostFixture fixture;
    const auto instances = GENERATE(std::size_t{1}, std::size_t{8});
    WriteReferenceScene(fixture, instances);
    auto host = fixture.MakeHost();
    const auto cold = host.Cook(fixture.request);
    REQUIRE(cold.HasValue());
    CHECK(cold.Value().cacheHits == 0);
    const auto reference = RequirePublishedReference(fixture, cold.Value().generation, instances);
    for (unsigned run = 0; run < 2; ++run) {
        const auto hit = host.Cook(fixture.request);
        REQUIRE(hit.HasValue());
        CHECK(hit.Value().cacheHits == 2);
        CHECK(hit.Value().generation.manifestDigest == cold.Value().generation.manifestDigest);
        CHECK(RequirePublishedReference(fixture, hit.Value().generation, instances) == reference);
        CHECK(fixture.jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
    }
}

TEST_CASE("Production repeated prefab cook retains generation across malformed boundary and stale completion failures",
          "[native][prefab-cook][qualification][host][lifecycle]") {
    HostFixture fixture;
    WriteReferenceScene(fixture, 8);
    auto host = fixture.MakeHost();
    const auto previous = host.Cook(fixture.request);
    REQUIRE(previous.HasValue());
    const auto reference = RequirePublishedReference(fixture, previous.Value().generation, 8);
    SECTION("later nested source is malformed") {
        fixture.WriteText("assets/second.prefab", "{");
    }
    SECTION("containing scene exceeds exact entity limit") {
        fixture.request.sceneLimits.maximumEntities = 40;
    }
    SECTION("registry changes at final host publication fence") {
        fixture.request.assets.validateHostInputs = [&fixture] {
            fixture.PublishRegistry();
            return Result<void>::Success();
        };
    }
    SECTION("cancellation arrives after candidate selector staging") {
        CancellationSource cancellation;
        auto publication = std::make_shared<HostPublicationFiles>();
        publication->staged = [&cancellation] {
            cancellation.RequestCancellation();
        };
        fixture.request.assets.publicationFiles = publication;
        CHECK(host.Cook(fixture.request, cancellation.Token()).HasError());
        fixture.AssertRetained(previous.Value().generation);
        CHECK(RequirePublishedReference(fixture, previous.Value().generation, 8) == reference);
        return;
    }
    CHECK(host.Cook(fixture.request).HasError());
    fixture.AssertRetained(previous.Value().generation);
    CHECK(RequirePublishedReference(fixture, previous.Value().generation, 8) == reference);
    CHECK(fixture.jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
}
