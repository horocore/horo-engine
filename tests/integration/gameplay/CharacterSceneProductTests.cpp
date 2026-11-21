#include "../../helpers/ProjectReplicationTestSupport.h"
#include "CharacterSceneProductFixture.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "GameplayReplicationAdmissionTestSupport.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "HeadlessProjectScene.h"
#include "HeadlessReplayArtifact.h"
#include "Horo/AI/AITaskContinuation.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Animation/AnimationErrors.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Platform/ExternalProcess.h"
#include "NetworkProductHost.h"
#include "navigation/ProjectSceneNavigationFixture.h"

#if HORO_TEST_NAVIGATION_NATIVE && HORO_TEST_PHYSICS_NATIVE
#include "../network/NetworkModeHostTestSupport.h"
#include "AnimationStateGraphTestFixtures.h"
#include "animation/AnimationCookFixture.h"
#endif

#include <bit>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Application::CharacterSceneProductTest {
    namespace {
        // The >1 MiB/8192-object fixture also runs under parallel MSVC Debug CI.
        // Keep startup bounded independently of the child process's cleanup budget.
        constexpr auto SceneLoadTimeout = std::chrono::seconds{60};
        constexpr auto CliProcessTimeout = SceneLoadTimeout + std::chrono::seconds{30};

        ExternalProcessResult RunCli(std::vector<std::string> arguments, const std::string_view expectedDiagnostic = {}) {
            NativeExternalProcessRunner runner;
            std::string output;
            ExternalProcessRequest request{.executable = HORO_TEST_CHARACTER_CLI_PATH,
                                           .arguments = std::move(arguments),
                                           .timeout = CliProcessTimeout,
                                           .onOutput = [&output](ProcessOutputLine line) {
                output += line.text + '\n';
            }};
            const auto result = runner.Run(request, {});
            UNSCOPED_INFO(output);
            if (result.HasError())
                UNSCOPED_INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().reason == ProcessTerminationReason::Exited);
            if (!expectedDiagnostic.empty())
                CHECK(output.find(expectedDiagnostic) != std::string::npos);
            UNSCOPED_INFO(output);
            return result.Value();
        }

        std::string Utf8Path(const std::filesystem::path &path) {
            const auto value = path.generic_u8string();
            return {value.begin(), value.end()};
        }

        std::vector<std::string> SceneArguments(const Fixture &fixture, const std::string &scene, const std::string &mode) {
            return {"--run-network-product",
                    "standalone",
                    "2",
                    "--project",
                    Utf8Path(fixture.project.project.dir.path),
                    "--scene",
                    scene,
                    "--physics",
                    mode,
                    "--cooked-root",
                    Utf8Path(fixture.project.cooked.path),
                    "--cook-target",
                    fixture.project.request.assets.target.Value(),
                    "--load-timeout-ms",
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(SceneLoadTimeout).count())};
        }

        std::vector<std::string> NavigationArguments(const NavigationProductFixture &fixture) {
            return {"--run-network-product",
                    "standalone",
                    "2",
                    "--project",
                    Utf8Path(fixture.project.project.dir.path),
                    "--scene",
                    "cooked:" + fixture.project.sceneId.ToString(),
                    "--cooked-root",
                    Utf8Path(fixture.project.cooked.path),
                    "--cook-target",
                    fixture.project.request.assets.target.Value(),
                    "--navigation",
                    "recast-detour"};
        }

        /** @brief Reads the actual generated fixture revision instead of substituting a module identity. */
        Internal::GameplayWorldSelection NavigationGameplay() {
            Internal::GameplayWorldSelection gameplay;
            gameplay.moduleId = "game.tests";
            gameplay.nativeArtifact = HORO_TEST_GAME_MODULE_PATH;
            gameplay.moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
            std::ifstream input{HORO_TEST_GAME_MODULE_REVISION_PATH};
            REQUIRE((input >> gameplay.descriptorRevision));
            return gameplay;
        }

#if HORO_TEST_NAVIGATION_NATIVE
        /** @brief Pairs the real Scene and Physics World facades and preserves production shutdown order on test failure. */
        struct NativeNavigationProductWorld final {
            std::unique_ptr<Internal::HeadlessProjectSceneResources> resources;
            std::optional<Internal::NetworkProductComposition> composition;
            std::unique_ptr<Network::INetworkModeService> scene;
            std::unique_ptr<Network::INetworkModeService> physics;
            DeterministicClock clock;
            std::unique_ptr<Internal::NetworkProductHost> scheduled;
            Network::INetworkModeService *scheduledScene{};
            Network::INetworkModeService *scheduledPhysics{};
            bool stopped{};
            std::uint64_t tick{};

            Result<void> Start(const NavigationProductFixture &fixture, const Internal::GameplayWorldSelection &gameplay,
                               std::vector<Internal::HeadlessNavigationActorPolicy> actors = {},
                               Physics::PhysicsRuntimeMode mode = Physics::PhysicsRuntimeMode::Null,
                               const std::function<void(Internal::HeadlessSceneActivationInput &,
                                                        Internal::HeadlessProjectSceneResources &)> &configure = {},
                               Network::NetworkProjectRole role = Network::NetworkProjectRole::Standalone,
                               std::optional<Internal::HeadlessSimulationAssets> simulationAssets = std::nullopt) {
                const Internal::HeadlessProjectSceneOptions options{.projectRoot = fixture.project.project.dir.path,
                                                                    .scene = "cooked:" + fixture.project.sceneId.ToString(),
                                                                    .cookedRoot = fixture.project.cooked.path,
                                                                    .cookTarget = fixture.project.request.assets.target.Value(),
                                                                    .physicsMode = mode,
                                                                    .navigationMode = Internal::HeadlessNavigationMode::RecastDetour,
                                                                    .simulationAssets = std::move(simulationAssets)};
                auto prepared = Internal::PrepareHeadlessProjectSceneResources(options);
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                resources = std::move(prepared).Value();
                auto &project = *resources;
                const auto nativePrefabs = gameplay.prefabPermission == Internal::GameplayPrefabPermission::Granted;
                Internal::HeadlessSceneActivationInput input{.mode = options.physicsMode,
                                                             .settings = *project.settings,
                                                             .registry = project.registry,
                                                             .loads = *project.loads,
                                                             .load = project.load,
                                                             .prefabLoads = nativePrefabs ? project.prefabLoads.get() : nullptr,
                                                             .navigation = project.navigation,
                                                             .navigationPath =
                                                                 Internal::HeadlessNavigationPathPolicy{fixture.pathFilter, 512},
                                                             .navigationActors = std::move(actors)};
                if (configure)
                    configure(input, project);
                Network::NetworkModeFactories factories;
                std::optional<Gameplay::GameplayReplicationAdmissionPolicy> admission;
#if HORO_TEST_PHYSICS_NATIVE
                const auto owners =
                    role == Network::NetworkProjectRole::DedicatedServer ? std::make_shared<Network::ProductOwners>() : nullptr;
                if (owners) {
                    admission.emplace();
                    admission->inventory.completeness = Network::NetworkReplicationInventoryCompleteness::Complete;
                    factories = Network::ProductFactories(owners);
                }
#endif
                auto configured = Internal::ComposeHeadlessSceneNetworkServices(project.definition, std::move(input), std::move(factories),
                                                                                gameplay, admission);
                if (configured.HasError())
                    return Result<void>::Failure(std::move(configured).ErrorValue());
#if HORO_TEST_PHYSICS_NATIVE
                if (owners || configured.Value().fixedParticipant)
                    return StartScheduled(std::move(configured).Value(), role, owners);
#endif
                REQUIRE(role == Network::NetworkProjectRole::Standalone);
                composition.emplace(std::move(configured).Value());
                const auto makeService = [&](const Network::NetworkModeServiceKind kind) {
                    auto service = composition->factories.services[static_cast<std::size_t>(kind)](
                        {kind, Network::NetworkProjectRole::Standalone, {}, Network::NetworkModeWorldKind::Standalone, {31}});
                    REQUIRE(service.HasValue());
                    return std::move(service).Value();
                };
                scene = makeService(Network::NetworkModeServiceKind::Scene);
                if (auto preparedScene = scene->Prepare(); preparedScene.HasError())
                    return preparedScene;
                physics = makeService(Network::NetworkModeServiceKind::Physics);
                if (auto preparedPhysics = physics->Prepare(); preparedPhysics.HasError())
                    return preparedPhysics;
                if (auto activated = scene->Activate(); activated.HasError())
                    return activated;
                return physics->Activate();
            }

            Network::INetworkModeService &PhysicsService() const {
                return *(scheduled ? scheduledPhysics : physics.get());
            }

            Network::INetworkModeService &SceneService() const {
                return *(scheduled ? scheduledScene : scene.get());
            }

#if HORO_TEST_PHYSICS_NATIVE
            Result<void> StartScheduled(Internal::NetworkProductComposition composed, Network::NetworkProjectRole role,
                                        const std::shared_ptr<Network::ProductOwners> &owners) {
                using namespace Network;
                const auto retain = [&](NetworkModeServiceKind kind, INetworkModeService *&destination) {
                    auto &factory = composed.factories.services[static_cast<std::size_t>(kind)];
                    factory = [create = std::move(factory), &destination](const NetworkModeServiceRequest &request) {
                        auto created = create(request);
                        if (created.HasValue())
                            destination = created.Value().get();
                        return created;
                    };
                };
                retain(NetworkModeServiceKind::Scene, scheduledScene);
                retain(NetworkModeServiceKind::Physics, scheduledPhysics);
                auto settings = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(10).Value()).Value();
                if (owners) {
                    settings.supportedRoles = settings.supportedRoles | NetworkProjectRoleSet::DedicatedServer;
                    settings.transport.requirement = NetworkProjectTransportRequirement::Required;
                    settings.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
                    settings.transport.capabilities.requiredChannels = 2;
                    settings.transport.capabilities.requiredMaximumMessageBytes = 1024;
                    settings.replication.completeness = NetworkReplicationInventoryCompleteness::Complete;
                }
                const auto project = NetworkProjectSettings::Create(settings);
                if (project.HasError())
                    return Result<void>::Failure(project.ErrorValue());
                const auto facts = ProductFacts(project.Value(), role, false);
                const auto kind = owners ? NetworkModeWorldKind::AuthorityServer : NetworkModeWorldKind::Standalone;
                const std::array worlds{NetworkModeWorld{kind, {31}}};
                auto host = Internal::NetworkProductHost::Create(clock,
                                                                 {project.Value(),
                                                                  facts.product,
                                                                  facts.inventory,
                                                                  facts.host,
                                                                  facts.requirements,
                                                                  facts.selection,
                                                                  6,
                                                                  worlds,
                                                                  {},
                                                                  owners ? owners->sessions : nullptr,
                                                                  NetworkWorldCompositionPurpose::Gameplay,
                                                                  owners ? std::optional{owners->worldSetup} : std::nullopt,
                                                                  std::move(composed.fixedParticipant),
                                                                  std::move(composed.pollRetirement),
                                                                  {},
                                                                  std::move(composed.aiHealth)},
                                                                 std::move(composed.factories));
                if (host.HasError())
                    return Result<void>::Failure(std::move(host).ErrorValue());
                scheduled = std::move(host).Value();
                if (auto started = scheduled->Startup(); started.HasError())
                    return started;
                if (auto firstFrame = scheduled->RunFrame(); firstFrame.HasError())
                    return firstFrame;
                REQUIRE(scheduled->Role(kind, worlds.front().scene).HasValue());
                return Result<void>::Success();
            }
#endif

            void RunFrame() {
                if (scheduled) {
                    ++tick;
                    clock.Advance(Duration::FromNanoseconds(16'666'667));
                    const auto frame = scheduled->RunFrame();
                    if (frame.HasError())
                        UNSCOPED_INFO(frame.ErrorValue().code.Value() + ": " + frame.ErrorValue().message);
                    REQUIRE(frame.HasValue());
                    return;
                }
                const auto cancellation = resources->cancellation.Token();
                const auto result = physics->RunFixedTick({++tick, Duration::FromNanoseconds(16'666'667), cancellation});
                if (result.HasError())
                    UNSCOPED_INFO(result.ErrorValue().message);
                REQUIRE(result.HasValue());
                const auto commit = physics->RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges);
                if (commit.HasError())
                    UNSCOPED_INFO(commit.ErrorValue().message);
                REQUIRE(commit.HasValue());
            }

            void Await(const Prefab::PrefabOperation &operation, const Prefab::PrefabSpawnState expected) {
                const auto deadline = tick + 64;
                while (operation.State() == Prefab::PrefabSpawnState::Pending && tick < deadline) {
                    RunFrame();
                    if (operation.State() == Prefab::PrefabSpawnState::Pending)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (const auto failure = operation.Failure())
                    UNSCOPED_INFO(failure->message);
                REQUIRE(operation.State() == expected);
            }

            ~NativeNavigationProductWorld() {
                Stop();
            }

            void Stop() {
                if (std::exchange(stopped, true))
                    return;
                if (scheduled)
                    scheduled->Shutdown();
                if (physics)
                    physics->Shutdown();
                if (scene)
                    scene->Shutdown();
                if (resources) {
                    resources->cancellation.RequestCancellation();
                    if (resources->prefabLoads)
                        resources->prefabLoads->Shutdown();
                    resources->loads->Shutdown();
                    resources->jobs.Shutdown(ShutdownPolicy::Cancel);
                }
                scheduled.reset();
            }
        };
#endif

    }  // namespace

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Actual terminal CLI activates authored and committed cooked Character Scenes",
              "[character][scene][cli][product][canonical]") {
        const auto extraObjects = GENERATE(std::size_t{0}, std::size_t{8192});
        CAPTURE(extraObjects);
        Fixture fixture{extraObjects};
        if (extraObjects != 0)
            REQUIRE(std::filesystem::file_size(fixture.project.project.dir.path / "assets/level.horo") > (1U << 20U));
        CHECK(RunCli(SceneArguments(fixture, "source:assets/level.horo", "canonical")).exitCode == 0);
        CHECK(RunCli(SceneArguments(fixture, "cooked:" + fixture.project.sceneId.ToString(), "canonical")).exitCode == 0);
    }
#endif

    TEST_CASE("Explicit Null terminal Scene host refuses enabled Character without fallback", "[character][scene][cli][product][null]") {
        Fixture fixture;
        CHECK(RunCli(SceneArguments(fixture, "source:assets/level.horo", "null")).exitCode == 3);
    }

    TEST_CASE("Terminal Navigation startup uses the exact selected cooked scope and project policy", "[navigation][scene][cli][product]") {
        NavigationProductFixture fixture;
        using Input = NavigationProductFixture::Input;
        SECTION("valid world publishes and shuts down") {
            fixture.Publish(Input::Valid);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == (HORO_TEST_NAVIGATION_NATIVE ? 0 : 3));
        }
        SECTION("selected policy requires an actual world") {
            fixture.Publish(Input::MissingSurface);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
        SECTION("foreign Scene scope cannot activate the artifact") {
            fixture.Publish(Input::ForeignScope);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
        SECTION("stale cooked authority cannot publish") {
            fixture.Publish(Input::StalePolicy);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
        SECTION("unsupported authored links cannot disappear from activation") {
            fixture.Publish(Input::UnsupportedLink);
            CHECK(RunCli(NavigationArguments(fixture)).exitCode == 3);
        }
    }

    TEST_CASE("Terminal native Navigation path requires its explicit filter and deadline", "[navigation][scene][cli][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Scene);
        const auto gameplay = NavigationGameplay();
        auto arguments = NavigationArguments(fixture);
        arguments.insert(arguments.end(), {"--game-module", Utf8Path(gameplay.nativeArtifact), gameplay.moduleId,
                                           std::to_string(gameplay.descriptorRevision), "deny", "--game-navigation-path"});
        auto missing = arguments;
        CHECK(RunCli(std::move(missing)).exitCode == 2);
        const auto filter = std::to_string(fixture.pathFilter.Value());
        auto unlimited = arguments;
        unlimited.insert(unlimited.end(), {filter, std::to_string(Internal::HeadlessNavigationPathPolicy::MaximumDeadlineTicks + 1)});
        CHECK(RunCli(std::move(unlimited)).exitCode == 2);
        auto unknown = arguments;
        unknown.insert(unknown.end(), {std::to_string(fixture.pathFilter.Value() + 1), "32"});
        const auto reason = HORO_TEST_NAVIGATION_NATIVE ? Navigation::NavigationErrors::FilterUnknown.summary : std::string_view{};
        CHECK(RunCli(std::move(unknown), reason).exitCode == 3);
        arguments.insert(arguments.end(), {filter, "32"});
        CHECK(RunCli(std::move(arguments)).exitCode == (HORO_TEST_NAVIGATION_NATIVE ? 0 : 3));
    }

#if HORO_TEST_NAVIGATION_NATIVE
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        namespace DecisionFixture = AI::DecisionGraphTestSupport;
        constexpr std::array<std::uint8_t, 16> PreviewTextureId{104};

        /** @brief Adds generated Animation artifacts with their cooker-issued dependency identities. */
        Animation::AnimationAuthoringComponent AddMotionGraph(NavigationProductFixture &project,
                                                              std::vector<NavigationProductFixture::AddedAsset> &assets,
                                                              const Animation::GraphParameterId trigger) {
            using namespace Animation;
            auto graph = Test::StateGraph::Data();
            auto &machine = Test::StateGraph::Machine(graph);
            machine.states.resize(2);
            machine.transitions.resize(1);
            machine.transitions.front().duration = {0};
            machine.transitions.front().conditions = {{trigger, GraphStatePredicate::Trigger, true}};
            Test::Instance::Prepared source(graph);
            std::vector<AnimationClipAsset> clips;
            for (const auto &authored : source.compileClips) {
                auto data = authored.Data();
                data.descriptor.duration = {AnimationTicksPerSecond};
                for (auto &track : data.tracks)
                    track.keys.back().time = {data.descriptor.duration.ticks};
                clips.push_back(Test::Cook::Take(AnimationClipAsset::Create(std::move(data), *source.skeleton)));
            }
            Test::Cook::Fixture cook;
            REQUIRE(cook.request.target == project.project.request.assets.target);
            cook.Replace(*source.skeleton, clips, graph);
            const auto output = cook.Contents(Test::Cook::Take(cook.Cook()));
            const auto registry = cook.registry.Snapshot();
            for (std::size_t index = 0; index < output.entries.size(); ++index) {
                const auto found = std::ranges::find(registry.Records(), output.entries[index].assetId, &Assets::AssetRecord::id);
                REQUIRE(found != registry.Records().end());
                auto record = *found;
                const std::filesystem::path authored{record.sourcePath.String()};
                auto relative = authored;
                relative.replace_extension(".horoasset");
                std::filesystem::copy_file(cook.source.path / authored, project.project.project.dir.path / relative);
                const auto sourcePath = relative.generic_string();
                project.project.WriteText(sourcePath + ".horo", SidecarJson(record.id.ToString(), record.type.Value()));
                record.sourcePath = ProjectPath::Parse(sourcePath).Value();
                record.metadataPath = ProjectPath::Parse(sourcePath + ".horo").Value();
                assets.push_back({std::move(record), {}, output.artifacts[index]});
            }
            return {.component = Test::Id<AnimationComponentId>(7),
                    .skeleton = source.skeleton->Data().skeleton,
                    .source = {AnimationSourceKind::Graph, {}, graph.id}};
        }

        /** @brief Adds the authored collision policy and committed material used by the native Character fixture. */
        NavigationProductFixture::AddedAsset AddCharacterCollisionContent(NavigationProductFixture &navigation, const Fixture &character) {
            const auto root = navigation.project.project.dir.path;
            const auto characterRoot = character.project.project.dir.path;
            for (const auto *relative :
                 {".horo/collision.json", ".horo/surfaces.json", "assets/controller.horoasset", "assets/controller.horoasset.horo"})
                std::filesystem::copy_file(characterRoot / relative, root / relative, std::filesystem::copy_options::overwrite_existing);
            std::ifstream materialBytes(root / "assets/controller.horoasset", std::ios::binary);
            REQUIRE(materialBytes.is_open());
            return {Fixture::Record(character.materialId, "core.physics_material", "assets/controller.horoasset"),
                    std::vector<std::uint8_t>{std::istreambuf_iterator<char>{materialBytes}, {}}};
        }

        /** @brief Adds the canonical Character geometry and the authored decision closure to the real cooked Navigation project. */
        Assets::AssetId PublishDecisionMotionProject(NavigationProductFixture &navigation, DecisionFixture::Fixture &decision,
                                                     const AI::DecisionGraphSource &source, const AI::AiControllerDescriptor &controller,
                                                     std::optional<Animation::GraphParameterId> trigger) {
            Fixture character;
            auto &project = navigation.project;
            const auto root = project.project.dir.path;
            auto material = AddCharacterCollisionContent(navigation, character);
            const auto plan = decision.Compile(std::span{&source, 1}, source.asset.Value());
            const auto payload = AI::DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(payload.HasValue());
            const auto authored = AI::DecisionPlanCodec::EncodeSource(std::span{&source, 1}, source.asset);
            REQUIRE(authored.HasValue());
            const auto asset = Assets::AssetId::FromBytes({103});
            const auto type = AI::DecisionAssetType(AI::DecisionPlanKind::BehaviorTree);
            WriteFile(root / "assets/move.horo_bt", authored.Value());
            project.WriteText("assets/move.horo_bt.horo", SidecarJson(asset.ToString(), type));
            std::vector added{std::move(material),
                              NavigationProductFixture::AddedAsset{Fixture::Record(asset, type, "assets/move.horo_bt"), payload.Value()}};
            const auto texture = Assets::AssetId::FromBytes(PreviewTextureId);
            constexpr std::string_view preview = "assets/preview.horoasset";
            const std::vector<std::uint8_t> pixels{0, 128, 255, 255};
            WriteFile(root / preview, pixels);
            project.WriteText(std::string{preview} + ".horo", SidecarJson(texture.ToString(), "core.texture"));
            added.push_back({Fixture::Record(texture, "core.texture", preview), pixels});
            const auto animation = trigger ? std::optional{AddMotionGraph(navigation, added, *trigger)} : std::nullopt;
            navigation.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::None, added,
                               [&](SceneSource::SceneSourceDocument &scene) {
                scene.objects.front().localTransform.translation = {3, 0, 2};
                auto &actor = scene.objects[2];
                actor.localTransform.translation = {1, 0.495F, 1};
                actor.components.characterController = character.source.objects[0].components.characterController;
                actor.components.animation = animation;
                actor.components.aiAgent = AI::AiAgentComponent{.agent = DecisionFixture::MakeIdentity<AI::AgentId>(50)};
                actor.components.aiController = AI::AiControllerComponent{.controller = controller.controller,
                                                                          .decisionAsset = source.asset,
                                                                          .blackboardSchema = decision.schema->Identity()};
                auto &floor = scene.objects[1].components;
                floor.rigidBody = character.source.objects[1].components.rigidBody;
                floor.colliders = character.source.objects[1].components.colliders;
            });
            return asset;
        }
    }  // namespace

    TEST_CASE("Cooked tactical point queries combine Navigation cost with Physics visibility",
              "[ai][navigation][character][product][eqs]") {
        using namespace AI;
        using DecisionFixture::MakeIdentity;
        enum class Scenario {
            Reachability,
            Cost,
            VisibilityFirst,
            NavigationFirst
        };
        const auto scenario = GENERATE(Scenario::Reachability, Scenario::Cost, Scenario::VisibilityFirst, Scenario::NavigationFirst);
        const bool scoreCost = scenario != Scenario::Reachability;
        const bool mixed = scenario == Scenario::VisibilityFirst || scenario == Scenario::NavigationFirst;
        std::optional<Fixture> character;
        if (mixed)
            character.emplace();
        const auto agent = MakeIdentity<AgentId>(50);
        const auto controller = MakeIdentity<ControllerTypeId>(49);
        const auto key = MakeIdentity<BlackboardKeyId>(4);
        const std::array keys{
            BlackboardKeyDescriptor{.key = key, .kind = BlackboardValueKind::WorldCoordinate, .presence = BlackboardKeyPresence::Optional}};
        auto schema = BlackboardSchema::Capture({.identity = DecisionFixture::SchemaId(), .keys = keys});
        REQUIRE(schema.HasValue());
        const auto blackboardSchema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());

        auto registry = CaptureBuiltinQuerySchema();
        REQUIRE(registry.HasValue());
        auto generator = CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(1), MakeIdentity<QueryStageId>(1),
                                                   MakeIdentity<QueryResultSchemaId>(1), "Tactical path",
                                                   {.stepMillimeters = scoreCost ? 500u : 4'000u,
                                                    .outerSteps = 1,
                                                    .innerSteps = scoreCost ? std::nullopt : std::optional<std::uint32_t>{1}},
                                                   1);
        REQUIRE(generator.HasValue());
        if (scenario == Scenario::VisibilityFirst) {
            generator = AddBuiltinPointVisibilityTest(generator.Value(), MakeIdentity<QueryStageId>(7), character->channel.Bytes());
            REQUIRE(generator.HasValue());
        }
        auto query =
            AddBuiltinPointNavigationTest(generator.Value(), MakeIdentity<QueryStageId>(2), scoreCost ? std::optional{10.0} : std::nullopt);
        REQUIRE(query.HasValue());
        if (scenario == Scenario::NavigationFirst)
            query = AddBuiltinPointVisibilityTest(query.Value(), MakeIdentity<QueryStageId>(7), character->channel.Bytes());
        REQUIRE(query.HasValue());
        auto queryBytes = EnvironmentQueryCookedCodec::Encode(query.Value(), registry.Value());
        REQUIRE(queryBytes.HasValue());

        Internal::GameplayWorldSelection gameplay;
        gameplay.moduleId = "game.ai.tests";
        gameplay.nativeArtifact = HORO_TEST_AI_MODULE_PATH;
        gameplay.moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
        std::ifstream revision{HORO_TEST_AI_MODULE_REVISION_PATH};
        REQUIRE((revision >> gameplay.descriptorRevision));
        auto image =
            Gameplay::GameModuleHost::PrepareImage(gameplay.nativeArtifact, {gameplay.moduleId, Gameplay::CurrentGameplayBuildFingerprint(),
                                                                             gameplay.descriptorRevision});
        REQUIRE(image.HasValue());
        auto cookImage = std::move(image).Value();

        auto task = DecisionFixture::TreeNode(2, DecisionControlKind::Task);
        task.type = MakeIdentity<BehaviorTreeNodeTypeId>(102);
        task.requirements = {{.key = key,
                              .kind = BlackboardValueKind::WorldCoordinate,
                              .access = BlackboardKeyAccess::ReadWrite,
                              .source = DecisionFixture::Location()}};
        const auto &result = query.Value().ResultSchema();
        task.properties.emplace_back(MakeIdentity<BehaviorTreePropertyId>(10), "output", BehaviorTreePropertyKind::QueryOutput,
                                     BehaviorTreeQueryOutput{query.Value().Id(), result.id, result.version, result.itemType, key, 120});
        auto graph = DecisionFixture::Tree(11, {DecisionFixture::TreeNode(1, DecisionControlKind::Root), std::move(task)},
                                           {DecisionFixture::Edge(1, 1, 2)});
        graph.executionTask = MakeIdentity<TaskId>(9000);
        auto nodes = cookImage->AcquireDecisionNodes();
        REQUIRE(nodes.HasValue());
        auto compiled =
            DecisionGraphCompiler::Compile(graph.asset, std::span{&graph, 1}, {std::move(nodes).Value(), {blackboardSchema}, {}, 1});
        REQUIRE(compiled.HasValue());
        REQUIRE(compiled.Value().plan);
        auto graphBytes = DecisionPlanCodec::EncodeCooked(*compiled.Value().plan);
        cookImage.reset();
        if (graphBytes.HasError())
            UNSCOPED_INFO(graphBytes.ErrorValue().code.Value() << ": " << graphBytes.ErrorValue().message);
        REQUIRE(graphBytes.HasValue());
        auto schemaBytes = BlackboardSchemaCodec::Encode(*blackboardSchema);
        REQUIRE(schemaBytes.HasValue());

        NavigationProductFixture fixture;
        const auto add = [&](const std::uint8_t serial, const std::string_view type, const std::string &path,
                             std::vector<std::uint8_t> bytes) {
            const auto id = Assets::AssetId::FromBytes({serial});
            fixture.project.WriteText(path, "Cooked fixture source");
            fixture.project.WriteText(path + ".horo", SidecarJson(id.ToString(), type));
            return NavigationProductFixture::AddedAsset{{id, Type(type), ProjectPath::Parse(path).Value(),
                                                         ProjectPath::Parse(path + ".horo").Value()},
                                                        std::move(bytes)};
        };
        std::vector assets{add(120, DecisionAssetType(DecisionPlanKind::BehaviorTree), "assets/path.horo_bt",
                               std::move(graphBytes).Value()),
                           add(121, BlackboardSchemaAssetType, "assets/path.horo_bb", std::move(schemaBytes).Value()),
                           add(122, EnvironmentQuerySourceAssetType, "assets/path.horo_eqs", std::move(queryBytes).Value())};
        std::optional<Runtime::SceneAssetDependency> materialDependency;
        if (character) {
            assets.push_back(AddCharacterCollisionContent(fixture, *character));
            materialDependency.emplace(character->materialId, Type("core.physics_material"));
        }
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::None, assets, [&](auto &source) {
            auto &actor = source.objects.front();
            actor.localTransform.translation = {1, 0, 1};
            actor.components.aiAgent = AiAgentComponent{.agent = agent};
            actor.components.aiController = AiControllerComponent{.controller = controller,
                                                                  .decisionAsset = graph.asset,
                                                                  .blackboardSchema = blackboardSchema->Identity()};
            if (character) {
                auto blocker = character->source.objects[1];
                blocker.id = {4};
                blocker.name = "Tactical visibility blocker";
                blocker.localTransform.translation = {1.25F, 0, 1.25F};
                auto &collider = blocker.components.colliders.front();
                collider.body.object = {blocker.id.value};
                collider.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{0.08F}};
                source.objects.push_back(std::move(blocker));
            }
        }, materialDependency);

        NativeNavigationProductWorld product;
        const auto mode = mixed ? Physics::PhysicsRuntimeMode::Canonical : Physics::PhysicsRuntimeMode::Null;
        const auto started = product.Start(fixture, gameplay, {}, mode, [&](auto &input, auto &project) {
            input.cookedDecisions = std::make_shared<const Internal::HeadlessCookedDecisionInput>(
                Internal::HeadlessCookedDecisionInput{.committed = *project.committed,
                                                      .bindings = {{controller, assets.front().record.id}},
                                                      .execution = AiSceneCookedDecisionExecution{project.jobs,
                                                                                                  {.mode = AiSchedulingMode::BestEffort},
                                                                                                  {}},
                                                      .provider = Internal::HeadlessDecisionProvider::SelectedNativeGameplay,
                                                      .querySchema =
                                                          std::make_shared<const QuerySchemaRegistry>(std::move(registry).Value())});
        });
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().code.Value() << ": " << started.ErrorValue().message);
        REQUIRE(started.HasValue());
        product.RunFrame();
        product.RunFrame();
        auto read = [&] {
            return Internal::InspectHeadlessDecisionValue(product.PhysicsService(), agent, key);
        };
        CHECK_FALSE(read().Value());
        std::optional<BlackboardValue> winner;
        for (std::size_t attempt = 0; attempt < 80 && !winner; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            product.RunFrame();
            const auto failure = Internal::InspectHeadlessDecisionFailure(product.PhysicsService());
            REQUIRE(failure.HasValue());
            if (failure.Value())
                UNSCOPED_INFO(failure.Value()->error.code.Value() << ": " << failure.Value()->error.message);
            REQUIRE_FALSE(failure.Value());
            auto value = read();
            REQUIRE(value.HasValue());
            winner = std::move(value).Value();
        }
        REQUIRE(winner);
        const auto actual = std::get<Math::WorldCoordinate64>(std::get<BlackboardScalarValue>(*winner));
        // This Detour provider scores through the cooked triangle's center, so the upper corner of the small lattice costs least.
        auto expected =
            scoreCost ? Math::WorldCoordinate64::FromMillimeters(1500, 0, 1500) : Math::WorldCoordinate64::FromMillimeters(5000, 0, 1000);
        if (mixed)
            expected = Math::WorldCoordinate64::FromMillimeters(1500, 0, 1000);
        CAPTURE(static_cast<int>(scenario), actual.Millimeters(), expected.Millimeters());
        CHECK(actual == expected);
    }

    TEST_CASE("Cooked AI replay records terminal best-effort Navigation outcomes", "[ai][navigation][character][product]") {
        NavigationProductFixture fixture;
        DecisionFixture::Fixture decision;
        auto source = DecisionFixture::TaskTree(11);
        source.executionTask = DecisionFixture::MakeIdentity<AI::TaskId>(9000);
        const std::array catalog{AI::AiControllerDescriptor{.controller = DecisionFixture::MakeIdentity<AI::ControllerTypeId>(49),
                                                            .decisionAsset = source.asset,
                                                            .decisionKind = AI::DecisionPlanKind::BehaviorTree,
                                                            .blackboardSchema = decision.schema}};
        const auto asset = PublishDecisionMotionProject(fixture, decision, source, catalog.front(), std::nullopt);
        const auto gameplay = NavigationGameplay();
        const auto agent = DecisionFixture::MakeIdentity<AI::AgentId>(50);
        auto replay = std::make_shared<Internal::HeadlessDecisionReplay>(Internal::HeadlessDecisionReplaySelection{agent, 2});
        NativeNavigationProductWorld product;
        auto started = product.Start(fixture, gameplay, {}, Physics::PhysicsRuntimeMode::Canonical, [&](auto &input, auto &assets) {
            input.aiControllers = catalog;
            input.cookedDecisions = std::make_shared<Internal::HeadlessCookedDecisionInput>(
                Internal::HeadlessCookedDecisionInput{.committed = *assets.committed,
                                                      .environment = decision.Environment(),
                                                      .bindings = {{catalog.front().controller, asset}},
                                                      .execution = AI::AiSceneCookedDecisionExecution{assets.jobs, {}, {}}});
            input.decisionReplay = replay;
        });
        INFO((started.HasError() ? started.ErrorValue().message : "started"));
        REQUIRE(started.HasValue());
        product.RunFrame();
        const auto capability = Internal::InspectHeadlessNavigationContext(product.PhysicsService());
        REQUIRE(capability);
        auto request = capability->Submit({.start = {1, 0, 1}, .destination = {2, 0, 2}});
        REQUIRE(request.HasValue());
        const auto cancelled = request.Value().Cancel();
        REQUIRE(cancelled.HasValue());
        REQUIRE(cancelled.Value());
        product.RunFrame();
        auto result = replay->TakeCommitted();
        REQUIRE(result);
        INFO((result->HasError() ? result->ErrorValue().message : "captured"));
        REQUIRE(result->HasValue());
        const auto &data = result->Value();
        REQUIRE(data.navigation);
        CHECK(data.navigation->tick == 2);
        REQUIRE(data.navigation->count == 1);
        CHECK_FALSE(data.navigation->results.front().status);
        CHECK(data.navigation->results.front().digest != Sha256Digest{});
        auto changed = data;
        ++changed.navigation->results.front().sequence;
        const auto comparison = Internal::CompareHeadlessReplayCaptures(data, changed);
        REQUIRE(comparison.HasValue());
        CHECK_FALSE(comparison.Value());
        const auto publish = [&](const std::filesystem::path &directory, const Internal::HeadlessDecisionReplayData &captured) {
            const auto path = directory.u8string();
            const auto output = Internal::HeadlessReplayOutput::Parse({reinterpret_cast<const char *>(path.data()), path.size()});
            REQUIRE(output.HasValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{captured.canonicalRecord}));
            REQUIRE(Internal::PublishHeadlessReplayArtifact(output.Value(), captured, digest).HasValue());
            return directory / "evidence.json";
        };
        const auto root = fixture.project.project.dir.path;
        const auto recorded = publish(root / "nav-recorded", data);
        std::ifstream evidence{recorded, std::ios::binary};
        REQUIRE(evidence.is_open());
        const std::string evidenceText{std::istreambuf_iterator<char>{evidence}, {}};
        CHECK(evidenceText.find(R"("nondeterministicProviders":{"navigation":["native-navigation-path-worker"]})") != std::string::npos);
        const auto differing = publish(root / "nav-differing", changed);
        const auto artifactComparison = Internal::CompareHeadlessReplayArtifacts(recorded, differing);
        REQUIRE(artifactComparison.HasValue());
        CHECK_FALSE(artifactComparison.Value());
        changed.navigation->profile = changed.sceneInput;
        CHECK(Internal::CompareHeadlessReplayCaptures(data, changed).HasError());
        const auto incompatible = publish(root / "nav-incompatible", changed);
        CHECK(Internal::CompareHeadlessReplayArtifacts(recorded, incompatible).HasError());
    }

    TEST_CASE("Cooked AI decision moves or follows through real Character feedback", "[ai][navigation][character][product]") {
        using namespace Navigation;
        enum class Scenario {
            MoveTo,
            Follow,
            MissingAnimation,
            Animated,
            Dedicated
        };
        const auto scenario =
            GENERATE(Scenario::MoveTo, Scenario::Follow, Scenario::MissingAnimation, Scenario::Animated, Scenario::Dedicated);
        const bool follows = scenario == Scenario::Follow;
        const bool dedicated = scenario == Scenario::Dedicated;
        const bool animated = scenario == Scenario::Animated || dedicated;
        const auto role = dedicated ? Network::NetworkProjectRole::DedicatedServer : Network::NetworkProjectRole::Standalone;
        const Network::NetworkModeWorld authority{Network::NetworkModeWorldKind::AuthorityServer, {31}};
        const auto motionTrigger = Animation::GraphParameterId::Create(2).Value();
        NavigationProductFixture fixture;
        DecisionFixture::Fixture decision;
        decision.probe.succeedTick = std::numeric_limits<std::uint64_t>::max();
        auto source = DecisionFixture::TaskTree(11);
        source.executionTask = DecisionFixture::MakeIdentity<AI::TaskId>(9000);
        const std::array catalog{AI::AiControllerDescriptor{.controller = DecisionFixture::MakeIdentity<AI::ControllerTypeId>(49),
                                                            .decisionAsset = source.asset,
                                                            .decisionKind = AI::DecisionPlanKind::BehaviorTree,
                                                            .blackboardSchema = decision.schema}};
        const auto decisionAsset = PublishDecisionMotionProject(fixture, decision, source, catalog.front(),
                                                                animated ? std::optional{motionTrigger} : std::nullopt);
        Math::Vec3 goal{3, 0, 2};
        std::optional<NavigationAgentOwner> target;
        bool targetVisible = true;
        bool staged{};
        auto dynamic = NavigationDynamicRegistry::Create().Value();
        Math::Vec3 position;
        Math::Vec3 velocity;
        std::size_t captures{};
        auto policy = std::make_shared<Internal::HeadlessNavigationMotionPolicy>();
        NativeNavigationProductWorld product;
        decision.probe.onStart = [&] {
            staged =
                Internal::StageHeadlessNavigationMotion(product.PhysicsService(), decision.probe.continuation,
                                                        {.mode = follows ? NavigationMovementMode::Follow : NavigationMovementMode::MoveTo,
                                                         .destination = goal,
                                                         .entity = target,
                                                         .acceptanceRadiusMeters = 0.2F,
                                                         .timeoutTicks = 350,
                                                         .query = {NavigationQueryKind::Path,
                                                                   NavigationQualityLevel::Balanced,
                                                                   {64, 16, 100}},
                                                         .capabilityRevision = 3},
                                                        product.tick)
                    .HasValue();
        };
        policy->solver = CreateRecastDetourCrowdBackend().Value();
        policy->capture = [&](const auto &, const auto &active, const auto &agents, const auto samples, const std::uint64_t tick) {
            REQUIRE(samples.size() == 1);
            if (captures++ == 0)
                REQUIRE(dynamic.CommitAtSafePoint(active, tick).HasValue());
            position = samples.front().position;
            velocity = samples.front().velocity;
            const std::array profiles{NavigationCrowdProfileFacts{.profile = agents.Agents().front().profile}};
            return BuildNavigationCrowdSnapshot(agents, dynamic.Snapshot().Value(), samples, profiles, {}, tick);
        };
        policy->follow = [&](const Runtime::RuntimeSceneView &scene, const NavigationAgentOwner owner, const std::uint64_t) {
            using Target = Result<std::optional<NavigationMovementTarget>>;
            if (!targetVisible || owner.scene.Value() != scene.RuntimeId().value)
                return Target::Success(std::nullopt);
            const auto member = scene.Get({scene.RuntimeId(), {.index = owner.entityIndex, .generation = owner.entityGeneration}});
            if (member.HasError())
                return Target::Failure(member.ErrorValue());
            auto location = member.Value().localTransform->translation;
            location.y = goal.y;
            return Target::Success(NavigationMovementTarget{owner, location, 1});
        };
        Internal::HeadlessNavigationActorPolicy actorPolicy{{3}, 1};
        if (animated || scenario == Scenario::MissingAnimation)
            actorPolicy.motionStartTrigger = motionTrigger;
        const auto started = product.Start(fixture, NavigationGameplay(), {actorPolicy}, Physics::PhysicsRuntimeMode::Canonical,
                                           [&](auto &input, auto &assets) {
            input.aiControllers = catalog;
            input.cookedDecisions = std::make_shared<Internal::HeadlessCookedDecisionInput>(
                Internal::HeadlessCookedDecisionInput{.committed = *assets.committed,
                                                      .environment = decision.Environment(),
                                                      .bindings = {{catalog.front().controller, decisionAsset}},
                                                      .execution =
                                                          AI::AiSceneCookedDecisionExecution{assets.jobs,
                                                                                             {.mode = AI::AiSchedulingMode::BestEffort},
                                                                                             {}}});
            input.navigationMotion = policy;
            if (animated) {
                input.animationTarget = fixture.project.request.assets.target;
                input.animationIdentities =
                    std::make_shared<Internal::AnimationRuntimeIdentitySource>(Internal::AnimationRuntimeIdentityRange{1001, 1100});
            }
        }, role, dedicated ? std::optional{Internal::HeadlessSimulationAssets{.decisions = {decisionAsset}}} : std::nullopt);
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().code.Value() + ": " + started.ErrorValue().message);
        REQUIRE(started.HasValue());
        const auto preview = Assets::AssetId::FromBytes(PreviewTextureId);
        const auto &committed = *product.resources->committed;
        CHECK(product.resources->registry.Snapshot().Find(preview));
        CHECK((committed.Find(preview) != nullptr) == !dedicated);
        CHECK(committed.SceneProvider().Exists(preview, {}).Value() == !dedicated);
        CHECK(committed.ArtifactProvider().Exists(preview, {}).Value() == !dedicated);
        CHECK(committed.Find(decisionAsset));
        if (dedicated) {
            const auto health = product.scheduled->AiHealth(authority);
            REQUIRE(health);
            CHECK(health->decisionTick == 0);
            CHECK_FALSE(product.scheduled->AiHealth({Network::NetworkModeWorldKind::Client, authority.scene}));
            CHECK_FALSE(product.scheduled->AiHealth({authority.kind, {32}}));
        }
        const auto scene = Internal::InspectHeadlessScene(product.SceneService());
        REQUIRE(scene);
        const auto actor = scene->Find({3});
        REQUIRE(actor);
        goal.y = scene->Get(*actor).Value().localTransform->translation.y;
        if (follows) {
            const auto following = scene->Find({1});
            REQUIRE(following);
            target = NavigationAgentOwner{NavigationSceneRuntimeId::Create(scene->RuntimeId().value).Value(), following->entity.index,
                                          following->entity.generation};
        }
        if (scenario == Scenario::MissingAnimation) {
            product.RunFrame();
            product.RunFrame();
            CHECK(staged);
            CHECK(captures == 0);
            REQUIRE(decision.probe.cleanups == 1);
            CHECK(decision.probe.lastTerminal == AI::AiTaskState::Failed);
            REQUIRE(decision.probe.lastFailure);
            CHECK(decision.probe.lastFailure->code.Value() == Animation::AnimationErrors::GraphAdmissionRejected.code.Value());
            return;
        }
        const auto positionReached = [&] {
            return std::hypot(position.x - goal.x, position.z - goal.z) <= 0.2F;
        };
        std::size_t graphTransitions{};
        for (std::size_t index = 0; index < 340 && decision.probe.cleanups == 0; ++index) {
            product.RunFrame();
            if (dedicated && index == 0) {
                const auto health = product.scheduled->AiHealth(authority);
                REQUIRE(health);
                CHECK(health->decisionTick == 1);
                CHECK(health->evaluated == 1);
                CHECK(health->workUnits > 0);
                CHECK_FALSE(health->degraded);
            }
            if (animated) {
                const auto *animation = Internal::InspectHeadlessAnimation(product.SceneService());
                REQUIRE(animation);
                const auto pose = animation->Query(*actor);
                REQUIRE(pose.HasValue());
                REQUIRE(pose.Value().component);
                std::array<Animation::GraphStateEvent, 8> events;
                std::uint32_t written{};
                REQUIRE(animation->CopyStateEvents(*actor, pose.Value().component->currentPose, events, written).HasValue());
                graphTransitions +=
                    std::ranges::count(std::span{events}.first(written), Animation::GraphStateEventKind::TransitionCompleted,
                                       &Animation::GraphStateEvent::kind);
            }
            if (follows && positionReached() && std::hypot(velocity.x, velocity.z) < 0.05F)
                break;
            std::this_thread::yield();
        }
        CHECK(staged);
        CHECK(captures > 2);
        REQUIRE(positionReached());
        if (animated)
            CHECK(graphTransitions == 1);
        if (follows) {
            REQUIRE(decision.probe.cleanups == 0);
            product.RunFrame();
            CHECK(decision.probe.cleanups == 0);
            targetVisible = false;
            product.RunFrame();
            product.RunFrame();
            REQUIRE(decision.probe.cleanups == 1);
            CHECK(decision.probe.lastTerminal == AI::AiTaskState::Failed);
            REQUIRE(decision.probe.lastFailure);
            CHECK(decision.probe.lastFailure->code.Value() == NavigationErrors::InvalidHandle.code.Value());
            return;
        }
        REQUIRE(decision.probe.cleanups == 1);
        if (decision.probe.lastFailure)
            UNSCOPED_INFO(decision.probe.lastFailure->code.Value() + ": " + decision.probe.lastFailure->message);
        CAPTURE(goal.y, position.y);
        CHECK(decision.probe.lastTerminal == AI::AiTaskState::Succeeded);
        CHECK(decision.probe.aborts == 0);
    }
#endif

    TEST_CASE("Configured Navigation actor admission requires a Character on the exact cooked entity",
              "[navigation][scene][gameplay][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Scene);
        NativeNavigationProductWorld product;
        const auto started = product.Start(fixture, NavigationGameplay(), {{{3}, 1}});
        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == Physics::PhysicsErrors::HandleStale.code.Value());
    }

    TEST_CASE("Native gameplay executes a cooked standalone path and rejects retained requests after world retirement",
              "[navigation][scene][gameplay][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Scene);
        const auto gameplay = NavigationGameplay();
        NativeNavigationProductWorld product;
        REQUIRE(product.Start(fixture, gameplay).HasValue());
        const auto outOfPhase = Internal::StageHeadlessNavigationMotion(*product.physics, {}, {}, 1);
        REQUIRE(outOfPhase.HasError());
        CHECK(outOfPhase.ErrorValue().code.Value() == AI::AIErrors::SchedulerPhaseInvalid.code.Value());

        const auto scope = Internal::InspectHeadlessNavigationContext(*product.physics);
        REQUIRE(scope);
        CHECK(scope->Binding().queryGranted);
        CHECK_FALSE(scope->Binding().movementGranted);
        CHECK(scope->Binding().scene.scene.Value() == 31);
        const auto movement = scope->Submit({.operation = Gameplay::GameplayNavigationOperation::MoveTo});
        REQUIRE(movement.HasError());
        CHECK(movement.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::PermissionDenied.code.Value());

        // Read results emitted by the real native behavior activated on the cooked Scene, rather than a fake backend.
        auto image = Platform::LoadDynamicLibraryAtPath(gameplay.nativeArtifact);
        REQUIRE(image.HasValue());
        auto *symbol = image.Value()->GetSymbol("HoroTestTakeNavigationResult");
        REQUIRE(symbol);
        const auto take = std::bit_cast<void (*)(void *)>(symbol);
        std::shared_ptr<const Gameplay::GameplayNavigationResult> output;
        while (product.tick < 512 && !output) {
            product.RunFrame();
            take(&output);
            if (!output)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(output);
        REQUIRE(output->path);
        CHECK(output->path->status == Navigation::NavigationPathStatus::Reachable);
        REQUIRE_FALSE(output->path->corridor.empty());
        CHECK(output->path->corridor.front().provenance.world == scope->Binding().scene.world);

        auto pending = scope->Submit({.start = {1, 0, 1}, .destination = {2, 0, 2}});
        REQUIRE(pending.HasValue());
        product.RunFrame();
        product.Stop();
        auto closed = product.composition->pollRetirement(64);
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        const auto rejected = pending.Value().Poll();
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::ScopeRevoked.code.Value());
        CHECK(gameplay.moduleRetirements->Close().HasValue());
    }

    TEST_CASE("Cooked prefab native path handles retire with the exact behavior agent while the module stays active",
              "[navigation][scene][gameplay][prefab][product]") {
        NavigationProductFixture fixture;
        fixture.Publish(NavigationProductFixture::Input::Valid, NavigationProductFixture::Client::Prefab);
        auto gameplay = NavigationGameplay();
        gameplay.prefabPermission = Internal::GameplayPrefabPermission::Granted;
        NativeNavigationProductWorld product;
        REQUIRE(product.Start(fixture, gameplay).HasValue());
        const auto moduleScope = Internal::InspectHeadlessNavigationContext(*product.physics);
        REQUIRE(moduleScope);

        auto image = Platform::LoadDynamicLibraryAtPath(gameplay.nativeArtifact);
        REQUIRE(image.HasValue());
        const auto prefabSymbol = image.Value()->GetSymbol("HoroTestCopyPrefabCapability");
        const auto pathSymbol = image.Value()->GetSymbol("HoroTestTakeNavigationSubmission");
        const auto resultSymbol = image.Value()->GetSymbol("HoroTestTakeNavigationResult");
        REQUIRE(prefabSymbol);
        REQUIRE(pathSymbol);
        REQUIRE(resultSymbol);
        using PrefabProbe = void (*)(std::shared_ptr<const Gameplay::GameplayPrefabContext> *);
        using PathProbe =
            void (*)(std::shared_ptr<const Gameplay::GameplayNavigationContext> *, std::optional<Gameplay::GameplayNavigationHandle> *);
        const auto copyPrefab = std::bit_cast<PrefabProbe>(prefabSymbol);
        const auto takePath = std::bit_cast<PathProbe>(pathSymbol);
        const auto takeResult = std::bit_cast<void (*)(void *)>(resultSymbol);
        std::shared_ptr<const Gameplay::GameplayPrefabContext> prefabs;
        product.RunFrame();
        copyPrefab(&prefabs);
        REQUIRE(prefabs);
        const auto spawned =
            prefabs->Spawn({.source = {fixture.project.prefabId, fixture.prefabDigest, fixture.project.request.assets.target},
                            .notBeforeTick = product.tick + 1});
        REQUIRE(spawned.HasValue());
        product.Await(spawned.Value(), Prefab::PrefabSpawnState::Committed);
        const auto instance = spawned.Value().Spawned();
        REQUIRE(instance.HasValue());
        const auto agent = instance.Value().Root();
        REQUIRE(agent.IsValid());
        product.RunFrame();

        std::shared_ptr<const Gameplay::GameplayNavigationContext> agentScope;
        std::optional<Gameplay::GameplayNavigationHandle> completed;
        takePath(&agentScope, &completed);
        REQUIRE(agentScope);
        REQUIRE(completed);
        CHECK(agentScope != moduleScope);
        const Gameplay::GameplayNavigationRequest path{.start = {1, 0, 1}, .destination = {2, 0, 2}};
        const auto quota = moduleScope->Submit(path);
        REQUIRE(quota.HasError());
        CHECK(quota.ErrorValue().code.Value() == Navigation::NavigationErrors::AdmissionRejected.code.Value());

        std::shared_ptr<const Gameplay::GameplayNavigationResult> result;
        while (product.tick < 512 && !result) {
            product.RunFrame();
            takeResult(&result);
            if (!result)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(result);
        REQUIRE(result->path);
        REQUIRE(result->path->status == Navigation::NavigationPathStatus::Reachable);

        CancellationSource cancelled;
        const auto preserved = prefabs->Despawn(instance.Value(), product.tick + 1, cancelled.Token());
        REQUIRE(preserved.HasValue());
        cancelled.RequestCancellation();
        product.Await(preserved.Value(), Prefab::PrefabSpawnState::Cancelled);
        CHECK(agentScope->IsUsable());
        REQUIRE(completed->Poll().HasValue());

        const auto pending = agentScope->Submit(path);
        REQUIRE(pending.HasValue());
        const auto retired = prefabs->Despawn(instance.Value(), product.tick + 1);
        REQUIRE(retired.HasValue());
        product.Await(retired.Value(), Prefab::PrefabSpawnState::Committed);
        const auto current = Internal::InspectHeadlessScene(*product.physics);
        REQUIRE(current);
        CHECK(current->Get(agent).HasError());
        const auto &completedRequest = *completed;
        for (const auto *request : {&completedRequest, &pending.Value()}) {
            const auto response = request->Poll();
            REQUIRE(response.HasError());
            CHECK(response.ErrorValue().code.Value() == Gameplay::NavigationCapabilityErrors::ScopeRevoked.code.Value());
        }
        CHECK_FALSE(agentScope->IsUsable());
        CHECK(agentScope->Submit(path).HasError());
        REQUIRE(moduleScope->IsUsable());

        // The canonical request keeps its slot charged until accepted worker work drains. The module resumes when that fence exits.
        bool moduleAvailable = false;
        const auto deadline = product.tick + 64;
        while (product.tick < deadline && !moduleAvailable) {
            product.RunFrame();
            const auto unrelated = moduleScope->Submit(path);
            moduleAvailable = unrelated.HasValue();
            if (!moduleAvailable)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(moduleAvailable);
        product.Stop();
        const auto drained = product.composition->pollRetirement(64);
        REQUIRE(drained.HasValue());
        CHECK(drained.Value());
        CHECK(gameplay.moduleRetirements->Close().HasValue());
    }
#endif

    TEST_CASE("Cooked project Scene selection captures the root's explicit network authority", "[scene][network][product]") {
        Fixture fixture;
        Internal::TestSupport::ReplicatedScriptFixture script;
        const auto &inventory = script.selection.replicationAdmission->inventory;
        Horo::Tests::WriteReplicationProject(fixture.project.project.dir.path, inventory);
        const Internal::HeadlessProjectSceneOptions options{.projectRoot = fixture.project.project.dir.path,
                                                            .scene = "cooked:" + fixture.project.sceneId.ToString(),
                                                            .cookedRoot = fixture.project.cooked.path,
                                                            .cookTarget = fixture.project.request.assets.target.Value(),
                                                            .captureNetworkSettings = true};
        auto prepared = Internal::PrepareHeadlessProjectSceneResources(options);
        if (prepared.HasError())
            UNSCOPED_INFO(prepared.ErrorValue().message);
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value()->networkSettings);
        const auto &captured = prepared.Value()->networkSettings->ReplicationInventory();
        CHECK(captured.completeness == inventory.completeness);
        REQUIRE(captured.declarations.size() == 1);
        CHECK(captured.declarations.front().schema == inventory.declarations.front().schema);
        CHECK(prepared.Value()->sceneAsset == fixture.project.sceneId);
    }

    TEST_CASE("Headless project AI settings supply scheduler budgets and gate development replay", "[scene][ai][product]") {
        Fixture fixture;
        fixture.project.WriteProjectMetadata(
            R"({"schemaVersion":1,"scheduler":{"maximumAgents":16,"evaluationsPerTick":4},"query":{"maximumWorkUnitsPerTick":2048}})");
        const auto selector = "cooked:" + fixture.project.sceneId.ToString();
        const Internal::HeadlessProjectSceneOptions options{.projectRoot = fixture.project.project.dir.path,
                                                            .scene = selector,
                                                            .cookedRoot = fixture.project.cooked.path,
                                                            .cookTarget = fixture.project.request.assets.target.Value()};
        const auto prepared = Internal::PrepareHeadlessProjectSceneResources(options);
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value()->decisionScheduler.maximumAgents == 16);
        CHECK(prepared.Value()->decisionScheduler.evaluationsPerTick == 4);
        CHECK(prepared.Value()->queryWorkUnitsLimit == 2048);
        CHECK_FALSE(prepared.Value()->developmentReplayAllowed);

        const auto gameplay = NavigationGameplay();
        auto arguments = SceneArguments(fixture, selector, "null");
        arguments.insert(arguments.end(),
                         {"--game-module", Utf8Path(gameplay.nativeArtifact), gameplay.moduleId,
                          std::to_string(gameplay.descriptorRevision), "deny", "--ai-decision", "49", fixture.project.sceneId.ToString()});
        auto single = arguments;
        single.insert(single.end(), {"--ai-replay", "50", "1"});
        if constexpr (HORO_TEST_NETWORK_PRODUCT_PACKAGED) {
            constexpr auto denial = "AI replay capture is unavailable in the packaged network product";
            fixture.project.WriteProjectMetadata(R"({"schemaVersion":1,"development":{"liveMutation":true,"replayCapture":true}})");
            CHECK(RunCli(std::move(single), denial).exitCode == 3);
            arguments.insert(arguments.end(), {"--ai-replay-window", "50", "1", "2"});
            CHECK(RunCli(std::move(arguments), denial).exitCode == 3);
        } else {
            CHECK(RunCli(std::move(single), "Project AI policy does not grant development replay capture").exitCode == 3);
        }
    }

    TEST_CASE("Terminal Scene options reject malformed incomplete and escaping input", "[character][scene][cli][product]") {
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", "relative", "--scene", "source:level.scene"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--physics", "canonical"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--navigation", "recast-detour"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", "/", "--scene", "untyped.scene"}).exitCode == 2);
        Fixture fixture;
        CHECK(RunCli(SceneArguments(fixture, "source:../escape.scene", "null")).exitCode == 3);
        CHECK(RunCli(SceneArguments(fixture, "cooked:not-an-asset-id", "null")).exitCode == 3);
    }

    TEST_CASE("Terminal Scene options reject duplicate and unbounded requests before project startup", "[character][scene][cli][product]") {
        const std::string root = Utf8Path(std::filesystem::absolute(std::filesystem::temp_directory_path()));
        const std::vector<std::vector<std::string>> invalidSuffixes{{"--scene"},
                                                                    {"--scene", ""},
                                                                    {"--unknown", "value"},
                                                                    {"--project", root},
                                                                    {"--scene", "source:unused.horo"},
                                                                    {"--cooked-root", root},
                                                                    {"--cook-target", "native"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--physics",
                                                                     "automatic"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--physics",
                                                                     "null", "--physics", "canonical"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--project", root},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--cooked-root",
                                                                     "relative", "--cook-target", "native"}};
        for (const auto &suffix : invalidSuffixes) {
            CAPTURE(suffix);
            std::vector<std::string> arguments{"--run-network-product", "standalone", "1"};
            arguments.insert(arguments.end(), suffix.begin(), suffix.end());
            CHECK(RunCli(std::move(arguments)).exitCode == 2);
        }
        for (const std::string_view timeout : {"0", "-1", "60001", "1ms", "18446744073709551616"}) {
            CAPTURE(timeout);
            CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", root, "--scene", "source:unused.horo",
                          "--load-timeout-ms", std::string{timeout}})
                      .exitCode == 2);
        }
    }
}  // namespace Horo::Application::CharacterSceneProductTest
