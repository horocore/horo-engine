#include "CharacterSceneProductFixture.h"
#include "CommittedSceneAssets.h"
#include "HeadlessNetworkServices.h"
#include "Horo/Network/NetworkErrors.h"

#include <chrono>

namespace Horo::Application::CharacterSceneProductTest {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Owns the real published project provider before any borrowing factory or world service. */
        struct HeadlessFixture final {
            Fixture project;
            std::unique_ptr<Internal::CommittedSceneAssets> committed;
            std::unique_ptr<Assets::AssetLoadService> loads;
            CancellationSource initialCancellation;
            Internal::HeadlessSceneLoadBoundary load{initialCancellation.Token(),
                                                     std::chrono::steady_clock::now() + std::chrono::seconds{2}};
            Network::NetworkModeFactories factories;

            HeadlessFixture() {
                auto assets = Internal::CommittedSceneAssets::Capture(Horo::NativeDurableFileSystem{},
                                                                      {project.project.cooked.path, project.project.request.assets.target},
                                                                      project.project.registry.Snapshot());
                REQUIRE(assets.HasValue());
                committed = std::move(assets).Value();
                loads = std::make_unique<Assets::AssetLoadService>(project.project.jobs, committed->Provider());
                auto definition = committed->DecodeScene(project.project.sceneId);
                REQUIRE(definition.HasValue());
                const auto settings = LoadProjectCollisionSettings(project.project.project.dir.path, "p1");
                REQUIRE(settings.HasValue());
                auto simulation = settings.Value().settings->Capture(1);
                REQUIRE(simulation.HasValue());
                auto physics = Physics::PhysicsWorldSettings::Capture({});
                auto character = Character::CharacterWorldSettings::Capture({});
                REQUIRE(physics.HasValue());
                REQUIRE(character.HasValue());
                const std::filesystem::path root{HORO_TEST_SOURCE_ROOT};
                const Internal::GameplayWorldSelection gameplay{.moduleId = "game.tests",
                                                                .scriptSource =
                                                                    root / "tests/fixtures/gameplay_physics/PhysicsContext.horo_script",
                                                                .scriptSidecar =
                                                                    root /
                                                                    "tests/fixtures/gameplay_physics/PhysicsContext.horo_script.meta",
                                                                .physicsPermission = Internal::GameplayPhysicsPermission::Granted,
                                                                .characterPermission = Internal::GameplayCharacterPermission::Granted};
                auto composed =
                    Internal::ComposeHeadlessSceneNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                      std::move(definition).Value()),
                                                                  {Physics::PhysicsRuntimeMode::Canonical,
                                                                   {std::move(physics).Value(), std::move(character).Value(),
                                                                    std::move(simulation).Value()},
                                                                   project.project.registry,
                                                                   *loads,
                                                                   load},
                                                                  {}, gameplay,
                                                                  Gameplay::GameplayReplicationAdmissionPolicy{
                                                                      .inventory = {
                                                                          .completeness =
                                                                              Network::NetworkReplicationInventoryCompleteness::Complete}});
                REQUIRE(composed.HasValue());
                factories = std::move(composed).Value();
            }

            Result<std::unique_ptr<Network::INetworkModeService>> Make(const Network::NetworkModeServiceKind kind,
                                                                       const Network::NetworkModeWorldKind world,
                                                                       const Runtime::SceneRuntimeId scene) {
                return factories.services[static_cast<std::size_t>(kind)](
                    {kind, Network::NetworkProjectRole::ListenServer, {}, world, scene});
            }
        };

        /** @brief Completes the actual Scene-first service pair without manufacturing a runtime ID or native handle. */
        std::unique_ptr<Network::INetworkModeService> PreparePair(HeadlessFixture &fixture,
                                                                  std::unique_ptr<Network::INetworkModeService> &scene,
                                                                  Network::NetworkModeWorldKind role, Runtime::SceneRuntimeId identity) {
            auto preparedScene = fixture.Make(Network::NetworkModeServiceKind::Scene, role, identity);
            REQUIRE(preparedScene.HasValue());
            scene = std::move(preparedScene).Value();
            auto preparedPhysics = fixture.Make(Network::NetworkModeServiceKind::Physics, role, identity);
            REQUIRE(preparedPhysics.HasValue());
            auto physics = std::move(preparedPhysics).Value();
            REQUIRE(scene->Prepare().HasValue());
            REQUIRE(physics->Prepare().HasValue());
            REQUIRE(scene->Activate().HasValue());
            REQUIRE(physics->Activate().HasValue());
            return physics;
        }
    }  // namespace

    TEST_CASE("Configured headless role records reserve identities and retire the exact Scene-owned pair",
              "[character][scene][headless][product][canonical]") {
        HeadlessFixture fixture;
        std::unique_ptr<Network::INetworkModeService> authorityScene, clientScene;
        auto authority = PreparePair(fixture, authorityScene, Network::NetworkModeWorldKind::AuthorityServer, {91});
        auto client = PreparePair(fixture, clientScene, Network::NetworkModeWorldKind::Client, {92});
        const auto authorityGrant = Internal::InspectHeadlessGameplayContext(*authority);
        const auto clientGrant = Internal::InspectHeadlessGameplayContext(*client);
        REQUIRE(authorityGrant);
        REQUIRE(clientGrant);
        CHECK(authorityGrant->Binding().scene == 91);
        CHECK(authorityGrant->Binding().sceneGeneration == 91);
        CHECK(clientGrant->Binding().scene == 92);
        CHECK(authorityGrant->Binding().world != clientGrant->Binding().world);
        const auto duplicate = fixture.Make(Network::NetworkModeServiceKind::Scene, Network::NetworkModeWorldKind::Client, {91});
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == Network::NetworkErrors::NetworkModeInvalid.code.Value());
        CHECK(authority->Activate().HasError());
        const auto authorityTick = authority->RunFixedTick({1, Duration::FromNanoseconds(16'666'667), {}});
        INFO((authorityTick.HasError() ? authorityTick.ErrorValue().code.Value() + ": " + authorityTick.ErrorValue().message
                                       : "authority tick succeeded"));
        REQUIRE(authorityTick.HasValue());
        authorityScene->Shutdown();
        CHECK(authorityGrant->Acquire("game.tests", 91, 91).HasError());
        CHECK(authority->RunFixedTick({2, Duration::FromNanoseconds(16'666'667), {}}).HasError());
        CHECK(clientGrant->Acquire("game.tests", 92, 92).HasValue());
        const auto clientTick = client->RunFixedTick({1, Duration::FromNanoseconds(16'666'667), {}});
        INFO((clientTick.HasError() ? clientTick.ErrorValue().code.Value() + ": " + clientTick.ErrorValue().message
                                    : "client tick succeeded"));
        REQUIRE(clientTick.HasValue());
        authority->Shutdown();
        client->Shutdown();
        clientScene->Shutdown();
        CHECK(clientGrant->Acquire("game.tests", 92, 92).HasError());
    }

    TEST_CASE("A cancelled new load boundary leaves the prior configured headless Scene grant alive",
              "[character][scene][headless][product][canonical][cancellation]") {
        HeadlessFixture fixture;
        std::unique_ptr<Network::INetworkModeService> activeScene;
        auto physics = PreparePair(fixture, activeScene, Network::NetworkModeWorldKind::AuthorityServer, {101});
        const auto retained = Internal::InspectHeadlessGameplayContext(*physics);
        REQUIRE(retained);
        CancellationSource nextCancellation;
        fixture.load = {nextCancellation.Token(), std::chrono::steady_clock::now() + std::chrono::seconds{2}};
        nextCancellation.RequestCancellation();
        auto next = fixture.Make(Network::NetworkModeServiceKind::Scene, Network::NetworkModeWorldKind::AuthorityServer, {103});
        REQUIRE(next.HasValue());
        auto failed = std::move(next).Value();
        const auto prepared = failed->Prepare();
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == Internal::HeadlessSceneLoadErrors::Cancelled.code.Value());
        failed->Shutdown();
        CHECK(retained->Acquire("game.tests", 101, 101).HasValue());
        const auto physicsTick = physics->RunFixedTick({1, Duration::FromNanoseconds(16'666'667), {}});
        INFO((physicsTick.HasError() ? physicsTick.ErrorValue().code.Value() + ": " + physicsTick.ErrorValue().message
                                     : "physics tick succeeded"));
        REQUIRE(physicsTick.HasValue());
        physics->Shutdown();
        activeScene->Shutdown();
        CHECK(retained->Acquire("game.tests", 101, 101).HasError());
    }
#endif
}  // namespace Horo::Application::CharacterSceneProductTest
