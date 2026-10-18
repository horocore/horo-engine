#pragma once

/** @file CharacterSceneProductFixture.h
 * @brief Real project/collision/material/cook fixture for actual application Scene composition.
 */
#include "Horo/Application/PhysicsTriggerVolumeHost.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PrefabSceneCookHostFixture.h"

#include <span>
#include <utility>

namespace Horo::Application::CharacterSceneProductTest {
    using namespace PrefabCookTestSupport;

    struct Fixture final {
        HostFixture project;
        const Assets::AssetId materialId{Id("00000000-0000-0000-0000-000000000091")};
        const Physics::CollisionLayerId layer{Physics::CollisionLayerId::FromBytes(Id("00000000-0000-0000-0000-000000000001").Bytes())};
        const Physics::CollisionProfileId profile{
            Physics::CollisionProfileId::FromBytes(Id("00000000-0000-0000-0000-000000000002").Bytes())};
        const Physics::PhysicsQueryChannelId channel{
            Physics::PhysicsQueryChannelId::FromBytes(Id("00000000-0000-0000-0000-000000000003").Bytes())};
        SceneSource::SceneSourceDocument source;

        /** @brief Cooks actual canonical authored data with optional extra inert Scene objects before product capture. */
        explicit Fixture(const std::size_t extraObjects = 0) {
            project.request.assets.target = Target("headless-canonical");
            // The generic cook fixture uses synthetic extensions; product discovery needs canonical source/sidecar pairs.
            std::filesystem::rename(project.project.dir.path / "assets/level.scene", project.project.dir.path / "assets/level.horo");
            std::filesystem::rename(project.project.dir.path / "assets/level.scene.horo",
                                    project.project.dir.path / "assets/level.horo.horo");
            WriteCollision();
            WriteMaterial();
            WriteScene(extraObjects);
            REQUIRE(project.registry
                        .Publish({Record(project.sceneId, "core.scene", "assets/level.horo"),
                                  Record(materialId, "core.physics_material", "assets/controller.horoasset")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            Cook();
        }

        /** @brief Creates this fixture's own registry input without accessing another fixture's private helpers. */
        static Assets::AssetRecord Record(const Assets::AssetId id, const std::string_view type, const std::string_view path) {
            return {.id = id,
                    .type = Type(type),
                    .sourcePath = ProjectPath::Parse(path).Value(),
                    .metadataPath = ProjectPath::Parse(std::string(path) + ".horo").Value()};
        }

        void WriteCollision() {
            ProjectCollisionSettingsInput input{.projectId = "p1"};
            input.schema = {.defaultProfile = profile,
                            .layers = {{layer}},
                            .pairs = {{layer, layer, Physics::SimulationPairResponse::Block}},
                            .queryChannels = {{channel}},
                            .profiles = {{profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}}}};
            input.layers = {{layer, "World", ""}};
            input.profiles = {{profile, "Controller", ""}};
            input.channels = {{channel, "Movement", ""}};
            auto settings = ProjectCollisionSettings::Create(std::move(input));
            REQUIRE(settings.HasValue());
            const auto encoded = EncodeProjectCollisionSettings(settings.Value());
            REQUIRE(encoded.HasValue());
            project.WriteText(".horo/collision.json", encoded.Value());
        }

        void WriteMaterial() {
            const auto material = Physics::PhysicsMaterialAsset::Create(
                {.id = Physics::PhysicsMaterialAssetId::FromAssetId(materialId), .sourceRevision = 7});
            REQUIRE(material.HasValue());
            const auto encoded = Physics::EncodePhysicsMaterialAsset(material.Value());
            REQUIRE(encoded.HasValue());
            const auto &bytes = encoded.Value();
            WriteFile(project.project.dir.path / "assets/controller.horoasset",
                      {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
            project.WriteText("assets/controller.horoasset.horo", SidecarJson(materialId.ToString(), "core.physics_material"));
        }

        void WriteScene(const std::size_t extraObjects = 0) {
            Runtime::CharacterControllerComponent controller{.id = {7}, .enabled = true};
            controller.collision.profile = profile;
            controller.collision.queryChannel = channel;
            controller.collision.defaultMaterial =
                Runtime::PhysicsColliderMaterialBinding{Physics::PhysicsMaterialSlotId::FromValue(1), materialId};
            source.objects = {{.id = {1},
                               .name = "Authored controller",
                               .localTransform = {.translation = {0, 0.495F, 0}},
                               .components = {.camera = Runtime::CameraComponent{}, .characterController = controller}},
                              {.id = {2},
                               .name = "Real initial floor",
                               .components = {.rigidBody = Runtime::RigidBodyComponent{.id = {20}, .body = {200}},
                                              .colliders = {
                                                  {.id = {21},
                                                   .collider = {201},
                                                   .body = {.object = {2}, .body = {200}},
                                                   .source =
                                                       Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{{0, 1, 0}, 0}},
                                                   .collisionProfile = profile,
                                                   .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), materialId}}}}}}};
            for (std::size_t index = 0; index < extraObjects; ++index)
                source.objects.push_back({.id = {index + 3}, .name = std::string(128, 'x')});
            project.WriteText("assets/level.horo", SceneSource::EncodeSceneSource({source.objects, source.prefabInstances}));
        }

        void Cook() {
            Assets::CookerCatalog catalog;
            REQUIRE(catalog
                        .Register({.contributionId = "character.product.material",
                                   .assetType = Type("core.physics_material"),
                                   .targets = {project.request.assets.target},
                                   .strategy = MakePhysicsMaterialCooker()})
                        .HasValue());
            auto sealed = catalog.Publish();
            REQUIRE(sealed.HasValue());
            PrefabSceneCookHost host{project.jobs,      sealed.Value(),    project.registry,
                                     project.inspector, project.mutations, project.migrations};
            const auto cooked = host.Cook(project.request);
            if (cooked.HasError())
                UNSCOPED_INFO(cooked.ErrorValue().message);
            REQUIRE(cooked.HasValue());
        }
    };
}  // namespace Horo::Application::CharacterSceneProductTest
