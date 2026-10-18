#include "Horo/Application/PhysicsTriggerVolumeHost.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "PrefabSceneCookHostFixture.h"

#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::PrefabCookTestSupport;
    using namespace Horo::Assets::ServiceTestSupport;

    /** @brief Creates exact fixture registry records without exposing another fixture's private helpers. */
    Assets::AssetRecord TriggerAssetRecord(Assets::AssetId id, std::string_view type, std::string_view path) {
        return {.id = id,
                .type = Type(type),
                .sourcePath = ProjectPath::Parse(path).Value(),
                .metadataPath = ProjectPath::Parse(std::string(path) + ".horo").Value()};
    }

    /** @brief Actual project collision authority selected explicitly by the headless host. */
    Physics::PhysicsWorldSimulationBinding ProjectSimulation() {
        const auto layer = Physics::CollisionLayerId::FromBytes(Id("00000000-0000-0000-0000-000000000001").Bytes());
        const auto profile = Physics::CollisionProfileId::FromBytes(Id("00000000-0000-0000-0000-000000000002").Bytes());
        const auto channel = Physics::PhysicsQueryChannelId::FromBytes(Id("00000000-0000-0000-0000-000000000003").Bytes());
        Physics::ProjectCollisionSchema
            authored{.defaultProfile = profile,
                     .layers = {{layer}},
                     .pairs = {{layer, layer, Physics::SimulationPairResponse::Overlap}},
                     .queryChannels = {{channel}},
                     .profiles = {{profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Overlap}}}}};
        auto normalized = Physics::NormalizedCollisionSchema::Create(authored);
        REQUIRE(normalized.HasValue());
        return {std::make_shared<const Physics::NormalizedCollisionSchema>(std::move(normalized).Value()), 1};
    }
}  // namespace

namespace {
    /** @brief Owns real project files, jobs and cook host together; no returned artifact borrows constructor locals. */
    struct TriggerCookFixture {
        HostFixture fixture;
        Physics::PhysicsWorldSimulationBinding simulation{ProjectSimulation()};
        Physics::CollisionProfileId profile{simulation.schema->Profiles().front().id};
        Assets::AssetId materialId{Id("00000000-0000-0000-0000-000000000091")};
        Assets::AssetTypeId materialType{Type("core.physics_material")};
        Physics::PhysicsMaterialAsset material{
            Physics::PhysicsMaterialAsset::Create({.id = Physics::PhysicsMaterialAssetId::FromAssetId(materialId), .sourceRevision = 7})
                .Value()};
        Assets::AssetCookArtifact artifact;
        std::unique_ptr<PrefabSceneCookHost> host;

        TriggerCookFixture() {
            const auto encoded = Physics::EncodePhysicsMaterialAsset(material).Value();
            const std::span bytes{reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size()};
            WriteFile(fixture.project.dir.path / "assets/trigger.material", bytes);
            fixture.WriteText("assets/trigger.material.horo", SidecarJson(materialId.ToString(), "core.physics_material"));
            const std::array objects{
                SceneSource::SceneObjectSnapshot{.id = {874}, .components = {.triggerVolume = Runtime::TriggerVolumeComponent{}}}};
            fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, {}}));
            REQUIRE(fixture.registry
                        .Publish({TestMeshRecord(), TriggerAssetRecord(fixture.prefabId, "core.prefab", "assets/hierarchy.prefab"),
                                  TriggerAssetRecord(fixture.sceneId, "core.scene", "assets/level.scene"),
                                  TriggerAssetRecord(materialId, "core.physics_material", "assets/trigger.material")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            artifact = {.id = materialId,
                        .type = materialType,
                        .target = fixture.request.assets.target,
                        .sourceDigest = ComputeSha256(std::as_bytes(bytes)),
                        .payloadDigest = ComputeSha256(std::as_bytes(bytes)),
                        .payload = {bytes.begin(), bytes.end()}};
            ComposeCookHost();
        }

        void ComposeCookHost() {
            Assets::CookerCatalog catalog;
            REQUIRE(catalog
                        .Register({.contributionId = "test.mesh",
                                   .assetType = Type("core.mesh"),
                                   .targets = {fixture.request.assets.target},
                                   .strategy = std::make_shared<const SettingsCooker>(1)})
                        .HasValue());
            REQUIRE(catalog
                        .Register({.contributionId = "test.material",
                                   .assetType = materialType,
                                   .targets = {fixture.request.assets.target},
                                   .strategy = MakePhysicsMaterialCooker()})
                        .HasValue());
            auto sealed = catalog.Publish();
            REQUIRE(sealed.HasValue());
            host = std::make_unique<PrefabSceneCookHost>(fixture.jobs, sealed.Value(), fixture.registry, fixture.inspector,
                                                         fixture.mutations, fixture.migrations);
        }

        Assets::AssetCookReport CookValid() {
            auto result = CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, material, artifact);
            INFO((result.HasError() ? result.ErrorValue().message : "Cook succeeded"));
            INFO((result.HasError() ? result.ErrorValue().code.Value() : std::string_view{}));
            INFO((result.HasError() ? result.ErrorValue().domain.Value() : std::string_view{}));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    };
}  // namespace

TEST_CASE_METHOD(TriggerCookFixture, "Public project Physics capture supplies real material closure to transactional scene cook",
                 "[host][trigger][cook]") {
    const auto capture = CaptureProjectTriggerVolumeBinding(simulation, profile, material, artifact);
    REQUIRE(capture.HasValue());
    REQUIRE(capture.Value().binding.materialSourceRevision == 7);
    REQUIRE(capture.Value().materialSourceDigest == artifact.sourceDigest);
    const auto cooked = CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, material, artifact);
    INFO((cooked.HasError() ? cooked.ErrorValue().message : "Cook succeeded"));
    INFO((cooked.HasError() ? cooked.ErrorValue().code.Value() : std::string_view{}));
    INFO((cooked.HasError() ? cooked.ErrorValue().domain.Value() : std::string_view{}));
    REQUIRE(cooked.HasValue());
    const auto inventory = Assets::ReadCookGenerationContents(cooked.Value().generation, 1024U * 1024U);
    REQUIRE(inventory.HasValue());
    bool foundScene{}, foundMaterial{};
    for (const auto &encoded : inventory.Value().artifacts) {
        const auto decoded = Assets::DecodeCookedArtifact(encoded);
        REQUIRE(decoded.HasValue());
        if (decoded.Value().id == materialId) {
            foundMaterial = true;
            REQUIRE(decoded.Value().payloadDigest == artifact.payloadDigest);
        }
        if (decoded.Value().id == fixture.sceneId) {
            foundScene = true;
            const auto &payload = decoded.Value().payload;
            const auto document = nlohmann::json::parse(payload);
            const auto scene = SceneCook::DecodeCookedSceneDefinition(payload, {document["scene"].get<std::uint64_t>()},
                                                                      {document["revision"].get<std::uint64_t>()});
            REQUIRE(scene.HasValue());
            REQUIRE(scene.Value().AssetDependencies().size() == 1);
            REQUIRE(scene.Value().TriggerVolumeEvidence().front().binding == capture.Value().binding);
        }
    }
    REQUIRE(foundScene);
    REQUIRE(foundMaterial);
}

TEST_CASE_METHOD(TriggerCookFixture, "Material capture rejects malformed or semantically different snapshots", "[host][trigger][cook]") {
    const auto cooked = CookValid();
    SECTION("wrong actual resource bytes cannot publish another generation") {
        auto changed = artifact;
        changed.payload.push_back(4);
        changed.payloadDigest = ComputeSha256(std::as_bytes(std::span{changed.payload}));
        REQUIRE(CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, material, changed).HasError());
        fixture.AssertRetained(cooked.generation);
    }
    SECTION("same identity and revision cannot hide different material coefficients") {
        auto descriptor = material.Descriptor();
        descriptor.friction = 0.75F;
        const auto other = Physics::PhysicsMaterialAsset::Create(descriptor).Value();
        const auto encodedOther = Physics::EncodePhysicsMaterialAsset(other).Value();
        const auto otherBytes = std::span{reinterpret_cast<const std::uint8_t *>(encodedOther.data()), encodedOther.size()};
        auto selfConsistent = artifact;
        selfConsistent.payload.assign(otherBytes.begin(), otherBytes.end());
        selfConsistent.payloadDigest = ComputeSha256(std::as_bytes(otherBytes));
        REQUIRE(CaptureProjectTriggerVolumeBinding(simulation, profile, material, selfConsistent).HasError());
        REQUIRE(CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, material, selfConsistent).HasError());
        fixture.AssertRetained(cooked.generation);
    }
    SECTION("corrupt artifact payload is rejected at public capture") {
        artifact.payload.front() ^= 1;
        REQUIRE(CaptureProjectTriggerVolumeBinding(simulation, profile, material, artifact).HasError());
    }
}

TEST_CASE_METHOD(TriggerCookFixture, "Actual material source and cooker output preserve the published generation",
                 "[host][trigger][cook]") {
    const auto cooked = CookValid();
    SECTION("actual cooker bytes must agree with a valid captured different material snapshot") {
        auto descriptor = material.Descriptor();
        descriptor.friction = 0.75F;
        const auto other = Physics::PhysicsMaterialAsset::Create(descriptor).Value();
        const auto encodedOther = Physics::EncodePhysicsMaterialAsset(other).Value();
        const auto otherBytes = std::span{reinterpret_cast<const std::uint8_t *>(encodedOther.data()), encodedOther.size()};
        auto selfConsistent = artifact;
        selfConsistent.payload.assign(otherBytes.begin(), otherBytes.end());
        selfConsistent.payloadDigest = ComputeSha256(std::as_bytes(otherBytes));
        REQUIRE(CaptureProjectTriggerVolumeBinding(simulation, profile, other, selfConsistent).HasValue());
        // The source file remains the original material. Its actual cooker output cannot be published under the other proof.
        REQUIRE(CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, other, selfConsistent).HasError());
        fixture.AssertRetained(cooked.generation);
    }
    SECTION("foreign schema profile cannot use the project default") {
        REQUIRE(CaptureProjectTriggerVolumeBinding(simulation, {}, material, artifact).HasError());
    }
    SECTION("source capture must match actual project file") {
        artifact.sourceDigest.bytes.front() ^= 1;
        REQUIRE(CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, material, artifact).HasError());
        fixture.AssertRetained(cooked.generation);
    }
}

TEST_CASE_METHOD(TriggerCookFixture, "Direct trigger material dependencies reject missing or foreign registry authority",
                 "[host][trigger][cook]") {
    const auto previous = CookValid();
    std::vector<Assets::AssetRecord> records{TestMeshRecord(),
                                             TriggerAssetRecord(fixture.prefabId, "core.prefab", "assets/hierarchy.prefab"),
                                             TriggerAssetRecord(fixture.sceneId, "core.scene", "assets/level.scene")};
    SECTION("Missing material registry identity") {}
    SECTION("Wrong material registry type") {
        records.push_back(TriggerAssetRecord(materialId, "core.mesh", "assets/trigger.material"));
    }
    REQUIRE(fixture.registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
    const auto result = CookScenesWithProjectTriggerVolumes(*host, fixture.request, simulation, profile, material, artifact);
    REQUIRE(result.HasError());
    fixture.AssertRetained(previous.generation);
}

TEST_CASE_METHOD(TriggerCookFixture, "Mixed authored trigger and actual Prefab roots retain the same material closure",
                 "[host][trigger][cook]") {
    const std::array objects{
        SceneSource::SceneObjectSnapshot{.id = {874}, .components = {.triggerVolume = Runtime::TriggerVolumeComponent{}}}};
    const std::array placements{
        SceneSource::ScenePrefabInstance{.instanceId = Prefab::PrefabInstanceId::Create(7).Value(),
                                         .sourcePrefab = Prefab::PrefabAssetReference::Create(fixture.prefabId).Value(),
                                         .parent = SceneSource::SceneObjectId{874}}};
    fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, placements}));
    const auto cooked = CookValid();
    const auto inventory = Assets::ReadCookGenerationContents(cooked.generation, 1024U * 1024U);
    REQUIRE(inventory.HasValue());
    bool foundScene{}, foundMaterial{};
    for (const auto &encoded : inventory.Value().artifacts) {
        const auto decoded = Assets::DecodeCookedArtifact(encoded);
        REQUIRE(decoded.HasValue());
        if (decoded.Value().id == materialId) {
            foundMaterial = true;
            CHECK(decoded.Value().payloadDigest == artifact.payloadDigest);
        }
        if (decoded.Value().id == fixture.sceneId) {
            foundScene = true;
            const auto document = nlohmann::json::parse(decoded.Value().payload);
            CHECK(document["entities"].size() == 3);
            CHECK(document["dependencies"].size() == 1);
            CHECK(document["triggerMigration"].size() == 1);
        }
    }
    REQUIRE(foundScene);
    REQUIRE(foundMaterial);
}
