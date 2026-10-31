#include "CommittedSceneAssets.h"
#include "EditorProjectSceneHost.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "PrefabSceneCookHostFixture.h"
#include "editor/gameplay/EditorPlaySessionController.h"

#include <chrono>
#include <functional>
#include <thread>

namespace Horo::Application::PrefabEditorProductTest {
    namespace {
        struct Observation final {
            Prefab::PrefabSpawnRequest request;
            std::shared_ptr<const Gameplay::GameplayPrefabContext> context;
            std::optional<Prefab::PrefabOperation> operation;
            std::size_t calls{};
            std::function<void(Gameplay::BehaviorContext &)> onTick;
        };

        class SpawnBehavior final : public Gameplay::IBehaviorInstance {
        public:
            explicit SpawnBehavior(Observation &observation) : observation_(observation) {}

            void OnFixedUpdate(Gameplay::BehaviorContext &context, Gameplay::FixedDeltaTime) override {
                if (observation_.onTick) {
                    observation_.onTick(context);
                    return;
                }
                ++observation_.calls;
                if (observation_.calls != 1)
                    return;
                observation_.context = context.PrefabContext();
                if (observation_.context) {
                    auto operation = observation_.context->Spawn(observation_.request);
                    REQUIRE(operation.HasValue());
                    observation_.operation = std::move(operation).Value();
                }
            }

        private:
            Observation &observation_;
        };

        Gameplay::IBehaviorInstance *Create(
            void *context) {  // NOSONAR(cpp:S5008) The behavior factory ABI supplies its typed observation through void*.
            return std::make_unique<SpawnBehavior>(*static_cast<Observation *>(context)).release();
        }

        void Destroy(void *, Gameplay::IBehaviorInstance *instance) noexcept {  // NOSONAR(cpp:S5008) Required behavior factory ABI.
            const std::unique_ptr<Gameplay::IBehaviorInstance> owned{instance};
        }

        Gameplay::BehaviorTypeId Type() {
            return Gameplay::BehaviorTypeId::Parse("game.tests.editor_prefab").Value();
        }

        Gameplay::BehaviorRegistry Registry(Observation &observation) {
            Gameplay::BehaviorRegistry registry;
            Gameplay::BehaviorDescriptor descriptor;
            descriptor.typeId = Type();
            descriptor.displayName = "Editor prefab product";
            descriptor.phases.push_back({Gameplay::BehaviorPhase::Gameplay, "game.tests.editor_prefab", {}, {}, {}});
            const auto registered = registry.Register({std::move(descriptor), {&observation, Create, Destroy}});
            REQUIRE(registered.HasValue());
            REQUIRE(registry.Freeze().HasValue());
            return registry;
        }

        /** @brief Supplies one opaque project component whose bytes must survive the real cook/load/spawn path. */
        Gameplay::SerializedComponent CustomComponent() {
            Gameplay::SerializedComponent component{.typeId = Gameplay::ComponentTypeId::Parse("game.tests.editor_data").Value(),
                                                    .schemaVersion = 2};
            for (const unsigned char byte : std::string_view{R"({"value":7,"retained":"opaque"})"})
                component.payload.push_back(static_cast<std::byte>(byte));
            return component;
        }

        Gameplay::ComponentRegistry Components() {
            Gameplay::ComponentRegistry result;
            REQUIRE(
                result.Register({.typeId = CustomComponent().typeId, .schemaVersion = 2, .displayName = "Editor data", .category = "Tests"})
                    .HasValue());
            REQUIRE(result.Freeze().HasValue());
            return result;
        }

        /** @brief Writes a real project schema shared by explicit Null and canonical product compositions. */
        void WriteCollision(const PrefabCookTestSupport::HostFixture &fixture) {
            using namespace PrefabCookTestSupport;
            const auto layer = Physics::CollisionLayerId::FromBytes(Id("00000000-0000-0000-0000-000000000001").Bytes());
            const auto profile = Physics::CollisionProfileId::FromBytes(Id("00000000-0000-0000-0000-000000000002").Bytes());
            const auto channel = Physics::PhysicsQueryChannelId::FromBytes(Id("00000000-0000-0000-0000-000000000003").Bytes());
            ProjectCollisionSettingsInput collision{.projectId = "p1"};
            collision.schema = {.defaultProfile = profile,
                                .layers = {{layer}},
                                .pairs = {{layer, layer, Physics::SimulationPairResponse::Block}},
                                .queryChannels = {{channel}},
                                .profiles = {{profile, layer, true, true, true, {{channel, Physics::CollisionQueryResponse::Block}}}}};
            collision.layers = {{layer, "World", ""}};
            collision.profiles = {{profile, "Default", ""}};
            collision.channels = {{channel, "Movement", ""}};
            auto settings = ProjectCollisionSettings::Create(std::move(collision));
            REQUIRE(settings.HasValue());
            const auto encodedCollision = EncodeProjectCollisionSettings(settings.Value());
            REQUIRE(encodedCollision.HasValue());
            fixture.WriteText(".horo/collision.json", encodedCollision.Value());
        }

        /** @brief Cooks custom members only into the explicit runtime root; the independent static Scene stays plain. */
        void WriteCustomTemplate(PrefabCookTestSupport::HostFixture &fixture, const Gameplay::ComponentRegistry &components) {
            using namespace PrefabCookTestSupport;
            auto schemas = PrefabCookSchemaContext::Capture(components, {});
            REQUIRE(schemas.HasValue());
            fixture.request.schemas = std::move(schemas).Value();
            Prefab::PrefabObjectNode root{.localId = {0}, .name = "Custom prefab root"};
            root.components.push_back({.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(), .component = CustomComponent()});
            auto document =
                Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                .assetId = fixture.prefabId,
                                                .objects = {std::move(root),
                                                            {.localId = {3}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "Child"}}},
                                               Prefab::PrefabLimitProfile::Create({}).Value());
            REQUIRE(document.HasValue());
            fixture.WriteText("assets/hierarchy.prefab", document.Value().SerializeCanonical().Value());
            const std::vector<SceneSource::SceneObjectSnapshot> objects{{.id = {900}, .name = "Static scene"}};
            fixture.WriteText("assets/level.horo", SceneSource::EncodeSceneSource({objects, {}}));
        }

        /** @brief Uses canonical registered source paths, while all prefab cooking remains in the existing real host. */
        void PrepareProject(PrefabCookTestSupport::HostFixture &fixture, Observation &observation,
                            const Gameplay::ComponentRegistry *components = nullptr) {
            using namespace PrefabCookTestSupport;
            std::filesystem::rename(fixture.project.dir.path / "assets/level.scene", fixture.project.dir.path / "assets/level.horo");
            std::filesystem::rename(fixture.project.dir.path / "assets/level.scene.horo",
                                    fixture.project.dir.path / "assets/level.horo.horo");
            const auto record = [](Assets::AssetId id, std::string_view type, std::string_view path) {
                return Assets::AssetRecord{.id = id,
                                           .type = Assets::AssetTypeId::Parse(type).Value(),
                                           .sourcePath = ProjectPath::Parse(path).Value(),
                                           .metadataPath = ProjectPath::Parse(std::string(path) + ".horo").Value()};
            };
            REQUIRE(fixture.registry
                        .Publish({TestMeshRecord(), record(fixture.prefabId, "core.prefab", "assets/hierarchy.prefab"),
                                  record(fixture.sceneId, "core.scene", "assets/level.horo")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            WriteCollision(fixture);
            if (components)
                WriteCustomTemplate(fixture, *components);
            fixture.request.runtimePrefabRoots = {fixture.prefabId};
            REQUIRE(fixture.Cook().HasValue());
            auto capture =
                Internal::CommittedSceneAssets::Capture(Horo::NativeDurableFileSystem{},
                                                        {fixture.cooked.path, fixture.request.assets.target}, fixture.registry.Snapshot());
            REQUIRE(capture.HasValue());
            auto bytes = capture.Value()->ArtifactProvider().Load(fixture.prefabId, {});
            REQUIRE(bytes.HasValue());
            observation.request.source = {fixture.prefabId, ComputeSha256(std::as_bytes(std::span(bytes.Value()))),
                                          fixture.request.assets.target};
        }

        Editor::SceneDocumentSnapshot Source() {
            Editor::SceneObjectComponentSet components;
            components.camera = Runtime::CameraComponent{};
            components.behaviors.push_back({{1}, Type(), 1, true, {}});
            return {{1}, {1}, {{.id = {1}, .name = "Camera and spawner", .components = std::move(components)}}};
        }

        /** @brief Composes the production host; an optional observer borrows its Play-owned lifetime until Stop. */
        Editor::EditorPlaySceneFactory Factory(PrefabCookTestSupport::HostFixture &fixture, bool granted, Physics::PhysicsRuntimeMode mode,
                                               Editor::EditorPlaySceneHost **captured = nullptr) {
            auto factory = Internal::MakeEditorProjectSceneFactory(fixture.jobs,
                                                                   {.physicsMode = mode,
                                                                    .cooked = Internal::SceneCookSelection{fixture.cooked.path,
                                                                                                           fixture.request.assets.target},
                                                                    .prefabGranted = granted});
            return [factory = std::move(factory), root = fixture.project.dir.path,
                    captured](Runtime::SceneRuntimeId id, const Runtime::RuntimeSceneDefinition &definition) {
                auto prepared = factory(root, id, definition);
                if (prepared.HasValue() && captured)
                    *captured = prepared.Value().get();
                return prepared;
            };
        }

        /** @brief Checks that presentation preserves both the active Play session and the pending fixed-tick deadline. */
        void RequirePendingPrefab(const Editor::EditorPlaySessionController &play, const Prefab::PrefabOperation &operation) {
            if (play.LastError())
                UNSCOPED_INFO("Play: " << play.LastError()->code.Value() << ": " << play.LastError()->message);
            if (const auto *error = operation.Failure())
                UNSCOPED_INFO(error->code.Value() << ": " << error->message);
            REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
            REQUIRE(operation.State() == Prefab::PrefabSpawnState::Pending);
        }

        template <typename Complete> void PresentUntil(Editor::EditorPlaySessionController &play, const Complete &complete) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!complete() && play.State() != Editor::EditorPlaySessionState::Failed && std::chrono::steady_clock::now() < deadline) {
                play.PresentationUpdate({0});
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (play.LastError())
                UNSCOPED_INFO(play.LastError()->message);
            REQUIRE(complete());
        }
    }  // namespace

    TEST_CASE("Actual prefab stage host loads committed assets in its separate authoring range", "[prefab][editor][product][stage]") {
        PrefabCookTestSupport::HostFixture fixture;
        Observation observation;
        PrepareProject(fixture, observation);
        auto factory =
            Internal::MakeEditorProjectPrefabStageFactory(fixture.jobs,
                                                          {.physicsMode = Physics::PhysicsRuntimeMode::Null,
                                                           .cooked = Internal::SceneCookSelection{fixture.cooked.path,
                                                                                                  fixture.request.assets.target}});
        Runtime::SceneDefinitionBuilder builder({91}, {1});
        builder.Add({.object = {1}});
        REQUIRE(builder.RequireAsset({fixture.prefabId, Assets::AssetTypeId::Parse("core.prefab").Value()}).HasValue());
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        CHECK(factory.create(fixture.project.dir.path, Editor::EditorFirstPlayScene, definition.Value()).HasError());
        auto prepared = factory.create(fixture.project.dir.path, Editor::EditorFirstPrefabStage, definition.Value());
        REQUIRE(prepared.HasValue());
        auto &host = *prepared.Value();
        CancellationSource cancellation;
        const auto token = cancellation.Token();
        REQUIRE(host.Scenes().Startup(token).HasValue());
        REQUIRE(host.Scenes().QueuePreparation(definition.Value()).HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        std::uint64_t frame = 1;
        while (!host.Scenes().ActiveScene() && std::chrono::steady_clock::now() < deadline) {
            REQUIRE(host.CommitSceneChanges({.frameNumber = frame++, .cancellation = token}).HasValue());
            REQUIRE_FALSE(host.Scenes().TakeOperationError().has_value());
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(host.Scenes().ActiveScene().has_value());
        CHECK(host.Scenes().ActiveScene()->RuntimeId() == Editor::EditorFirstPrefabStage);
        CHECK(host.Scenes().ActiveScene()->FindAsset(fixture.prefabId).has_value());
        CHECK(observation.calls == 0);
        host.Shutdown();
        CHECK_FALSE(host.Scenes().ActiveScene().has_value());
    }

    TEST_CASE("Actual Editor Play grants committed prefab loading and revokes retained clients on Stop",
              "[prefab][editor][product][committed]") {
        Physics::PhysicsRuntimeMode mode{Physics::PhysicsRuntimeMode::Null};
        SECTION("explicit Null Physics") {
            // Exercise the explicitly selected default Null composition.
        }
#if HORO_TEST_PHYSICS_NATIVE
        SECTION("explicit canonical Physics") {
            mode = Physics::PhysicsRuntimeMode::Canonical;
        }
#endif
        CAPTURE(static_cast<unsigned>(mode));
        PrefabCookTestSupport::HostFixture fixture;
        Observation observation;
        auto components = Components();
        PrepareProject(fixture, observation, &components);
        const auto registry = Registry(observation);
        Editor::EditorPlaySessionController play;
        const auto started = play.Start(Source(), registry, components, Factory(fixture, true, mode));
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().code.Value() << ": " << started.ErrorValue().message);
        REQUIRE(started.HasValue());
        components = Gameplay::ComponentRegistry{};
        REQUIRE(components.Freeze().HasValue());
        PresentUntil(play, [&play] {
            return play.State() == Editor::EditorPlaySessionState::Playing;
        });
        const auto ticked = play.FixedUpdate({}, {1.0 / 60.0});
        if (ticked.HasError())
            UNSCOPED_INFO(ticked.ErrorValue().code.Value() << ": " << ticked.ErrorValue().message);
        REQUIRE(ticked.HasValue());
        const auto receipt = play.CollisionReceipt();
        REQUIRE(receipt);
        CHECK(receipt->state == (mode == Physics::PhysicsRuntimeMode::Null ? Editor::EditorScenePhysicsState::NoPhysics
                                                                           : Editor::EditorScenePhysicsState::Active));
        CAPTURE(observation.calls, static_cast<unsigned>(play.State()));
        REQUIRE(observation.operation);
        PresentUntil(play, [&observation] {
            return observation.operation->State() != Prefab::PrefabSpawnState::Pending;
        });
        REQUIRE(observation.operation->State() == Prefab::PrefabSpawnState::Committed);
        REQUIRE(observation.operation->Spawned().HasValue());
        const auto root = play.SceneView()->Get(observation.operation->Spawned().Value().Root());
        REQUIRE(root.HasValue());
        REQUIRE(root.Value().components->gameplayComponents.size() == 1);
        CHECK(root.Value().components->gameplayComponents.front() == CustomComponent());
        const auto retained = observation.context;
        REQUIRE(retained);
        play.Stop();
        CHECK_FALSE(play.SceneView());
        CHECK(retained->Spawn(observation.request).HasError());
        CHECK(observation.operation->Spawned().HasValue());
    }

    TEST_CASE("Actual Editor Play preserves the Scene tick across prefab grant reload", "[prefab][editor][product][reload]") {
        PrefabCookTestSupport::HostFixture fixture;
        Observation observation;
        const auto components = Components();
        PrepareProject(fixture, observation, &components);
        // Keep the old request pending by contract, regardless of worker completion speed.
        observation.request.notBeforeTick = 4;
        const auto registry = Registry(observation);
        Editor::EditorPlaySceneHost *host{};
        Editor::EditorPlaySessionController play;
        REQUIRE(play.Start(Source(), registry, components, Factory(fixture, true, Physics::PhysicsRuntimeMode::Null, &host)).HasValue());
        PresentUntil(play, [&play] {
            return play.State() == Editor::EditorPlaySessionState::Playing;
        });
        REQUIRE(host);
        for (int tick = 0; tick < 3; ++tick)
            REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
        const auto retired = observation.context;
        REQUIRE(retired);
        REQUIRE(retired->CurrentTick().Value() == 3);
        REQUIRE(observation.operation);
        REQUIRE(observation.operation->State() == Prefab::PrefabSpawnState::Pending);
        auto snapshot = play.QuiesceForReload();
        REQUIRE(snapshot.HasValue());
        CHECK(retired->CurrentTick().HasError());
        CHECK(observation.operation->State() == Prefab::PrefabSpawnState::Cancelled);
        REQUIRE(play.RestoreAfterReload(registry, snapshot.Value()).HasValue());
        auto reloaded = host->PrefabServices(registry, components);
        REQUIRE(reloaded.HasValue());
        REQUIRE(reloaded.Value());
        REQUIRE(reloaded.Value()->CurrentTick().HasValue());
        CHECK(reloaded.Value()->CurrentTick().Value() == 3);
        auto nextRequest = observation.request;
        nextRequest.notBeforeTick = reloaded.Value()->CurrentTick().Value() + 1;
        auto next = reloaded.Value()->Spawn(nextRequest);
        REQUIRE(next.HasValue());
        for (int frame = 0; frame < 3; ++frame) {
            play.PresentationUpdate({0});
            RequirePendingPrefab(play, next.Value());
        }
        REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
        PresentUntil(play, [&next] {
            return next.Value().State() != Prefab::PrefabSpawnState::Pending;
        });
        if (const auto *error = next.Value().Failure())
            UNSCOPED_INFO(error->code.Value() << ": " << error->message);
        CHECK(next.Value().State() == Prefab::PrefabSpawnState::Committed);
        play.Stop();
        CHECK(reloaded.Value()->CurrentTick().HasError());
    }

    TEST_CASE("Actual Editor Play denies prefab capability independently of committed file presence",
              "[prefab][editor][product][permission]") {
        Physics::PhysicsRuntimeMode mode{Physics::PhysicsRuntimeMode::Null};
        SECTION("explicit Null Physics") {
            // Exercise the explicitly selected default Null composition.
        }
#if HORO_TEST_PHYSICS_NATIVE
        SECTION("explicit canonical Physics") {
            mode = Physics::PhysicsRuntimeMode::Canonical;
        }
#endif
        CAPTURE(static_cast<unsigned>(mode));
        PrefabCookTestSupport::HostFixture fixture;
        Observation observation;
        PrepareProject(fixture, observation);
        const auto registry = Registry(observation);
        Editor::EditorPlaySessionController play;
        const auto started = play.Start(Source(), registry, Factory(fixture, false, mode));
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().code.Value() << ": " << started.ErrorValue().message);
        REQUIRE(started.HasValue());
        PresentUntil(play, [&play] {
            return play.State() == Editor::EditorPlaySessionState::Playing;
        });
        const auto ticked = play.FixedUpdate({}, {1.0 / 60.0});
        if (ticked.HasError())
            UNSCOPED_INFO(ticked.ErrorValue().code.Value() << ": " << ticked.ErrorValue().message);
        REQUIRE(ticked.HasValue());
        const auto receipt = play.CollisionReceipt();
        REQUIRE(receipt);
        CHECK(receipt->state == (mode == Physics::PhysicsRuntimeMode::Null ? Editor::EditorScenePhysicsState::NoPhysics
                                                                           : Editor::EditorScenePhysicsState::Active));
        CHECK(observation.calls == 1);
        CHECK_FALSE(observation.context);
        CHECK_FALSE(observation.operation);
        play.Stop();
    }

    TEST_CASE("Actual Editor Play rejects a cooked custom schema absent from the Scene metadata snapshot",
              "[prefab][editor][product][schema][rollback]") {
        PrefabCookTestSupport::HostFixture fixture;
        Observation observation;
        const auto cookedComponents = Components();
        PrepareProject(fixture, observation, &cookedComponents);
        const auto registry = Registry(observation);
        Gameplay::ComponentRegistry missingComponents;
        REQUIRE(missingComponents.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        REQUIRE(play.Start(Source(), registry, missingComponents, Factory(fixture, true, Physics::PhysicsRuntimeMode::Null)).HasValue());
        PresentUntil(play, [&play] {
            return play.State() == Editor::EditorPlaySessionState::Playing;
        });
        const auto revision = play.SceneView()->StructuralRevision();
        REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
        REQUIRE(observation.operation);
        PresentUntil(play, [&observation] {
            return observation.operation->State() != Prefab::PrefabSpawnState::Pending;
        });
        CHECK(observation.operation->State() == Prefab::PrefabSpawnState::Failed);
        CHECK(observation.operation->Failure() != nullptr);
        CHECK(play.SceneView()->StructuralRevision() == revision);
        CHECK(play.SceneView()->SlotCount() == 1);
        CHECK(play.SceneView()->Find({1}).has_value());
    }

    TEST_CASE("New Editor prefab hosts capture project depth while existing hosts retain their admitted policy",
              "[prefab][editor][product][settings]") {
        PrefabCookTestSupport::HostFixture fixture;
        Observation observation;
        const auto registry = Registry(observation);
        const auto components = Components();
        const std::array descriptors{registry.Registrations().front().descriptor};
        const auto schemas = PrefabCookSchemaContext::Capture(components, descriptors);
        REQUIRE(schemas.HasValue());
        fixture.request.schemas = schemas.Value();
        auto source = Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                      .assetId = fixture.prefabId,
                                                      .objects = {{.localId = {0},
                                                                   .name = "Runtime policy child",
                                                                   .behaviors = {{{1}, Type(), 1, true, {}}}}}},
                                                     Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(source.HasValue());
        fixture.WriteText("assets/hierarchy.prefab", source.Value().SerializeCanonical().Value());
        const auto writeDepth = [&](const std::int64_t depth) {
            auto values = PrefabPolicyFieldValues({});
            values[static_cast<std::size_t>(PrefabLimitField::RuntimeSpawnDepth)] = depth;
            const auto settings = PrefabProjectSettings::Create(values);
            REQUIRE(settings.HasValue());
            fixture.WriteText(".horo/prefab.json", settings.Value().Configuration().ToJson());
        };
        writeDepth(2);
        PrepareProject(fixture, observation);
        const auto secondId = PrefabCookTestSupport::Id("00000000-0000-0000-0000-0000000000b2");
        auto secondSource = Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                            .assetId = secondId,
                                                            .objects = {{.localId = {0}, .name = "Second runtime target"}}},
                                                           Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(secondSource.HasValue());
        fixture.WriteText("assets/second.prefab", secondSource.Value().SerializeCanonical().Value());
        fixture.WriteText("assets/second.prefab.horo", PrefabCookTestSupport::SidecarJson(secondId.ToString(), "core.prefab"));
        const auto snapshot = fixture.registry.Snapshot();
        const auto records = snapshot.Records();
        std::vector<Assets::AssetRecord> expandedRecords{records.begin(), records.end()};
        expandedRecords.push_back({.id = secondId,
                                   .type = Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   .sourcePath = ProjectPath::Parse("assets/second.prefab").Value(),
                                   .metadataPath = ProjectPath::Parse("assets/second.prefab.horo").Value()});
        REQUIRE(fixture.registry.Publish(std::move(expandedRecords)).status == Assets::AssetRegistryBuildStatus::Complete);
        fixture.request.runtimePrefabRoots.push_back(secondId);
        REQUIRE(fixture.Cook().HasValue());
        auto committed =
            Internal::CommittedSceneAssets::Capture(NativeDurableFileSystem{}, {fixture.cooked.path, fixture.request.assets.target},
                                                    fixture.registry.Snapshot());
        REQUIRE(committed.HasValue());
        auto firstBytes = committed.Value()->ArtifactProvider().Load(fixture.prefabId, {});
        REQUIRE(firstBytes.HasValue());
        observation.request.source.artifactDigest = ComputeSha256(std::as_bytes(std::span(firstBytes.Value())));
        auto secondBytes = committed.Value()->ArtifactProvider().Load(secondId, {});
        REQUIRE(secondBytes.HasValue());
        const Prefab::PrefabTemplateLoadRequest secondTarget{secondId, ComputeSha256(std::as_bytes(std::span(secondBytes.Value()))),
                                                             fixture.request.assets.target};
        const auto descendant = [&](Editor::EditorPlaySessionController &play) {
            observation.operation.reset();
            std::shared_ptr<const Gameplay::GameplayPrefabContext> inherited;
            observation.onTick = [&](Gameplay::BehaviorContext &context) {
                if (!observation.operation) {
                    auto started = context.PrefabContext()->Spawn(observation.request);
                    REQUIRE(started.HasValue());
                    observation.operation = std::move(started).Value();
                } else if (observation.operation->State() == Prefab::PrefabSpawnState::Committed &&
                           context.Entity().index == observation.operation->Spawned().Value().Root().entity.index)
                    inherited = context.PrefabContext();
            };
            REQUIRE(play.Start(Source(), registry, components, Factory(fixture, true, Physics::PhysicsRuntimeMode::Null)).HasValue());
            PresentUntil(play, [&] {
                return play.State() == Editor::EditorPlaySessionState::Playing;
            });
            REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
            REQUIRE(observation.operation);
            PresentUntil(play, [&] {
                return observation.operation->State() != Prefab::PrefabSpawnState::Pending;
            });
            if (const auto *failure = observation.operation->Failure())
                INFO(failure->code.Value() << ": " << failure->message);
            REQUIRE(observation.operation->State() == Prefab::PrefabSpawnState::Committed);
            REQUIRE(play.FixedUpdate({}, {1.0 / 60.0}).HasValue());
            REQUIRE(inherited);
            observation.onTick = {};
            return inherited;
        };
        Editor::EditorPlaySessionController oldPlay;
        const auto retained = descendant(oldPlay);
        writeDepth(1);
        auto another = observation.request;
        // A separately cooked target proves policy admission independently of recursion and missing-source rejection.
        another.source = secondTarget;
        another.notBeforeTick = retained->CurrentTick().Value();
        auto admitted = retained->Spawn(another);
        REQUIRE(admitted.HasValue());
        admitted.Value().Cancel();
        oldPlay.Stop();
        CHECK(retained->Spawn(another).HasError());
        Editor::EditorPlaySessionController newPlay;
        const auto current = descendant(newPlay);
        another.notBeforeTick = current->CurrentTick().Value();
        const auto rejected = current->Spawn(another);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Prefab::PrefabErrors::SpawnDepthExceeded.code.Value());
        newPlay.Stop();
    }
}  // namespace Horo::Application::PrefabEditorProductTest
