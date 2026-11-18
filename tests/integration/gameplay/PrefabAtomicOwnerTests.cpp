#include "DecisionGraphCompilerTestSupport.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "HeadlessNetworkServices.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabAtomicOwnerTestSupport.h"

#include <set>

namespace Horo::Application::PrefabAtomicTest {
#if HORO_TEST_PHYSICS_NATIVE
    using enum Fault;

    namespace {
        /** @brief Captures a declared registry for both real host and prefab admission. */
        std::shared_ptr<const AI::AiControllerPerceptionEndpoints> PerceptionEndpoints() {
            const AI::PerceptionDescriptorOrigin origin{AI::PerceptionDescriptorSourceKind::Native,
                                                        AI::TestSupport::MakeIdentity<AI::PerceptionProviderId>(71), 1};
            const auto sense = AI::SenseTypeIds::Sight;
            const auto stimulus = AI::TestSupport::MakeIdentity<AI::StimulusTypeId>(81);
            const auto listener = AI::TestSupport::MakeIdentity<AI::PerceptionListenerTypeId>(91);
            const std::array senses{AI::SenseTypeDescriptor{sense, origin, "Sight"}};
            const std::array stimuli{AI::StimulusTypeDescriptor{stimulus, origin, "Visible actor"}};
            const std::array listeners{AI::PerceptionListenerDescriptor{.identity = listener,
                                                                        .origin = origin,
                                                                        .displayName = "Sight",
                                                                        .sense = sense,
                                                                        .stimuli = {{stimulus}},
                                                                        .stimulusCount = 1}};
            auto registry = AI::PerceptionDescriptorRegistry::Capture({senses, stimuli, listeners}, {});
            REQUIRE(registry.HasValue());
            return std::make_shared<const AI::AiControllerPerceptionEndpoints>(
                AI::AiControllerPerceptionEndpoints{.registry = std::make_shared<const AI::PerceptionDescriptorRegistry>(
                                                        std::move(registry).Value()),
                                                    .listeners = {listener},
                                                    .sources = {sense}});
        }

        /** @brief Supplies explicit absolute positions for this fixture's known Scene facts. */
        Result<std::vector<AI::AiScenePerceptionPosition>> PerceptionPositions(const Runtime::RuntimeSceneView &scene) {
            std::vector<AI::AiScenePerceptionPosition> positions;
            for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
                const auto entity = scene.EntityAt(slot);
                if (entity && entity->components->aiController)
                    positions.push_back(
                        {entity->entity, Math::WorldCoordinate64::FromMillimeters(static_cast<std::int64_t>(slot) * 1000, 0, 0)});
            }
            return Result<std::vector<AI::AiScenePerceptionPosition>>::Success(std::move(positions));
        }

        /** @brief Verifies the Scene's actual remapped values equal the published AI owner's records. */
        void RequireAllOwners(Fixture &fixture, std::size_t count, std::uint32_t bodies) {
            REQUIRE(fixture.Count() == count);
            REQUIRE(fixture.Bodies() == bodies);
            const auto snapshot = fixture.ai.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == count);
            std::set<std::uint64_t> identities;
            for (const auto &agent : snapshot.Value().Agents()) {
                REQUIRE(agent.state == AI::AiAgentActivationState::Active);
                const auto entity = fixture.scenes.ActiveScene()->Get(agent.owner);
                REQUIRE(entity.HasValue());
                REQUIRE(entity.Value().components->aiAgent.has_value());
                CHECK(entity.Value().components->aiAgent->agent == agent.agent.agent);
                CHECK(identities.insert(agent.agent.agent.Value()).second);
            }
        }

        std::string_view FailureCode(Fault fault) {
            switch (fault) {
                case Material:
                case MaterialIdentity:
                    return Physics::PhysicsErrors::MaterialDescriptorInvalid.code.Value();
                case AiDescriptor:
                    return AI::AIErrors::ControllerDescriptorMissing.code.Value();
                default:
                    return Gameplay::GameplayErrors::BehaviorNotRegistered.code.Value();
            }
        }

        TEST_CASE("Production cooked prefab publication admits Physics AI and native Gameplay independently of owner registration order",
                  "[prefab][integration][atomic][physics][ai][gameplay]") {
            bool aiFirst{};
            SECTION("AI registered before derived owners") {
                aiFirst = true;
            }
            SECTION("AI registered after derived owners") {
                // Keep the default registration order with AI last.
            }
            Fixture fixture{aiFirst};
            RequireAllOwners(fixture, 1, 0);
            const auto resident = fixture.ai.Snapshot().Value().Agents().front().handle;
            REQUIRE(fixture.ai.StartTaskAtSafePoint(resident, AI::TestSupport::MakeIdentity<AI::TaskId>(31)).HasValue());
            const auto retained = fixture.ai.Snapshot();
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            CHECK(fixture.ai.Find(resident).Value().hasRunningTask);
            CHECK(retained.Value().Agents().size() == 1);
            fixture.Tick(2);
            fixture.Reach(1);
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.ai.Find(resident).Value().hasRunningTask);
        }

        TEST_CASE("Cooked prefab root zero Physics endpoints resolve through exact group fixups before native publication",
                  "[prefab][integration][atomic][physics][creation]") {
            Fixture fixture{false, RootPhysicsEndpoints};
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 2);
            const auto operation = fixture.SpawnOperation();
            const auto spawned = operation.Spawned();
            REQUIRE(spawned.HasValue());
            const auto root = spawned.Value().Root();
            const auto view = fixture.scenes.ActiveScene()->Get(root);
            REQUIRE(view.HasValue());
            REQUIRE(view.Value().physicsReferences.size() == 1);
            CHECK(view.Value().physicsReferences.front().target == root);
            CHECK(view.Value().physicsReferences.front().body == Runtime::PhysicsBodySlotId{1});
            bool foundChild{};
            for (std::size_t index = 0; index < fixture.scenes.ActiveScene()->SlotCount(); ++index) {
                const auto entity = fixture.scenes.ActiveScene()->EntityAt(index);
                if (!entity || entity->entity == root)
                    continue;
                if (entity->components->physicsConstraints.empty())
                    continue;
                foundChild = true;
                REQUIRE(entity->physicsReferences.size() == 3);
                CHECK(std::ranges::any_of(entity->physicsReferences, [root](const auto &reference) {
                    return reference.kind == Runtime::GroupPhysicsReferenceKind::ConstraintSecond && reference.target == root;
                }));
            }
            CHECK(foundChild);
        }

        TEST_CASE("Every real prefab owner failure leaves the previous aggregate and resident AI task intact",
                  "[prefab][integration][atomic][rollback]") {
            Fault fault{};
            SECTION("verified envelope with malformed native material") {
                fault = Material;
            }
            SECTION("valid material payload with a different asset identity") {
                fault = MaterialIdentity;
            }
            SECTION("missing actual AI activation descriptor") {
                fault = AiDescriptor;
            }
            SECTION("unregistered native Gameplay behavior") {
                fault = GameplayDescriptor;
            }
            INFO("Injected owner failure: " << static_cast<unsigned>(fault));
            Fixture fixture{false, fault};
            const auto before = fixture.scenes.ActiveScene()->StructuralRevision();
            const auto resident = fixture.ai.Snapshot().Value().Agents().front().handle;
            REQUIRE(fixture.ai.StartTaskAtSafePoint(resident, AI::TestSupport::MakeIdentity<AI::TaskId>(31)).HasValue());
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            const auto expected = FailureCode(fault);
            bool matched{};
            for (const Error *error = operation.Failure(); error; error = error->cause.Get())
                matched = matched || error->code.Value() == expected;
            CHECK(matched);
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == before);
            CHECK(fixture.ai.Find(resident).Value().hasRunningTask);
        }

        TEST_CASE("Repeated real native prefab spawns reserve fresh AI identities after prior group retirement",
                  "[prefab][integration][atomic][identity]") {
            Fixture fixture;
            fixture.Tick(1);
            fixture.Reach(3);
            std::uint64_t highWater{};
            const auto beforeRetirement = fixture.ai.Snapshot();
            REQUIRE(beforeRetirement.HasValue());
            for (const auto &agent : beforeRetirement.Value().Agents())
                highWater = std::max(highWater, agent.agent.agent.Value());
            fixture.Tick(2);
            fixture.Reach(1);
            fixture.RestartGameplay();
            fixture.Tick(3);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            const auto afterRestart = fixture.ai.Snapshot();
            REQUIRE(afterRestart.HasValue());
            for (const auto &agent : afterRestart.Value().Agents())
                if (agent.owner != fixture.controller.owner)
                    CHECK(agent.agent.agent.Value() > highWater);
        }

        TEST_CASE("Cooked prefab degraded AI preserves the unresolved authored controller without execution authority",
                  "[prefab][integration][atomic][ai][degraded]") {
            Fixture fixture{false, AiDescriptor, {.missingController = AI::AiMissingControllerPolicy::PreserveInactive}};
            fixture.Tick(1);
            fixture.Reach(3);
            const auto snapshot = fixture.ai.Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto agents = snapshot.Value().Agents();
            REQUIRE(agents.size() == 3);
            CHECK(std::ranges::count(agents, AI::AiAgentActivationState::Active, &AI::AiAgentRuntimeRecord::state) == 2);
            const auto unavailable = std::ranges::find(agents, AI::AiAgentActivationState::Degraded, &AI::AiAgentRuntimeRecord::state);
            REQUIRE(unavailable != agents.end());
            CHECK_FALSE(unavailable->hasBlackboard);
            CHECK(unavailable->stagedCapabilities.bits == 0);
            REQUIRE(unavailable->controller);
            CHECK(unavailable->controller->enabled);
            CHECK(unavailable->controller->controller == AI::TestSupport::MakeIdentity<AI::ControllerTypeId>(99));
            CHECK(unavailable->controller->decisionAsset == fixture.controller.descriptor.decisionAsset);
            const auto scene = fixture.scenes.ActiveScene();
            REQUIRE(scene);
            const auto entity = scene->Get(unavailable->owner);
            REQUIRE(entity.HasValue());
            CHECK(entity.Value().components->aiAgent == std::optional{unavailable->agent});
            CHECK(entity.Value().components->aiController == unavailable->controller);
            CHECK(fixture.ai.StartTaskAtSafePoint(unavailable->handle, AI::TestSupport::MakeIdentity<AI::TaskId>(31)).HasError());
            CHECK(fixture.ai.CaptureDecisionSnapshotAtSafePoint(unavailable->handle).HasError());
            const auto canonical = fixture.ai.CaptureCanonicalState(*scene);
            REQUIRE(canonical.HasValue());
            const auto durable = std::ranges::find_if(canonical.Value().agents, [&](const auto &entry) {
                return entry.authored == unavailable->agent;
            });
            REQUIRE(durable != canonical.Value().agents.end());
            REQUIRE(durable->controller);
            CHECK(durable->controller->authored == *unavailable->controller);
            CHECK_FALSE(durable->controller->blackboard);
            fixture.Tick(2);
            fixture.Reach(1);
            CHECK(fixture.ai.Find(unavailable->handle).HasError());
        }

        TEST_CASE("A retained perception publication drops the despawned prefab source and listener before recapture",
                  "[prefab][integration][atomic][ai][perception]") {
            Fixture fixture;
            fixture.Tick(1);
            fixture.Reach(3);
            const auto prefab = fixture.SpawnOperation().Spawned();
            REQUIRE(prefab.HasValue());
            const auto root = prefab.Value().Root();
            const auto snapshot = fixture.ai.Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto agents = snapshot.Value().Agents();
            const auto spawned = std::ranges::find(agents, root, &AI::AiAgentRuntimeRecord::owner);
            const auto resident = std::ranges::find(agents, fixture.controller.owner, &AI::AiAgentRuntimeRecord::owner);
            REQUIRE(spawned != agents.end());
            REQUIRE(resident != agents.end());
            const auto capability = fixture.ai.CapabilityAtSafePoint();
            const auto spawnedLifetime = capability.CaptureAgentLifetime(spawned->handle);
            const auto residentLifetime = capability.CaptureAgentLifetime(resident->handle);
            REQUIRE(spawnedLifetime.HasValue());
            REQUIRE(residentLifetime.HasValue());

            const auto sense = AI::SenseTypeIds::Sight;
            const std::array listeners{AI::PerceptionSpatialListener{.entity = root,
                                                                     .senses = {sense},
                                                                     .senseCount = 1,
                                                                     .ownerCancellation = spawnedLifetime.Value()},
                                       AI::PerceptionSpatialListener{.entity = resident->owner,
                                                                     .senses = {sense},
                                                                     .senseCount = 1,
                                                                     .ownerCancellation = residentLifetime.Value()}};
            const std::array sources{AI::PerceptionSpatialSource{.entity = root,
                                                                 .layers = 1,
                                                                 .senses = {sense},
                                                                 .senseCount = 1,
                                                                 .ownerCancellation = spawnedLifetime.Value()}};
            AI::PerceptionSpatialBroadphase broadphase;
            const auto spatial = broadphase.Publish(*fixture.scenes.ActiveScene(), 1, listeners, sources);
            REQUIRE(spatial.HasValue());
            REQUIRE(spatial.Value()->Allows(resident->owner, root, sense));
            REQUIRE(spatial.Value()->ListenerPosition(root).HasValue());

            fixture.Tick(2);
            fixture.Reach(1);
            CHECK(spawnedLifetime.Value().IsCancellationRequested());
            CHECK_FALSE(residentLifetime.Value().IsCancellationRequested());
            CHECK(capability.CaptureAgentLifetime(spawned->handle).HasError());
            CHECK(broadphase.Current() == spatial.Value());
            CHECK_FALSE(spatial.Value()->Allows(resident->owner, root, sense));
            CHECK(spatial.Value()->ListenerPosition(root).HasError());
            const auto remaining = spatial.Value()->Query({.listener = resident->owner, .sense = sense, .visibleLayers = 1});
            REQUIRE(remaining.HasValue());
            CHECK(remaining.Value().count == 0);
        }

        TEST_CASE("Scene AI stages declared Perception endpoints for real cooked prefab spawn and despawn",
                  "[prefab][integration][atomic][ai][perception]") {
            Fixture fixture{false, Fault::None, {.requirePerceptionEndpoints = true}, PerceptionEndpoints()};
            fixture.Tick(1);
            fixture.Reach(3);
            const auto prefab = fixture.SpawnOperation().Spawned();
            REQUIRE(prefab.HasValue());
            const auto root = prefab.Value().Root();
            AI::PerceptionSpatialBroadphase broadphase;
            const auto scene = *fixture.scenes.ActiveScene();
            const auto positions = PerceptionPositions(scene);
            REQUIRE(positions.HasValue());
            const auto spatial = fixture.ai.PublishPerceptionAtSafePoint(scene, 1, positions.Value(), broadphase);
            REQUIRE(spatial.HasValue());
            CHECK(spatial.Value()->SourceCount() == 3);
            CHECK(spatial.Value()->ListenerCount() == 3);
            CHECK(spatial.Value()->Allows(fixture.controller.owner, root, AI::SenseTypeIds::Sight));
            CHECK(fixture.ai.PublishPerceptionAtSafePoint(scene, 2, {}, broadphase).HasError());
            CHECK(broadphase.Current() == spatial.Value());

            fixture.Tick(2);
            fixture.Reach(1);
            CHECK_FALSE(spatial.Value()->Allows(fixture.controller.owner, root, AI::SenseTypeIds::Sight));
            const auto replacementScene = *fixture.scenes.ActiveScene();
            const auto replacementPositions = PerceptionPositions(replacementScene);
            REQUIRE(replacementPositions.HasValue());
            const auto replacement = fixture.ai.PublishPerceptionAtSafePoint(replacementScene, 2, replacementPositions.Value(), broadphase);
            REQUIRE(replacement.HasValue());
            CHECK(replacement.Value()->SourceCount() == 1);
            CHECK(replacement.Value()->ListenerCount() == 1);
        }

        TEST_CASE("Configured headless Scene requires authored Perception endpoints and authoritative positions",
                  "[prefab][integration][ai][perception][host]") {
            using namespace Network;
            auto controller = AI::TestSupport::MakeFixture(1, 11, 1, 0);
            controller.agent.controller->requiredCapabilities = AI::AiCapabilitySet::Of(AI::AiCapability::Perception);
            Runtime::SceneDefinitionBuilder source{{7}, {1}};
            source.Add({.object = {1}, .components = {.aiAgent = controller.agent.agent, .aiController = controller.agent.controller}});
            auto definition = std::move(source).Build();
            REQUIRE(definition.HasValue());

            JobSystem jobs{{1, 8}};
            Assets::MemoryAssetProvider bytes;
            Assets::AssetLoadService loader{jobs, bytes};
            Assets::AssetRegistry registry;
            const Internal::HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{3}};
            std::array catalog{controller.descriptor};
            Internal::HeadlessSceneActivationInput input{Physics::PhysicsRuntimeMode::Canonical,
                                                         Physics::Settings(),
                                                         registry,
                                                         loader,
                                                         load,
                                                         catalog};
            input.perceptionPositions = PerceptionPositions;
            bool admitted = false;
            auto role = NetworkProjectRole::Standalone;
            auto world = NetworkModeWorldKind::Standalone;
            SECTION("complete host configuration publishes one endpoint pair") {
                catalog.front().perceptionEndpoints = PerceptionEndpoints();
                admitted = true;
            }
            SECTION("missing endpoint declaration rejects preparation") {}
            SECTION("headless authority requires an explicit position provider") {
                catalog.front().perceptionEndpoints = PerceptionEndpoints();
                input.perceptionPositions = {};
            }
            SECTION("host cannot publish when coordinate evidence is missing") {
                catalog.front().perceptionEndpoints = PerceptionEndpoints();
                input.perceptionPositions = [](const Runtime::RuntimeSceneView &) {
                    return Result<std::vector<AI::AiScenePerceptionPosition>>::Success({});
                };
            }
            SECTION("the paired client preserves existing Scene admission without publishing endpoints") {
                role = NetworkProjectRole::ListenServer;
                world = NetworkModeWorldKind::Client;
                admitted = true;
            }

            const auto composed = Internal::ComposeHeadlessSceneNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                                    std::move(definition).Value()),
                                                                                std::move(input));
            REQUIRE(composed.HasValue());
            const auto built = composed.Value().factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)](
                {NetworkModeServiceKind::Scene, role, {}, world, {7}});
            REQUIRE(built.HasValue());
            auto &sceneService = *built.Value();
            const auto prepared = sceneService.Prepare();
            if (admitted) {
                REQUIRE(prepared.HasValue());
                REQUIRE(sceneService.Activate().HasValue());
                const auto spatial = Internal::InspectHeadlessPerception(sceneService);
                if (world == NetworkModeWorldKind::Standalone) {
                    REQUIRE(spatial);
                    CHECK(spatial->SourceCount() == 1);
                    CHECK(spatial->ListenerCount() == 1);
                    CHECK(spatial->Scene() == Runtime::SceneRuntimeId{7});
                    const auto physics = composed.Value().factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Physics)](
                        {NetworkModeServiceKind::Physics, role, {}, world, {7}});
                    REQUIRE(physics.HasValue());
                    REQUIRE(physics.Value()->Prepare().HasValue());
                    REQUIRE(physics.Value()->Activate().HasValue());
                    REQUIRE(physics.Value()->RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
                    const auto refreshed = Internal::InspectHeadlessPerception(sceneService);
                    REQUIRE(refreshed);
                    CHECK(refreshed->Revision() == spatial->Revision() + 1);
                    physics.Value()->Shutdown();
                } else {
                    CHECK_FALSE(spatial);
                }
            } else {
                REQUIRE(prepared.HasError());
                CHECK_FALSE(Internal::InspectHeadlessPerception(sceneService));
            }
            sceneService.Shutdown();
        }

        /** @brief Reuses the compiler's exact immutable bindings as a normal Scene controller descriptor. */
        AI::TestSupport::ActivationFixture CookedController(const AI::DecisionGraphTestSupport::Fixture &fixture,
                                                            const std::shared_ptr<const AI::CookedDecisionPlan> &plan) {
            auto controller = AI::TestSupport::MakeFixture(1, 11, 1, 0);
            controller.schema = fixture.schema;
            controller.plan = plan->Graphs()[plan->Occurrences().front().plan].bindings;
            controller.agent.controller->decisionAsset = plan->Root();
            controller.agent.controller->blackboardSchema = fixture.schema->Identity();
            controller.descriptor.decisionAsset = plan->Root();
            controller.descriptor.blackboardSchema = fixture.schema;
            controller.descriptor.decisionPlan = controller.plan;
            controller.descriptor.cookedPlan = plan;
            return controller;
        }

        TEST_CASE("Configured headless Scene admits exact committed cooked AI plans before publishing its controller",
                  "[prefab][integration][ai][decisions][host]") {
            using namespace Network;
            namespace Decisions = AI::DecisionGraphTestSupport;
            namespace Output = Assets::OutputTestSupport;
            Decisions::Fixture ai;
            auto graphSource = Decisions::TaskTree(41);
            graphSource.executionTask = Decisions::MakeIdentity<AI::TaskId>(879);
            const auto artifactId = Assets::AssetId::Parse("5c881920-450f-4ca7-b938-d151696ea419").Value();
            const auto target = Output::Target("headless-null");
            auto artifactType = Output::Type("core.behavior_tree");
            auto requestedId = artifactId;
            auto declaredRoot = graphSource.asset;
            const ErrorCodeDescriptor *expected = nullptr;
            std::optional<std::size_t> executionBudget;
            bool malformed = false;
            bool staleProvider = false;
            SECTION("exact cooked root") {}
            SECTION("owner runs opted-in decision through the product fixed tick") {
                executionBudget = 64;
            }
            SECTION("missing authored parent task") {
                executionBudget = 64;
                graphSource.executionTask = {};
                expected = &AI::AIErrors::TaskContextInvalid;
            }
            SECTION("insufficient host work allowance rejects startup") {
                executionBudget = 1;
                expected = &AI::AIErrors::SchedulerBudgetExhausted;
            }
            SECTION("foreign project asset identity") {
                requestedId = Assets::AssetId::Parse("5c881920-450f-4ca7-b938-d151696ea428").Value();
                expected = &AI::AIErrors::ControllerDescriptorMissing;
            }
            SECTION("wrong asset family") {
                artifactType = Output::Type("core.state_machine");
                expected = &AI::AIErrors::ControllerDescriptorIncompatible;
            }
            SECTION("wrong declared graph identity") {
                declaredRoot = Decisions::Graph(42);
                expected = &AI::AIErrors::ControllerDescriptorIncompatible;
            }
            SECTION("malformed authenticated semantic payload") {
                malformed = true;
                expected = &AI::AIErrors::DecisionAssetSchemaInvalid;
            }
            SECTION("stale providers from the committed compile environment") {
                staleProvider = true;
                expected = &AI::AIErrors::RuntimeUnavailable;
            }

            const auto plan = ai.Compile(std::span{&graphSource, 1}, 41);
            auto encoded = AI::DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(encoded.HasValue());
            auto payload = std::move(encoded).Value();
            if (malformed)
                payload.resize(3);
            if (staleProvider)
                ai.admission->store(false);

            Assets::MemoryAssetProvider raw;
            const auto digest = Put(raw, artifactId, artifactType, target, std::move(payload));
            auto envelope = raw.Load(artifactId, {});
            REQUIRE(envelope.HasValue());
            const std::array artifacts{std::move(envelope).Value()};
            const std::array manifest{Assets::AssetCookManifestEntry{artifactId, artifactType, artifactId.ToString() + ".cooked", digest}};
            Output::TempDir directory;
            REQUIRE(Output::PublishFixture(directory.path, target, manifest, artifacts).HasValue());
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{artifactId, artifactType, ProjectPath::Parse("assets/guard.horo_bt").Value(),
                                   ProjectPath::Parse("assets/guard.horo_bt.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto committed =
                Internal::CommittedSceneAssets::Capture(NativeDurableFileSystem{}, {directory.path, target}, registry.Snapshot());
            REQUIRE(committed.HasValue());

            auto controller = CookedController(ai, plan);
            controller.agent.controller->decisionAsset = declaredRoot;
            controller.descriptor.decisionAsset = declaredRoot;
            controller.descriptor.decisionPlan.reset();
            controller.descriptor.cookedPlan.reset();
            Runtime::SceneDefinitionBuilder source{{7}, {1}};
            source.Add({.object = {1}, .components = {.aiAgent = controller.agent.agent, .aiController = controller.agent.controller}});
            auto definition = std::move(source).Build();
            REQUIRE(definition.HasValue());
            JobSystem jobs{{1, 8}};
            Assets::AssetLoadService loader{jobs, committed.Value()->SceneProvider()};
            const Internal::HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
            const std::array catalog{controller.descriptor};
            auto selected = Internal::HeadlessCookedDecisionInput{*committed.Value(),
                                                                  ai.Environment(),
                                                                  {{controller.descriptor.controller, requestedId}}};
            if (executionBudget)
                selected.execution.emplace(AI::AiSceneCookedDecisionExecution{jobs, {}, {.workUnitsPerTick = *executionBudget}});
            const auto decisions = std::make_shared<const Internal::HeadlessCookedDecisionInput>(std::move(selected));
            const auto composed = Internal::ComposeHeadlessSceneNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                                    std::move(definition).Value()),
                                                                                {.mode = Physics::PhysicsRuntimeMode::Canonical,
                                                                                 .settings = Physics::Settings(),
                                                                                 .registry = registry,
                                                                                 .loads = loader,
                                                                                 .load = load,
                                                                                 .aiControllers = catalog,
                                                                                 .cookedDecisions = decisions});
            REQUIRE(composed.HasValue());
            const auto built = composed.Value().factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)](
                {NetworkModeServiceKind::Scene, NetworkProjectRole::Standalone, {}, NetworkModeWorldKind::Standalone, {7}});
            REQUIRE(built.HasValue());
            const auto prepared = built.Value()->Prepare();
            if (expected) {
                REQUIRE(prepared.HasError());
                CHECK(prepared.ErrorValue().code.Value() == expected->code.Value());
            } else {
                REQUIRE(prepared.HasValue());
                const auto physics = composed.Value().factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Physics)](
                    {NetworkModeServiceKind::Physics, NetworkProjectRole::Standalone, {}, NetworkModeWorldKind::Standalone, {7}});
                REQUIRE(physics.HasValue());
                REQUIRE(physics.Value()->Prepare().HasValue());
                REQUIRE(physics.Value()->Activate().HasValue());
                if (executionBudget) {
                    CHECK(ai.probe.factories == 1);
                    CHECK(ai.probe.starts == 0);
                    CancellationSource cancellation;
                    const auto token = cancellation.Token();
                    const Runtime::FixedStepContext fixed{1, Duration::FromNanoseconds(16'666'667), token};
                    const auto result = physics.Value()->RunFixedTick(fixed);
                    INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
                    REQUIRE(result.HasValue());
                    CHECK(ai.probe.starts == 1);
                    CHECK(ai.probe.resumes == 0);
                }
                physics.Value()->Shutdown();
            }
            CHECK(ai.probe.factories == (executionBudget && !expected ? 1 : 0));
            built.Value()->Shutdown();
            CHECK(ai.probe.aborts == (executionBudget && !expected ? 1 : 0));
            CHECK(ai.probe.cleanups == (executionBudget && !expected ? 1 : 0));
        }

        TEST_CASE("Cooked prefab Perception endpoint positions publish within the actual aggregate Scene lifecycle",
                  "[prefab][integration][atomic][ai][perception]") {
            AI::PerceptionSpatialBroadphase spatial;
            std::size_t captures{};
            bool supplied = true;
            Fault fault = Fault::None;
            const ErrorCodeDescriptor *failure = nullptr;
            AI::AiScenePerceptionGroupInput group{spatial,
                                                  {},
                                                  [&](const Runtime::RuntimeSceneView &scene,
                                                      std::span<const Runtime::RuntimeEntityView> prospective) {
                ++captures;
                std::vector<AI::AiScenePerceptionPosition> positions;
                REQUIRE(prospective.size() == 2);
                for (const auto &entity : prospective) {
                    CHECK(scene.Get(entity.entity).HasError());
                    if (supplied)
                        positions.push_back(
                            {entity.entity, Math::WorldCoordinate64::FromMillimeters(entity.entity.entity.index * 1000, 0, 0)});
                }
                return Result<std::vector<AI::AiScenePerceptionPosition>>::Success(std::move(positions));
            }};
            SECTION("valid initial coordinates publish and later despawn requires no position capture") {}
            SECTION("missing prospective authority preserves all owners") {
                group.positions = {};
                failure = &AI::AIErrors::PerceptionDependencyMissing;
            }
            SECTION("absent prospective coordinates preserve all owners") {
                supplied = false;
                failure = &AI::AIErrors::PerceptionDependencyMissing;
            }
            SECTION("later Gameplay rejection discards the prepared spatial snapshot") {
                fault = Fault::GameplayDescriptor;
                failure = &Gameplay::GameplayErrors::BehaviorNotRegistered;
            }
            Fixture fixture{true, fault, {.requirePerceptionEndpoints = true}, PerceptionEndpoints(), group};
            const auto baseline = *fixture.scenes.ActiveScene();
            const auto positions = PerceptionPositions(baseline);
            REQUIRE(positions.HasValue());
            const auto initial = fixture.ai.PublishPerceptionAtSafePoint(baseline, 1, positions.Value(), spatial);
            REQUIRE(initial.HasValue());
            REQUIRE(initial.Value()->SourceCount() == 1);
            fixture.Tick(1);
            if (failure) {
                const auto rejected = fixture.AwaitRejection();
                REQUIRE(rejected.Failure());
                CHECK(rejected.Failure()->code.Value() == failure->code.Value());
                CHECK(baseline.IsCurrent());
                CHECK(spatial.Current() == initial.Value());
                CHECK(initial.Value()->ReadAuthorization()->IsCurrent());
                CHECK(captures == (group.positions ? 1 : 0));
                RequireAllOwners(fixture, 1, 0);
                return;
            }
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            REQUIRE(spatial.Current());
            CHECK(spatial.Current()->ListenerCount() == 3);
            CHECK(spatial.Current()->SourceCount() == 3);
            CHECK(spatial.Current()->Revision() == 2);
            CHECK_FALSE(initial.Value()->ReadAuthorization()->IsCurrent());
            const auto spawned = spatial.Current();
            supplied = false;
            fixture.Tick(2);
            fixture.Reach(1);
            RequireAllOwners(fixture, 1, 0);
            CHECK(captures == 1);
            CHECK(spatial.Current()->ListenerCount() == 1);
            CHECK(spatial.Current()->SourceCount() == 1);
            CHECK(spatial.Current()->Revision() == 3);
            CHECK_FALSE(spawned->ReadAuthorization()->IsCurrent());
        }

        /** @brief Observes whether aggregate retirement revokes the actual owner token before scheduler callbacks. */
        struct RetirementProbe final : AI::IAiScheduledDecision {
            CancellationToken owner;
            const Runtime::RuntimeSceneService *scenes{};
            Runtime::EntityRef entity;
            bool revokedOnCancel{};
            bool removedOnCancel{};
            std::size_t cancellations{};

            Result<AI::AiSliceOutcome> Evaluate(const AI::AiDecisionSliceContext &, AI::AiWorkBudget &) noexcept override {
                return Result<AI::AiSliceOutcome>::Success(AI::AiSliceOutcome::Complete);
            }

            Result<bool> Commit(const AI::AiDecisionSliceContext &, AI::AiWorkBudget &) noexcept override {
                return Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                revokedOnCancel = owner.IsCancellationRequested();
                const auto scene = scenes->ActiveScene();
                removedOnCancel = scene && scene->Get(entity).HasError();
                ++cancellations;
            }
        };

        TEST_CASE("Cooked prefab despawn fences live AI work and frozen blackboards before a replacement spawn",
                  "[prefab][integration][atomic][ai][retirement]") {
            Fault fault = Fault::None;
            SECTION("published retirement") {}
            SECTION("operation cancelled during aggregate validation") {
                fault = Fault::CancelRetirement;
            }
            Fixture fixture{false, fault};
            REQUIRE(fixture.ai.ConfigureTaskSchedulerAtSafePoint(fixture.jobs).HasValue());
            const auto scheduler = fixture.ai.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            const auto resident = fixture.ai.Snapshot().Value().Agents().front().handle;
            const auto residentSnapshot = fixture.ai.CaptureDecisionSnapshotAtSafePoint(resident);
            REQUIRE(residentSnapshot.HasValue());

            fixture.Tick(1);
            fixture.Reach(3);
            const auto snapshot = fixture.ai.Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto agent = std::ranges::find_if(snapshot.Value().Agents(), [resident](const auto &entry) {
                return entry.handle != resident;
            });
            REQUIRE(agent != snapshot.Value().Agents().end());
            auto reserved = fixture.ai.ReserveDecisionTaskSlotsAtSafePoint(agent->handle, 1);
            REQUIRE(reserved.HasValue());
            const auto retained = fixture.ai.CaptureDecisionSnapshotAtSafePoint(agent->handle);
            REQUIRE(retained.HasValue());
            const auto task = fixture.ai.StartTaskAtSafePoint(agent->handle, AI::TestSupport::MakeIdentity<AI::TaskId>(31));
            REQUIRE(task.HasValue());
            const auto retainedCapability = fixture.ai.CapabilityAtSafePoint();
            REQUIRE(retainedCapability.InspectTask(agent->handle, task.Value()).HasValue());
            auto probe = std::make_shared<RetirementProbe>();
            probe->owner = reserved.Value().Cancellation();
            probe->scenes = &fixture.scenes;
            probe->entity = agent->owner;
            REQUIRE(fixture.ai.RegisterDecisionAtSafePoint(agent->handle, {}, std::make_shared<int>(1), probe).HasValue());
            REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
            REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());

            fixture.Tick(2);
            if (fault == Fault::CancelRetirement) {
                const auto priorRevision = fixture.scenes.ActiveScene()->StructuralRevision();
                fixture.retirementToCancel = fixture.DespawnOperation();
                REQUIRE(fixture.retirementToCancel.State() == Prefab::PrefabSpawnState::Pending);
                fixture.AwaitState(fixture.retirementToCancel, Prefab::PrefabSpawnState::Cancelled);
                CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == priorRevision);
                CHECK(fixture.scenes.ActiveScene()->Get(agent->owner).HasValue());
                CHECK(probe->cancellations == 0);
                REQUIRE(scheduler.Value()->Wake(agent->handle, {.perception = true}).HasValue());
                REQUIRE(retained.Value().Revision().HasValue());
                REQUIRE(retainedCapability.InspectTask(agent->handle, task.Value()).HasValue());
                return;
            }
            fixture.Reach(1);
            CHECK(probe->cancellations == 1);
            CHECK(probe->revokedOnCancel);
            CHECK(probe->removedOnCancel);
            CHECK_FALSE(reserved.Value().CanAdmit());
            CHECK(retained.Value().Revision().HasError());
            CHECK(retainedCapability.InspectTask(agent->handle, task.Value()).HasError());
            CHECK(scheduler.Value()->Wake(agent->handle, {.perception = true}).HasError());
            CHECK(residentSnapshot.Value().Revision().HasValue());

            fixture.RestartGameplay();
            fixture.Tick(3);
            fixture.Reach(3);
            const auto replacement = fixture.ai.Snapshot();
            REQUIRE(replacement.HasValue());
            CHECK(replacement.Value().Find(agent->handle).HasError());
            CHECK(replacement.Value().Find(resident).HasValue());
            CHECK(probe->cancellations == 1);
        }

        TEST_CASE("Native prefab decisions publish with the group and retire their exact runnable tasks",
                  "[prefab][integration][atomic][ai][decisions]") {
            namespace Decisions = AI::DecisionGraphTestSupport;
            Decisions::Fixture decisions;
            decisions.probe.succeedTick = 99;
            auto source = Decisions::TaskTree(51);
            source.executionTask = Decisions::MakeIdentity<AI::TaskId>(879);
            const auto plan = decisions.Compile(std::span{&source, 1}, 51);
            const auto controller = CookedController(decisions, plan);
            AI::AiTaskSchedulerSettings settings;
            bool admit = true;
            SECTION("running prefabs retire before the next decision") {}
            SECTION("an oversubscribed group preserves the resident decision and task identities") {
                admit = false;
                settings.maximumAgents = 1;
                settings.evaluationsPerTick = 1;
            }
            Fixture fixture{false, Fault::None, {}, {}, std::nullopt, &controller, settings};
            auto scheduler = fixture.ai.TaskSchedulerAtSafePoint();
            REQUIRE(scheduler.HasValue());
            auto &owner = *scheduler.Value();
            REQUIRE(owner.EvaluateAtDecision(1).HasValue());
            REQUIRE(owner.CommitAtIntentDispatch(1).HasValue());
            REQUIRE(decisions.probe.starts == 1);
            const auto resident = fixture.ai.Snapshot().Value().Agents().front().handle;

            fixture.Tick(1);
            if (admit) {
                fixture.Reach(3);
            } else {
                const auto rejected = fixture.AwaitRejection();
                REQUIRE(rejected.Failure());
                bool capacityError = false;
                for (const Error *error = rejected.Failure(); error; error = error->cause.Get())
                    capacityError = capacityError || error->code.Value() == AI::AIErrors::AgentCapacityExceeded.code.Value();
                CHECK(capacityError);
            }

            REQUIRE(owner.EvaluateAtDecision(2).HasValue());
            REQUIRE(owner.CommitAtIntentDispatch(2).HasValue());
            CHECK(decisions.probe.starts == (admit ? 3 : 1));
            CHECK(decisions.probe.resumes == 1);
            if (!admit) {
                CHECK(fixture.Count() == 1);
                const auto next = fixture.ai.ReserveDecisionTaskSlotsAtSafePoint(resident, 1);
                REQUIRE(next.HasValue());
                CHECK(next.Value().FirstSlot() == plan->Storage().taskSlots + 1);
                CHECK(decisions.probe.aborts == 0);
                return;
            }

            fixture.Tick(2);
            fixture.Reach(1);
            CHECK(decisions.probe.aborts == 2);
            CHECK(decisions.probe.cleanups == 2);
            REQUIRE(owner.EvaluateAtDecision(3).HasValue());
            REQUIRE(owner.CommitAtIntentDispatch(3).HasValue());
            CHECK(decisions.probe.starts == 3);
            CHECK(decisions.probe.resumes == 2);
            CHECK(decisions.probe.aborts == 2);
        }

        TEST_CASE("Cancelling accepted prefab work or unloading its actual module never publishes a partial aggregate",
                  "[prefab][integration][atomic][cancel]") {
            Fixture fixture;
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.SpawnOperation();
            REQUIRE(operation.State() == Prefab::PrefabSpawnState::Pending);
            SECTION("caller cancellation") {
                operation.Cancel();
                fixture.AwaitState(operation, Prefab::PrefabSpawnState::Cancelled);
            }
            SECTION("native module unload closes its owner scope") {
                fixture.gameplay->UnloadModule();
                CHECK(operation.State() == Prefab::PrefabSpawnState::Cancelled);
            }
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Spawned().HasError());
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Catalog replacement fences accepted prefab loads before any owner publication",
                  "[prefab][integration][atomic][catalog]") {
            Fixture fixture;
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.SpawnOperation();
            fixture.PublishRegistry();
            fixture.AwaitState(operation, Prefab::PrefabSpawnState::Failed);
            REQUIRE(operation.Failure() != nullptr);
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Eviction and provider shutdown retain real prefab leases through all-owner Scene unload",
                  "[prefab][integration][atomic][lifetime]") {
            Fixture fixture;
            fixture.Tick(1);
            fixture.Reach(3);
            const auto lease = fixture.LoadLease();
            const auto digest = lease.Artifact().Digest();
            fixture.templates.Evict(fixture.asset);
            RequireAllOwners(fixture, 3, 1);
            CHECK(fixture.templates.PayloadSnapshot().retainedPayloadBytes > 0);
            fixture.gameplay->Shutdown();
            fixture.activeGameplay = nullptr;
            fixture.templates.Shutdown();
            REQUIRE(fixture.scenes.QueueUnload().HasValue());
            REQUIRE(fixture.scenes
                        .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                 {.frameNumber = ++fixture.frame, .cancellation = fixture.cancellation.Token()})
                        .HasValue());
            CHECK_FALSE(fixture.scenes.ActiveScene());
            CHECK(fixture.native.Borrow().HasError());
            CHECK(fixture.ai.Snapshot().HasError());
            REQUIRE(lease.Template() != nullptr);
            CHECK(lease.Template()->Data().assetId == fixture.asset);
            CHECK(lease.Artifact().Digest() == digest);
            CHECK_FALSE(lease.Artifact().Bytes().empty());
            CHECK(fixture.templates.QueuePreparedGroup(lease, {}, {}).HasError());
        }

        TEST_CASE("Scene rejects duplicate foreign and incomplete facts from a real detached AI reservation",
                  "[prefab][integration][atomic][identity][authority]") {
            Fault fault{DuplicateFacts};
            SECTION("duplicate fact") {
                // Keep the default duplicate-fact fault.
            }
            SECTION("foreign Scene owner") {
                fault = ForeignFacts;
            }
            SECTION("incomplete reservation") {
                fault = MissingFacts;
            }
            Fixture fixture{false, fault};
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Failure()->code.Value() == "scene.runtime.invalid_candidate");
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Duplicate identity producer registration is rejected before the actual prefab transaction",
                  "[prefab][integration][atomic][identity][authority]") {
            Fixture fixture{false, DuplicateProducer};
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
        }

        TEST_CASE("Enabled structural Navigation rejects the complete prefab before any owner publishes",
                  "[prefab][integration][atomic][navigation]") {
            Fixture fixture{false, Navigation};
            const auto revision = fixture.scenes.ActiveScene()->StructuralRevision();
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Failure()->code.Value() == "scene.navigation.structural_unsupported");
            RequireAllOwners(fixture, 1, 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == revision);
        }

        TEST_CASE("Disabled typed Navigation remains intact when the real supported prefab owners publish",
                  "[prefab][integration][atomic][navigation]") {
            Fixture fixture{false, DisabledNavigation};
            fixture.Tick(1);
            fixture.Reach(3);
            RequireAllOwners(fixture, 3, 1);
            bool found{};
            for (std::size_t index = 0; index < fixture.scenes.ActiveScene()->SlotCount(); ++index) {
                const auto entity = fixture.scenes.ActiveScene()->EntityAt(index);
                if (!entity || !entity->components->navigationAgent)
                    continue;
                found = true;
                CHECK_FALSE(entity->components->navigationAgent->enabled);
                CHECK(entity->components->navigationAgent->profile.Value() == 7);
                CHECK(entity->components->navigationAgent->filter.Value() == 9);
            }
            CHECK(found);
        }

        TEST_CASE("Malformed prefab originals and unresolved Physics source endpoints never enter any owner",
                  "[prefab][integration][atomic][validation]") {
            Fault fault{DuplicateAgent};
            SECTION("same-template duplicate original agent identity") {
                // Keep the default duplicate agent identity fault.
            }
            SECTION("body reference outside the exact source occurrence") {
                fault = BodyEndpoint;
            }
            Fixture fixture{false, fault};
            fixture.Tick(1);
            const auto operation = fixture.AwaitRejection();
            REQUIRE(operation.Failure() != nullptr);
            CHECK(operation.Failure()->code.Value() == Prefab::PrefabErrors::CorruptedPayload.code.Value());
            RequireAllOwners(fixture, 1, 0);
            CHECK_FALSE(fixture.scenes.TakeStructuralCommitResult());
        }
    }  // namespace
#else
    TEST_CASE("Prefab all-owner composition reports an omitted native Physics peer explicitly", "[prefab][integration][physics]") {
        const auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical);
        REQUIRE(runtime.HasError());
        CHECK(runtime.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value());
    }
#endif
}  // namespace Horo::Application::PrefabAtomicTest
