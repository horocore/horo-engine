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

TEST_CASE("Public project Physics capture supplies real material closure to transactional scene cook", "[host][trigger][cook]") {
    HostFixture fixture;
    const auto simulation = ProjectSimulation();
    const auto profile = simulation.schema->Profiles().front().id;
    const auto materialId = Id("00000000-0000-0000-0000-000000000091");
    const auto materialType = Type("core.physics_material");
    const auto material =
        Physics::PhysicsMaterialAsset::Create({.id = Physics::PhysicsMaterialAssetId::FromAssetId(materialId), .sourceRevision = 7})
            .Value();
    const auto encodedMaterial = Physics::EncodePhysicsMaterialAsset(material).Value();
    const std::span bytes{reinterpret_cast<const std::uint8_t *>(encodedMaterial.data()), encodedMaterial.size()};
    WriteFile(fixture.project.dir.path / "assets/trigger.material", bytes);
    fixture.WriteText("assets/trigger.material.horo", SidecarJson(materialId.ToString(), "core.physics_material"));
    const std::array objects{
        SceneSource::SceneObjectSnapshot{.id = {874}, .components = {.triggerVolume = Runtime::TriggerVolumeComponent{}}}};
    fixture.WriteText("assets/level.scene", SceneSource::EncodeSceneSource({objects, {}}));
    REQUIRE(fixture.registry
                .Publish({TestMeshRecord(), Record(fixture.prefabId, "core.prefab", "assets/hierarchy.prefab"),
                          Record(fixture.sceneId, "core.scene", "assets/level.scene"),
                          Record(materialId, "core.physics_material", "assets/trigger.material")})
                .status == Assets::AssetRegistryBuildStatus::Complete);
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
    PrefabSceneCookHost host{fixture.jobs, sealed.Value(), fixture.registry, fixture.inspector, fixture.mutations, fixture.migrations};
    Assets::AssetCookArtifact artifact{.id = materialId,
                                       .type = materialType,
                                       .target = fixture.request.assets.target,
                                       .sourceDigest = ComputeSha256(std::as_bytes(std::span{bytes})),
                                       .payloadDigest = ComputeSha256(std::as_bytes(std::span{bytes})),
                                       .payload = {bytes.begin(), bytes.end()}};
    const auto capture = CaptureProjectTriggerVolumeBinding(simulation, profile, material, artifact);
    REQUIRE(capture.HasValue());
    REQUIRE(capture.Value().binding.materialSourceRevision == 7);
    REQUIRE(capture.Value().materialSourceDigest == artifact.sourceDigest);
    const auto cooked = CookScenesWithProjectTriggerVolumes(host, fixture.request, simulation, profile, material, artifact);
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
    SECTION("wrong actual resource bytes cannot publish another generation") {
        auto changed = artifact;
        changed.payload.push_back(4);
        changed.payloadDigest = ComputeSha256(std::as_bytes(std::span{changed.payload}));
        REQUIRE(CookScenesWithProjectTriggerVolumes(host, fixture.request, simulation, profile, material, changed).HasError());
        fixture.AssertRetained(cooked.Value().generation);
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
        REQUIRE(CookScenesWithProjectTriggerVolumes(host, fixture.request, simulation, profile, material, selfConsistent).HasError());
        fixture.AssertRetained(cooked.Value().generation);
    }
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
        REQUIRE(CookScenesWithProjectTriggerVolumes(host, fixture.request, simulation, profile, other, selfConsistent).HasError());
        fixture.AssertRetained(cooked.Value().generation);
    }
    SECTION("foreign schema profile cannot use the project default") {
        REQUIRE(CaptureProjectTriggerVolumeBinding(simulation, {}, material, artifact).HasError());
    }
    SECTION("corrupt artifact payload is rejected at public capture") {
        artifact.payload.front() ^= 1;
        REQUIRE(CaptureProjectTriggerVolumeBinding(simulation, profile, material, artifact).HasError());
    }
    SECTION("source capture must match actual project file") {
        artifact.sourceDigest.bytes.front() ^= 1;
        REQUIRE(CookScenesWithProjectTriggerVolumes(host, fixture.request, simulation, profile, material, artifact).HasError());
        fixture.AssertRetained(cooked.Value().generation);
    }
}
