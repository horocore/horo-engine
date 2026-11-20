#include "DecisionGraphCompilerTestSupport.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "HeadlessNetworkServices.h"
#include "HeadlessPerceptionPolicy.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "OwnedTestDirectory.h"
#include "PrefabAtomicOwnerTestSupport.h"

#include <cstring>
#include <fstream>
#include <set>

namespace Horo::Application::PrefabAtomicTest {
#if HORO_TEST_PHYSICS_NATIVE
    using enum Fault;

    /** @brief Drives the actual headless fixed lifecycle through its replay publication or abort boundary. */
    void ExpectDecisionReplay(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                              Internal::HeadlessSceneActivationInput input, AI::AgentId agent, const Sha256Digest &planDigest, bool abort,
                              std::optional<std::uint32_t> referenceGeneration);

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
                    CHECK(spatial->ReadAuthorization()->IsCurrent());
                    const auto physics = composed.Value().factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Physics)](
                        {NetworkModeServiceKind::Physics, role, {}, world, {7}});
                    REQUIRE(physics.HasValue());
                    REQUIRE(physics.Value()->Prepare().HasValue());
                    REQUIRE(physics.Value()->Activate().HasValue());
                    REQUIRE(physics.Value()->RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
                    const auto refreshed = Internal::InspectHeadlessPerception(sceneService);
                    REQUIRE(refreshed);
                    CHECK(refreshed->Revision() == spatial->Revision() + 1);
                    CHECK_FALSE(spatial->ReadAuthorization()->IsCurrent());
                    REQUIRE(refreshed->ReadAuthorization()->IsCurrent());
                    sceneService.Shutdown();
                    CHECK_FALSE(refreshed->ReadAuthorization()->IsCurrent());
                    CHECK_FALSE(Internal::InspectHeadlessPerception(*physics.Value()));
                    physics.Value()->Shutdown();
                } else {
                    CHECK_FALSE(spatial);
                }
            } else {
                REQUIRE(prepared.HasError());
                CHECK_FALSE(Internal::InspectHeadlessScene(sceneService));
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

        /** @brief Observes fixture code through a test-only image reference that survives startup and retirement. */
        class NativeCookedDecisionProbe final {
        public:
            explicit NativeCookedDecisionProbe() : image_(Platform::LoadDynamicLibrary(HORO_TEST_AI_MODULE_PATH).Value()) {
                Bind();
            }

            ~NativeCookedDecisionProbe() {
                observeDecision_(nullptr);
                observeFactories_(nullptr);
                observeModule_(nullptr, false);
            }

            std::shared_ptr<const Gameplay::GameplayPrefabContext> PrefabCapability() {
                void (*copy)(std::shared_ptr<const Gameplay::GameplayPrefabContext> *) noexcept {};
                ReadObserver("HoroTestCopyAiPrefabCapability", copy);
                std::shared_ptr<const Gameplay::GameplayPrefabContext> capability;
                copy(&capability);
                REQUIRE(capability);
                return capability;
            }

            std::uint32_t factories{};
            std::array<std::uint32_t, 3> decision{};
            std::array<std::uint32_t, 4> module{};

        private:
            template <typename Observer> void ReadObserver(const char *name, Observer &observer) {
                auto *symbol = image_->GetSymbol(name);
                REQUIRE(symbol);
                static_assert(sizeof(Observer) == sizeof(symbol));
                std::memcpy(&observer, &symbol, sizeof(symbol));
            }

            void Bind() {
                ReadObserver("HoroTestObserveDecisionNodeLifecycle", observeDecision_);
                ReadObserver("HoroTestObserveDecisionFactory", observeFactories_);
                ReadObserver("HoroTestObservePerceptionSenseLifecycle", observeModule_);
                observeDecision_(decision.data());
                observeFactories_(&factories);
                observeModule_(module.data(), false);
            }

            std::unique_ptr<Platform::DynamicLibrary> image_;
            void (*observeDecision_)(std::uint32_t *) noexcept {};
            void (*observeFactories_)(std::uint32_t *) noexcept {};
            void (*observeModule_)(std::uint32_t *, bool) noexcept {};
        };

        /** @brief Loads the real generated native producer and captures its exact immutable executable generation. */
        class NativePerceptionFixture final {
        public:
            NativePerceptionFixture() {
                const Gameplay::GameModuleLoadExpectation expected{"game.ai.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                                   Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH)};
                auto candidate = Gameplay::GameModuleHost{}.Load(HORO_TEST_AI_MODULE_PATH, expected);
                REQUIRE(candidate.HasValue());
                REQUIRE_FALSE(candidate.Value().diagnostic);
                module_ = std::move(candidate).Value().owner;
                auto captured = module_->AcquirePerceptionSenses();
                REQUIRE(captured.HasValue());
                execution.generation = std::move(captured).Value();
                REQUIRE(execution.generation->Contracts().size() == 1);
                const auto &contract = execution.generation->Contracts().front();
                execution.capabilities = AI::PerceptionCapabilitySet::Of(AI::PerceptionCapability::SpatialCandidateQuery);
                execution.radiusMillimeters = 4000;
                execution.visibleLayers = 1;
                execution.workUnitsPerTick = RequiredUnits() * 3;
                const auto listener = AI::TestSupport::MakeIdentity<AI::PerceptionListenerTypeId>(91);
                const std::array listeners{AI::PerceptionListenerDescriptor{.identity = listener,
                                                                            .origin = contract.sense.origin,
                                                                            .displayName = "Fixture spatial listener",
                                                                            .sense = contract.sense.identity,
                                                                            .stimuli = {{contract.stimulus.identity}},
                                                                            .stimulusCount = 1}};
                auto registry =
                    AI::PerceptionDescriptorRegistry::Capture({std::span<const AI::SenseTypeDescriptor>{&contract.sense, 1},
                                                               std::span<const AI::StimulusTypeDescriptor>{&contract.stimulus, 1},
                                                               listeners},
                                                              execution.capabilities);
                REQUIRE(registry.HasValue());
                endpoints = std::make_shared<const AI::AiControllerPerceptionEndpoints>(
                    AI::AiControllerPerceptionEndpoints{.registry = std::make_shared<const AI::PerceptionDescriptorRegistry>(
                                                            std::move(registry).Value()),
                                                        .listeners = {listener},
                                                        .sources = {contract.sense.identity}});
            }

            [[nodiscard]] std::size_t RequiredUnits() const {
                return execution.generation->Contracts().front().workUnits + AI::PerceptionSenseGeneration::ValidationWorkUnits + 1;
            }

            NativeCookedDecisionProbe probe;

        private:
            std::unique_ptr<Gameplay::LoadedGameModule> module_;

        public:
            AI::AiScenePerceptionExecution execution;
            std::shared_ptr<const AI::AiControllerPerceptionEndpoints> endpoints;
        };

        /** @brief Captures test-authored bytes through the same native project read owner as product composition. */
        Result<std::shared_ptr<const Internal::HeadlessPerceptionPolicy>> CaptureProjectPerception(
            const Runtime::RuntimeSceneDefinition &definition, const Assets::AssetId asset, const std::string &source) {
            Horo::Tests::OwnedTestDirectory directory{"native-perception-placement"};
            constexpr std::string_view path = "perception.json";
            {
                std::ofstream stream{directory.Path() / path};
                REQUIRE(stream << source);
            }
            auto files = Platform::CreateNativeProjectReadFiles(directory.Path(), "perception-test", 1);
            REQUIRE(files.HasValue());
            const Platform::ProjectReadContext context{.projectIdentity = "perception-test",
                                                       .projectGeneration = 1,
                                                       .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3}};
            return Internal::CaptureHeadlessPerceptionPolicy(*files.Value(), context, path, asset, definition);
        }

        /** @brief Populates explicit stable selectors and a nonzero origin without native provider defaults. */
        std::string ProjectPerceptionSource(const Assets::AssetId asset, const std::uint64_t revision,
                                            const AI::AiControllerDescriptor &controller) {
            const auto &endpoints = *controller.perceptionEndpoints;
            return "{\"schema\":1,\"sceneAsset\":\"" + asset.ToString() + "\",\"sceneRevision\":" + std::to_string(revision) +
                   ",\"worldOriginMillimeters\":[100,200,-300],\"controllers\":[{\"controller\":" +
                   std::to_string(controller.controller.Value()) + ",\"listeners\":[" +
                   std::to_string(endpoints.listeners.front().Value()) + "],\"sources\":[" +
                   std::to_string(endpoints.sources.front().Value()) + "],\"layers\":1,\"mask\":1}]}";
        }

        TEST_CASE("Configured authoritative headless ticks poll the admitted native producer over its actual Scene",
                  "[prefab][integration][ai][perception][host]") {
            using namespace Network;
            NativePerceptionFixture native;
            auto controller = AI::TestSupport::MakeFixture(1, 11, 1, 0);
            const auto observerId = controller.agent.agent.agent;
            const auto privateKey = controller.schema->Keys().front().key;
            controller.agent.controller->requiredCapabilities = AI::AiCapabilitySet::Of(AI::AiCapability::Perception);
            controller.descriptor.perceptionEndpoints = native.endpoints;
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            builder.Add({.object = {1},
                         .localTransform = {.translation = {0.25F, 0.0F, 0.0F}},
                         .components = {.aiAgent = controller.agent.agent, .aiController = controller.agent.controller}});
            controller.agent.agent.agent = AI::TestSupport::MakeIdentity<AI::AgentId>(2);
            builder.Add({.object = {2},
                         .parent = Runtime::SceneObjectId{1},
                         .localTransform = {.translation = {0.75F, 0.0F, 0.0F}},
                         .components = {.aiAgent = controller.agent.agent, .aiController = controller.agent.controller}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            JobSystem jobs{{1, 8}};
            Assets::MemoryAssetProvider bytes;
            Assets::AssetLoadService loader{jobs, bytes};
            Assets::AssetRegistry registry;
            CancellationSource cancellation;
            const Internal::HeadlessSceneLoadBoundary load{cancellation.Token(),
                                                           std::chrono::steady_clock::now() + std::chrono::seconds{3}};
            std::array catalog{controller.descriptor};
            Internal::HeadlessSceneActivationInput input{Physics::PhysicsRuntimeMode::Canonical,
                                                         Physics::Settings(),
                                                         registry,
                                                         loader,
                                                         load,
                                                         catalog};
            input.perceptionPositions = PerceptionPositions;
            input.perceptionExecution = native.execution;
            auto role = NetworkProjectRole::Standalone;
            auto world = NetworkModeWorldKind::Standalone;
            bool seatsAdmitted = true;
            bool usesAuthoredCoordinates = false;
            SECTION("bounded fixed dispatch and paired shutdown") {}
            SECTION("durable endpoint selectors and parented coordinates reach actual fixed dispatch") {
                const auto asset = Assets::AssetId::Parse("5c881920-450f-4ca7-b938-d151696ea419").Value();
                const auto source = ProjectPerceptionSource(asset, definition.Value().Revision().value, controller.descriptor);
                auto policy = CaptureProjectPerception(definition.Value(), asset, source);
                REQUIRE(policy.HasValue());
                input.perceptionProject =
                    Internal::HeadlessPerceptionProjectAdmission{std::move(policy).Value(), native.endpoints->registry};
                input.perceptionPositions = {};
                catalog.front().perceptionEndpoints.reset();
                usesAuthoredCoordinates = true;
            }
            SECTION("the declared work budget refuses startup before invoking a sense") {
                input.perceptionExecution->workUnitsPerTick = native.RequiredUnits() - 1;
                seatsAdmitted = false;
            }
            SECTION("the connected client inherits no native Perception authority") {
                role = NetworkProjectRole::ListenServer;
                world = NetworkModeWorldKind::Client;
            }
            auto composed = Internal::ComposeHeadlessSceneNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                              std::move(definition).Value()),
                                                                          std::move(input));
            REQUIRE(composed.HasValue());
            const auto service = [&](const NetworkModeServiceKind kind) {
                return composed.Value().factories.services[static_cast<std::size_t>(kind)]({kind, role, {}, world, {7}});
            };
            auto sceneService = service(NetworkModeServiceKind::Scene);
            REQUIRE(sceneService.HasValue());
            const auto prepared = sceneService.Value()->Prepare();
            if (!seatsAdmitted) {
                REQUIRE(prepared.HasError());
                CHECK(prepared.ErrorValue().code.Value() == AI::AIErrors::SchedulerBudgetExhausted.code.Value());
                CHECK_FALSE(Internal::InspectHeadlessScene(*sceneService.Value()));
                CHECK(native.probe.module[0] == 0);
                return;
            }
            REQUIRE(prepared.HasValue());
            REQUIRE(sceneService.Value()->Activate().HasValue());
            const auto scene = Internal::InspectHeadlessScene(*sceneService.Value());
            REQUIRE(scene);
            const auto observer = scene->Find({1});
            const auto source = scene->Find({2});
            REQUIRE(observer);
            REQUIRE(source);
            CHECK(native.probe.module[1] == 0);
            if (world == NetworkModeWorldKind::Client) {
                CHECK(native.probe.module[0] == 0);
                CHECK(Internal::InspectHeadlessPerceptionMemory(*sceneService.Value(), *observer).HasError());
                const auto denied = Internal::InspectHeadlessDecisionValue(*sceneService.Value(), observerId, privateKey);
                REQUIRE(denied.HasError());
                CHECK(denied.ErrorValue().code.Value() == AI::AIErrors::RuntimeUnavailable.code.Value());
                return;
            }
            CHECK(Internal::InspectHeadlessDecisionValue(*sceneService.Value(), observerId, privateKey).HasValue());
            CHECK(native.probe.module[0] == 2);
            auto physics = service(NetworkModeServiceKind::Physics);
            REQUIRE(physics.HasValue());
            REQUIRE(physics.Value()->Prepare().HasValue());
            REQUIRE(physics.Value()->Activate().HasValue());
            const auto quantum = Duration::FromNanoseconds(native.execution.memory.fixedStep.count());
            REQUIRE(physics.Value()->RunFixedTick({1, quantum, cancellation.Token()}).HasValue());
            CHECK(native.probe.module[1] == 2);
            const auto memory = Internal::InspectHeadlessPerceptionMemory(*physics.Value(), *observer);
            REQUIRE(memory.HasValue());
            REQUIRE(memory.Value().count == 1);
            CHECK(memory.Value().entries.front().key.source == AI::ProjectPerceptionSource(*source));
            CHECK(memory.Value().entries.front().lastSensedTick == 1);
            if (usesAuthoredCoordinates)
                CHECK(memory.Value().entries.front().lastKnownPosition == Math::WorldCoordinate64::FromMillimeters(1100, 200, -300));
            sceneService.Value()->Shutdown();
            CHECK(Internal::InspectHeadlessPerceptionMemory(*physics.Value(), *observer).HasError());
            CHECK(native.probe.module[2] == 2);
            physics.Value()->Shutdown();
        }

        TEST_CASE("Durable Perception placement denies stale Scene revisions and ambiguous declarations",
                  "[prefab][integration][ai][perception][host]") {
            NativePerceptionFixture native;
            auto controller = AI::TestSupport::MakeFixture(1, 11, 1, 0);
            controller.descriptor.perceptionEndpoints = native.endpoints;
            const auto asset = Assets::AssetId::Parse("5c881920-450f-4ca7-b938-d151696ea419").Value();
            Runtime::SceneDefinitionBuilder builder{{7}, {42}};
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto source = ProjectPerceptionSource(asset, 42, controller.descriptor);
            const ErrorCodeDescriptor *expected = nullptr;
            SECTION("stale revision") {
                source = ProjectPerceptionSource(asset, 41, controller.descriptor);
                expected = &AI::AIErrors::PerceptionSpatialStale;
            }
            SECTION("duplicate root key") {
                source.insert(1, "\"schema\":1,");
                expected = &AI::AIErrors::PerceptionDescriptorInvalid;
            }
            SECTION("missing origin") {
                const auto begin = source.find("\"worldOriginMillimeters\":");
                const auto end = source.find("],", begin);
                REQUIRE(end != std::string::npos);
                source.erase(begin, end + 2 - begin);
                expected = &AI::AIErrors::PerceptionDescriptorInvalid;
            }
            REQUIRE(expected != nullptr);
            const auto captured = CaptureProjectPerception(definition.Value(), asset, source);
            REQUIRE(captured.HasError());
            CHECK(captured.ErrorValue().code.Value() == expected->code.Value());
        }

        /** @brief Returns a well-formed authored controller for which the captured catalog has no descriptor. */
        AI::AiControllerComponent UnavailableController(AI::AiControllerComponent component) {
            component.controller = AI::TestSupport::MakeIdentity<AI::ControllerTypeId>(999);
            return component;
        }

        /** @brief Encodes two actual authored controllers using the runtime prefab codec and typed Scene payload projection. */
        std::vector<std::uint8_t> EncodeDecisionPrefab(Assets::AssetId asset, const AI::TestSupport::ActivationFixture &controller,
                                                       const bool missingChildController) {
            Prefab::CookedPrefabEntity root;
            root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("decision fixture", 16)))};
            SceneSource::SceneObjectComponentSet values{.aiAgent = controller.agent.agent, .aiController = controller.agent.controller};
            root.members.emplace_back(Builtin(values, Kind::AiAgent, "aiAgent", 1));
            root.members.emplace_back(Builtin(values, Kind::AiController, "aiController", 2));

            auto child = root;
            child.parent = Prefab::CookedPrefabEntitySlot{0};
            child.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {7}).Value();
            values.aiAgent->agent = AI::TestSupport::MakeIdentity<AI::AgentId>(2);
            child.members.front() = Builtin(values, Kind::AiAgent, "aiAgent", 3);
            if (missingChildController) {
                values.aiController = UnavailableController(*values.aiController);
                child.members.back() = Builtin(values, Kind::AiController, "aiController", 4);
            }

            auto encoded = Prefab::CookedPrefab::Create({.assetId = asset, .entities = {std::move(root), std::move(child)}},
                                                        Prefab::PrefabLimitProfile::Create({}).Value());
            REQUIRE(encoded.HasValue());
            std::vector<std::uint8_t> bytes;
            for (const auto byte : encoded.Value().Bytes())
                bytes.push_back(std::to_integer<std::uint8_t>(byte));
            return bytes;
        }

        /** @brief Drives the real deferred Scene safe point until a native prefab operation reaches its terminal state. */
        void AwaitHeadlessAiPrefab(Network::INetworkModeService &physics, const Prefab::PrefabOperation &operation,
                                   const Prefab::PrefabSpawnState expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (operation.State() == Prefab::PrefabSpawnState::Pending && std::chrono::steady_clock::now() < deadline) {
                REQUIRE(physics.RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
                if (operation.State() == Prefab::PrefabSpawnState::Pending)
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(operation.State() == expected);
        }

        /** @brief Drives the product query owner across real cooked prefab retirement and replacement. */
        void RunSelectedQueryPrefab(Network::INetworkModeService &physics, NativeCookedDecisionProbe &probe,
                                    const Prefab::PrefabTemplateLoadRequest &source, const CancellationToken &cancellation,
                                    AI::BlackboardKeyId key) {
            using enum Prefab::PrefabSpawnState;
            const auto tick = [&](std::uint64_t frame) {
                const auto result = physics.RunFixedTick({frame, Duration::FromNanoseconds(16'666'667), cancellation});
                if (result.HasError())
                    UNSCOPED_INFO(result.ErrorValue().message);
                REQUIRE(result.HasValue());
            };
            tick(1);
            const auto capability = probe.PrefabCapability();
            REQUIRE(capability);
            const auto spawn = [&](std::uint64_t frame) {
                CAPTURE(frame);
                auto operation = capability->Spawn({.source = source, .notBeforeTick = frame});
                if (operation.HasError())
                    UNSCOPED_INFO(operation.ErrorValue().code.Value() << ": " << operation.ErrorValue().message);
                REQUIRE(operation.HasValue());
                AwaitHeadlessAiPrefab(physics, operation.Value(), Committed);
                auto instance = operation.Value().Spawned();
                REQUIRE(instance.HasValue());
                return std::move(instance).Value();
            };
            const auto agents = [&] {
                const auto scene = Internal::InspectHeadlessScene(physics);
                REQUIRE(scene);
                std::vector<AI::AgentId> ids;
                for (std::size_t slot = 0; slot < scene->SlotCount(); ++slot) {
                    const auto entity = scene->EntityAt(slot);
                    if (entity && entity->components->aiAgent)
                        ids.push_back(entity->components->aiAgent->agent);
                }
                REQUIRE(ids.size() == 2);
                return ids;
            };
            const auto expectUnset = [&](const std::vector<AI::AgentId> &ids) {
                for (const auto id : ids) {
                    const auto value = Internal::InspectHeadlessDecisionValue(physics, id, key);
                    REQUIRE(value.HasValue());
                    CHECK_FALSE(value.Value());
                }
            };
            const auto first = spawn(1);
            const auto retired = agents();
            tick(2);
            tick(3);
            expectUnset(retired);
            auto despawn = capability->Despawn(first, 3);
            REQUIRE(despawn.HasValue());
            AwaitHeadlessAiPrefab(physics, despawn.Value(), Committed);
            tick(4);
            for (const auto id : retired)
                CHECK(Internal::InspectHeadlessDecisionValue(physics, id, key).HasError());

            const auto replacement = spawn(4);
            CHECK(replacement.Root() != first.Root());
            const auto current = agents();
            tick(5);
            tick(6);
            expectUnset(current);
            for (std::uint64_t frame = 7; frame <= 9; ++frame)
                tick(frame);
            for (const auto id : current) {
                CHECK(std::ranges::find(retired, id) == retired.end());
                auto value = Internal::InspectHeadlessDecisionValue(physics, id, key);
                REQUIRE(value.HasValue());
                REQUIRE(value.Value());
                const auto &winner = std::get<Math::WorldCoordinate64>(std::get<AI::BlackboardScalarValue>(*value.Value()));
                CHECK(winner == Math::WorldCoordinate64::FromMillimeters(-500, 0, -500));
            }
            CHECK(probe.factories == 0);
            CHECK((probe.decision == std::array<std::uint32_t, 3>{}));
        }

        /** @brief Runs native task ownership through the real grouped spawn or atomic scheduler admission refusal. */
        void RunSelectedDecisionPrefab(Network::INetworkModeService &physics, NativeCookedDecisionProbe &probe,
                                       const Prefab::PrefabTemplateLoadRequest &source, const CancellationToken &cancellation,
                                       const bool rollback, const std::optional<AI::AiControllerComponent> &degradedController) {
            using enum Prefab::PrefabSpawnState;
            const auto tick = [&](const std::uint64_t frame) {
                REQUIRE(physics.RunFixedTick({frame, Duration::FromNanoseconds(16'666'667), cancellation}).HasValue());
            };
            tick(1);
            CHECK(probe.factories == 0);
            CHECK(probe.decision[0] == 0);
            const auto capability = probe.PrefabCapability();
            auto spawn = capability->Spawn({.source = source});
            REQUIRE(spawn.HasValue());
            AwaitHeadlessAiPrefab(physics, spawn.Value(), rollback ? Failed : Committed);
            tick(2);
            if (rollback) {
                REQUIRE(spawn.Value().Failure());
                bool capacityError = false;
                for (const auto *error = spawn.Value().Failure(); error; error = error->cause.Get())
                    capacityError = capacityError || error->code.Value() == AI::AIErrors::AgentCapacityExceeded.code.Value();
                CHECK(capacityError);
                CHECK(spawn.Value().Spawned().HasError());
                CHECK((probe.decision == std::array<std::uint32_t, 3>{}));
                return;
            }
            auto instance = spawn.Value().Spawned();
            REQUIRE(instance.HasValue());
            const auto taskCount = degradedController ? 1u : 2u;
            CHECK(probe.factories == taskCount);
            CHECK(probe.decision[0] == taskCount);
            std::optional<Runtime::EntityRef> degradedEntity;
            if (degradedController) {
                const auto scene = Internal::InspectHeadlessScene(physics);
                REQUIRE(scene);
                for (std::size_t slot = 0; slot < scene->SlotCount(); ++slot) {
                    const auto entity = scene->EntityAt(slot);
                    if (!entity || entity->parent != instance.Value().Root())
                        continue;
                    REQUIRE_FALSE(degradedEntity);
                    degradedEntity = entity->entity;
                    CHECK(entity->components->aiController == degradedController);
                    REQUIRE(entity->components->aiAgent);
                    CHECK(entity->components->aiAgent->enabled);
                }
                REQUIRE(degradedEntity);
            }
            auto despawn = capability->Despawn(instance.Value(), 2);
            REQUIRE(despawn.HasValue());
            AwaitHeadlessAiPrefab(physics, despawn.Value(), Committed);
            if (degradedEntity) {
                const auto scene = Internal::InspectHeadlessScene(physics);
                REQUIRE(scene);
                CHECK(scene->Get(*degradedEntity).HasError());
            }
            CHECK((probe.decision == std::array{taskCount, taskCount, taskCount}));
            tick(3);
            CHECK((probe.decision == std::array{taskCount, taskCount, taskCount}));
            CHECK(probe.factories == taskCount);
        }

        /** @brief Writes only the project metadata and sidecars needed to admit a committed scene in the real executable. */
        void WriteDecisionProductSources(const std::filesystem::path &root, std::span<const Assets::AssetRecord> records) {
            std::filesystem::create_directories(root / ".horo");
            std::filesystem::create_directories(root / "assets");
            std::ofstream metadata{root / ".horo/project.json"};
            metadata
                << R"({"horoVersion":"0.0.1","persistentContract":"sha256:0101010101010101010101010101010101010101010101010101010101010101","projectId":"p1","name":"Test","projectVersion":"0.1.0","createdAt":"2026-07-18T00:00:00Z","settings":{"renderBackend":"opengl"}})";
            REQUIRE(metadata.good());
            for (const auto &record : records) {
                std::ofstream source{root / record.sourcePath.String()};
                source << "Cooked fixture source";
                REQUIRE(source.good());
                std::ofstream sidecar{root / record.metadataPath.String()};
                sidecar << nlohmann::json{{"schemaVersion", 1}, {"assetId", record.id.ToString()}, {"assetType", record.type.Value()}};
                REQUIRE(sidecar.good());
            }
        }

        struct DecisionProductTerminalOptions final {
            std::optional<Assets::AssetId> prefab;
            std::string_view missingController;
            std::string_view perceptionPolicy;
            std::string_view refusal{"Selected cooked AI artifact has the wrong family."};
        };

        /** @brief Runs the real terminal executable and checks its bounded admission outcome. */
        void ExpectDecisionProductExit(const int expected, const std::filesystem::path &project, const std::filesystem::path &cooked,
                                       const Assets::AssetCookTargetId &target, Assets::AssetId scene,
                                       const Internal::GameplayWorldSelection &gameplay, AI::ControllerTypeId controller,
                                       Assets::AssetId decision, const DecisionProductTerminalOptions &options = {}) {
            const auto utf8 = [](const std::filesystem::path &path) {
                const auto value = path.generic_u8string();
                return std::string{value.begin(), value.end()};
            };
            std::string output;
            ExternalProcessRequest request{.executable = HORO_TEST_HEADLESS_ENGINE_PATH,
                                           .arguments = {"--run-network-product",
                                                         "standalone",
                                                         "2",
                                                         "--project",
                                                         utf8(project),
                                                         "--scene",
                                                         "cooked:" + scene.ToString(),
                                                         "--physics",
                                                         "null",
                                                         "--cooked-root",
                                                         utf8(cooked),
                                                         "--cook-target",
                                                         target.Value(),
                                                         "--game-module",
                                                         utf8(gameplay.nativeArtifact),
                                                         gameplay.moduleId,
                                                         std::to_string(gameplay.descriptorRevision),
                                                         "deny",
                                                         options.prefab ? "--ai-prefab-decision" : "--ai-decision",
                                                         std::to_string(controller.Value()),
                                                         decision.ToString()},
                                           .timeout = std::chrono::seconds{30},
                                           .onOutput = [&output](ProcessOutputLine line) {
                output += line.text + '\n';
            }};
            if (options.prefab)
                request.arguments.insert(request.arguments.end(), {options.prefab->ToString(), "--prefab", "grant"});
            if (!options.missingController.empty())
                request.arguments.insert(request.arguments.end(), {"--ai-missing-controller", std::string{options.missingController}});
            if (!options.perceptionPolicy.empty())
                request.arguments.insert(request.arguments.end(), {"--game-perception-policy", std::string{options.perceptionPolicy}});
            auto result = NativeExternalProcessRunner{}.Run(request, {});
            INFO(output);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().reason == ProcessTerminationReason::Exited);
            CHECK(result.Value().exitCode == expected);
            const std::string_view diagnostic = expected == 0 ? "network product completed 2 frames" : options.refusal;
            CHECK(output.find(diagnostic) != std::string::npos);
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
            const auto sceneId = Assets::AssetId::Parse("fc881920-450f-4ca7-b938-d151696ea420").Value();
            const auto prefabId = Assets::AssetId::Parse("dc881920-450f-4ca7-b938-d151696ea420").Value();
            auto artifactType = Output::Type("core.behavior_tree");
            auto requestedId = artifactId;
            auto declaredRoot = graphSource.asset;
            const ErrorCodeDescriptor *expected = nullptr;
            std::optional<std::size_t> executionBudget;
            bool malformed = false;
            bool loadSchemaFromCook = false;
            bool schemaArtifactPresent = false;
            bool malformedSchema = false;
            bool staleProvider = false;
            bool sceneDeclaresController = false;
            bool conflictingSceneDeclaration = false;
            bool wrongAuthoredSchema = false;
            bool unknownController = false;
            bool duplicateSelection = false;
            bool prepareNative = false;
            bool cancelBeforeNativeActivation = false;
            bool runTerminal = false;
            bool captureReplay = false;
            bool abortReplay = false;
            std::optional<std::uint32_t> replayReferenceGeneration;
            bool selectNativePerception = false;
            std::optional<AI::PerceptionSenseContract> nativeSense;
            bool prefabOnly = false;
            bool prefabQuery = false;
            bool rollbackPrefab = false;
            bool preserveInactive = false;
            auto selectedPrefab = prefabId;
            SECTION("standalone captures one selected decision only after the real fixed commit") {
                executionBudget = 100;
                captureReplay = true;
            }
            SECTION("standalone rejection after replay preparation cannot publish the attempted tick") {
                executionBudget = 100;
                captureReplay = true;
                abortReplay = true;
            }
            SECTION("standalone replay encodes a live authored Blackboard entity by its stable identity") {
                executionBudget = 100;
                captureReplay = true;
                replayReferenceGeneration = 1;
            }
            SECTION("standalone replay rejects a Blackboard entity with a stale slot generation") {
                executionBudget = 100;
                captureReplay = true;
                replayReferenceGeneration = 2;
            }
            SECTION("degraded prefab preserves a missing child controller while only its registered sibling obtains a native task") {
                prepareNative = true;
                prefabOnly = true;
                preserveInactive = true;
            }
            SECTION("terminal missing-controller policy preserves authored Scene data and validates explicitly selected assets") {
                prepareNative = true;
                runTerminal = true;
                prefabOnly = true;
                preserveInactive = true;
            }
            SECTION("terminal degraded policy rejects an explicitly selected malformed decision asset") {
                prepareNative = true;
                runTerminal = true;
                prefabOnly = true;
                preserveInactive = true;
                malformed = true;
            }
            SECTION("prefab-only selection refuses a committed asset of another type") {
                prepareNative = true;
                prefabOnly = true;
                selectedPrefab = artifactId;
                expected = &Prefab::PrefabErrors::DependencyTypeMismatch;
            }
            SECTION("prefab-only selection requires an actual matching typed occurrence") {
                prepareNative = true;
                prefabOnly = true;
                unknownController = true;
                expected = &AI::AIErrors::ControllerDescriptorMissing;
            }
            SECTION("terminal accepts an exact prefab-only controller without an initial Scene agent") {
                prepareNative = true;
                runTerminal = true;
                prefabOnly = true;
            }
            SECTION("native prefab-only controller spawns two running decisions and retires both exact tasks on despawn") {
                prepareNative = true;
                prefabOnly = true;
            }
            SECTION("a cooked prefab query awaiting publication retires without writing to replacement agents") {
                prepareNative = true;
                prefabOnly = true;
                prefabQuery = true;
            }
            SECTION("native prefab-only scheduler refusal rolls back the entire spawn without dispatch") {
                prepareNative = true;
                prefabOnly = true;
                rollbackPrefab = true;
            }
            SECTION("prefab-only cooked plan rejects a mismatched authored blackboard") {
                prepareNative = true;
                prefabOnly = true;
                wrongAuthoredSchema = true;
                expected = &AI::AIErrors::ControllerDescriptorIncompatible;
            }
            SECTION("terminal admits authored Perception through the selected module and refuses a finite undersized budget") {
                prepareNative = true;
                runTerminal = true;
                selectNativePerception = true;
            }
            SECTION("selected prepared image freezes inert Perception before and binds one real native provider after startup") {
                prepareNative = true;
                selectNativePerception = true;
            }
            SECTION("actual terminal process publishes the selected cooked native decision and rejects the wrong artifact type") {
                prepareNative = true;
                runTerminal = true;
            }
            SECTION("selected native provider starts only after the real paired Physics and Character activation") {
                prepareNative = true;
            }
            SECTION("cancellation after native metadata qualification invokes no native module or decision callback") {
                prepareNative = true;
                cancelBeforeNativeActivation = true;
            }
            if (prepareNative) {
                executionBudget = 64;
                loadSchemaFromCook = true;
                schemaArtifactPresent = true;
                sceneDeclaresController = true;
                graphSource = Decisions::TaskTree(41, 101);
                graphSource.executionTask = Decisions::MakeIdentity<AI::TaskId>(879);
            }
            SECTION("exact cooked root") {}
            SECTION("owner runs opted-in decision through the product fixed tick") {
                executionBudget = 64;
            }
            SECTION("owner resolves schema from authenticated cooked catalog and runs provider") {
                executionBudget = 64;
                loadSchemaFromCook = true;
                schemaArtifactPresent = true;
            }
            SECTION("authored Scene and explicit project asset select a runnable controller without a separate descriptor") {
                executionBudget = 64;
                loadSchemaFromCook = true;
                schemaArtifactPresent = true;
                sceneDeclaresController = true;
            }
            SECTION("selected authored controller cannot name two different decisions") {
                sceneDeclaresController = true;
                conflictingSceneDeclaration = true;
                expected = &AI::AIErrors::DescriptorConflict;
            }
            SECTION("exact cooked schema must match the authored Scene binding") {
                sceneDeclaresController = true;
                wrongAuthoredSchema = true;
                expected = &AI::AIErrors::ControllerDescriptorIncompatible;
            }
            SECTION("selection without either a descriptor or Scene declaration is refused") {
                sceneDeclaresController = true;
                unknownController = true;
                expected = &AI::AIErrors::ControllerDescriptorMissing;
            }
            SECTION("duplicate explicit mappings are refused before any provider is created") {
                duplicateSelection = true;
                expected = &AI::AIErrors::DescriptorConflict;
            }
            SECTION("committed schema catalog refuses missing semantic definition") {
                loadSchemaFromCook = true;
                expected = &AI::AIErrors::DecisionAssetSchemaMissing;
            }
            SECTION("authenticated but malformed schema payload refuses admission") {
                loadSchemaFromCook = true;
                schemaArtifactPresent = true;
                malformedSchema = true;
                expected = &AI::AIErrors::BlackboardSchemaInvalid;
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

            constexpr std::uint64_t QueryKeyValue = 4;
            const auto queryKey = Decisions::MakeIdentity<AI::BlackboardKeyId>(QueryKeyValue);
            std::shared_ptr<const AI::QuerySchemaRegistry> queryRegistry;
            std::vector<std::uint8_t> queryBytes;
            if (replayReferenceGeneration) {
                const std::array keys{
                    AI::BlackboardKeyDescriptor{.key = Decisions::MakeIdentity<AI::BlackboardKeyId>(1),
                                                .kind = AI::BlackboardValueKind::EntityReference,
                                                .defaultValue = AI::BlackboardScalarValue{
                                                    AI::BlackboardStoredEntityReference{7, 1, *replayReferenceGeneration}}}};
                auto schema = AI::BlackboardSchema::Capture({.identity = Decisions::SchemaId(), .keys = keys});
                REQUIRE(schema.HasValue());
                ai.schema = std::make_shared<const AI::BlackboardSchema>(std::move(schema).Value());
            }
            if (prefabQuery) {
                auto registry = AI::CaptureBuiltinQuerySchema();
                REQUIRE(registry.HasValue());
                const auto query =
                    AI::CreateBuiltinLatticeQuery(Decisions::MakeIdentity<AI::QueryId>(1), Decisions::MakeIdentity<AI::QueryStageId>(1),
                                                  Decisions::MakeIdentity<AI::QueryResultSchemaId>(1), "Prefab query",
                                                  {.stepMillimeters = 500, .outerSteps = 1}, 8);
                REQUIRE(query.HasValue());
                auto encoded = AI::EnvironmentQueryCookedCodec::Encode(query.Value(), registry.Value());
                REQUIRE(encoded.HasValue());
                queryBytes = std::move(encoded).Value();
                queryRegistry = std::make_shared<const AI::QuerySchemaRegistry>(std::move(registry).Value());

                const std::array keys{AI::BlackboardKeyDescriptor{.key = queryKey,
                                                                  .kind = AI::BlackboardValueKind::WorldCoordinate,
                                                                  .presence = AI::BlackboardKeyPresence::Optional}};
                auto schema = AI::BlackboardSchema::Capture({.identity = Decisions::SchemaId(), .keys = keys});
                REQUIRE(schema.HasValue());
                ai.schema = std::make_shared<const AI::BlackboardSchema>(std::move(schema).Value());

                auto task = Decisions::TreeNode(2, AI::DecisionControlKind::Task);
                task.type = Decisions::MakeIdentity<AI::BehaviorTreeNodeTypeId>(102);
                task.requirements = {{.key = queryKey,
                                      .kind = AI::BlackboardValueKind::WorldCoordinate,
                                      .access = AI::BlackboardKeyAccess::ReadWrite,
                                      .source = Decisions::Location()}};
                const auto &result = query.Value().ResultSchema();
                const AI::BehaviorTreeQueryOutput output{query.Value().Id(), result.id, result.version, result.itemType, queryKey, 10};
                task.properties.emplace_back(Decisions::MakeIdentity<AI::BehaviorTreePropertyId>(10), "output",
                                             AI::BehaviorTreePropertyKind::QueryOutput, output);
                const auto executionTask = graphSource.executionTask;
                graphSource = Decisions::Tree(41, {Decisions::TreeNode(1, AI::DecisionControlKind::Root), std::move(task)},
                                              {Decisions::Edge(1, 1, 2)});
                graphSource.executionTask = executionTask;
            }

            std::optional<Internal::GameplayWorldSelection> gameplay;
            std::optional<NativeCookedDecisionProbe> nativeProbe;
            auto environment = ai.Environment();
            std::unique_ptr<Gameplay::PreparedGameModuleImage> cookImage;
            if (prepareNative) {
                gameplay.emplace();
                gameplay->moduleId = "game.ai.tests";
                gameplay->nativeArtifact = HORO_TEST_AI_MODULE_PATH;
                gameplay->moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
                if (prefabOnly)
                    gameplay->prefabPermission = Internal::GameplayPrefabPermission::Granted;
                std::ifstream revision{HORO_TEST_AI_MODULE_REVISION_PATH};
                REQUIRE((revision >> gameplay->descriptorRevision));
                nativeProbe.emplace();
                auto image = Gameplay::GameModuleHost::PrepareImage(gameplay->nativeArtifact,
                                                                    {gameplay->moduleId, Gameplay::CurrentGameplayBuildFingerprint(),
                                                                     gameplay->descriptorRevision});
                REQUIRE(image.HasValue());
                auto nodes = image.Value()->AcquireDecisionNodes();
                REQUIRE(nodes.HasValue());
                environment.nodes = std::move(nodes).Value();
                if (selectNativePerception) {
                    const auto metadata = image.Value()->DescribePerceptionSenses();
                    REQUIRE(metadata.HasValue());
                    REQUIRE(metadata.Value().size() == 1);
                    nativeSense = metadata.Value().front();
                    CHECK((nativeProbe->module == std::array<std::uint32_t, 4>{}));
                }
                cookImage = std::move(image).Value();
            }
            auto compiled = AI::DecisionGraphCompiler::Compile(graphSource.asset, std::span{&graphSource, 1}, environment);
            REQUIRE(compiled.HasValue());
            auto plan = std::move(compiled).Value().plan;
            REQUIRE(plan);
            auto encoded = AI::DecisionPlanCodec::EncodeCooked(*plan);
            environment.nodes.reset();
            cookImage.reset();
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
            std::vector<std::vector<std::uint8_t>> artifacts{std::move(envelope).Value()};
            std::vector manifest{Assets::AssetCookManifestEntry{artifactId, artifactType, artifactId.ToString() + ".cooked", digest}};
            std::vector records{Assets::AssetRecord{artifactId, artifactType, ProjectPath::Parse("assets/guard.horo_bt").Value(),
                                                    ProjectPath::Parse("assets/guard.horo_bt.horo").Value()}};
            if (queryRegistry) {
                const auto queryAsset = Assets::AssetId::FromBytes({105});
                const auto queryType = Output::Type(AI::EnvironmentQuerySourceAssetType);
                const auto queryDigest = Put(raw, queryAsset, queryType, target, std::move(queryBytes));
                auto queryEnvelope = raw.Load(queryAsset, {});
                REQUIRE(queryEnvelope.HasValue());
                artifacts.push_back(std::move(queryEnvelope).Value());
                manifest.push_back({queryAsset, queryType, queryAsset.ToString() + ".cooked", queryDigest});
                records.push_back({queryAsset, queryType, ProjectPath::Parse("assets/position.horo_eqs").Value(),
                                   ProjectPath::Parse("assets/position.horo_eqs.horo").Value()});
            }
            auto controller = CookedController(ai, plan);
            controller.agent.controller->decisionAsset = declaredRoot;
            controller.descriptor.decisionAsset = declaredRoot;
            controller.descriptor.decisionPlan.reset();
            controller.descriptor.cookedPlan.reset();
            if (loadSchemaFromCook)
                controller.descriptor.blackboardSchema.reset();
            if (wrongAuthoredSchema)
                controller.agent.controller->blackboardSchema = Decisions::MakeIdentity<AI::BlackboardSchemaId>(903);

            Prefab::PrefabTemplateLoadRequest prefabSource{prefabId, {}, target};
            if (prefabOnly) {
                const auto prefabType = Output::Type("core.prefab");
                prefabSource.artifactDigest =
                    Put(raw, prefabId, prefabType, target, EncodeDecisionPrefab(prefabId, controller, preserveInactive && !runTerminal));
                auto cookedPrefab = raw.Load(prefabId, {});
                REQUIRE(cookedPrefab.HasValue());
                artifacts.push_back(std::move(cookedPrefab).Value());
                manifest.push_back({prefabId, prefabType, prefabId.ToString() + ".cooked", prefabSource.artifactDigest});
                records.push_back({prefabId, prefabType, ProjectPath::Parse("assets/crew.prefab").Value(),
                                   ProjectPath::Parse("assets/crew.prefab.horo").Value()});
            }
            if (schemaArtifactPresent) {
                const auto schemaId = Assets::AssetId::Parse("e5881920-450f-4ca7-b938-d151696ea419").Value();
                const auto schemaType = Output::Type(AI::BlackboardSchemaAssetType);
                auto schema = AI::BlackboardSchemaCodec::Encode(*ai.schema);
                REQUIRE(schema.HasValue());
                auto schemaBytes = std::move(schema).Value();
                if (malformedSchema)
                    schemaBytes.resize(3);
                const auto schemaDigest = Put(raw, schemaId, schemaType, target, std::move(schemaBytes));
                auto schemaEnvelope = raw.Load(schemaId, {});
                REQUIRE(schemaEnvelope.HasValue());
                artifacts.push_back(std::move(schemaEnvelope).Value());
                manifest.push_back({schemaId, schemaType, schemaId.ToString() + ".cooked", schemaDigest});
                records.push_back({schemaId, schemaType, ProjectPath::Parse("assets/guard.horo_bb").Value(),
                                   ProjectPath::Parse("assets/guard.horo_bb.horo").Value()});
            }
            if (selectNativePerception)
                controller.agent.controller->requiredCapabilities =
                    controller.agent.controller->requiredCapabilities.Union(AI::AiCapabilitySet::Of(AI::AiCapability::Perception));
            Runtime::SceneDefinitionBuilder source{SceneCook::SceneDefinitionIdForAsset(sceneId), {1}};
            if (prefabOnly) {
                source.Add({.object = {1}, .components = {.behaviors = {{{1}, BehaviorType("game.ai.tests.prefab_capability_probe")}}}});
            } else {
                source.Add({.object = {1}, .components = {.aiAgent = controller.agent.agent, .aiController = controller.agent.controller}});
            }
            if (replayReferenceGeneration)
                source.Add({.object = {2}});
            if (selectNativePerception) {
                auto peer = controller.agent.agent;
                peer.agent = Decisions::MakeIdentity<AI::AgentId>(2);
                source.Add({.object = {2},
                            .localTransform = {.translation = {1.0F, 0.0F, 0.0F}},
                            .components = {.aiAgent = peer, .aiController = controller.agent.controller}});
            }
            if (preserveInactive && runTerminal)
                source.Add({.object = {2},
                            .components = {.aiAgent = controller.agent.agent,
                                           .aiController = UnavailableController(*controller.agent.controller)}});
            if (conflictingSceneDeclaration) {
                auto conflicting = controller.agent;
                conflicting.agent.agent = Decisions::MakeIdentity<AI::AgentId>(2);
                conflicting.controller->decisionAsset = Decisions::Graph(55);
                source.Add({.object = {2}, .components = {.aiAgent = conflicting.agent, .aiController = conflicting.controller}});
            }
            auto definition = std::move(source).Build();
            REQUIRE(definition.HasValue());
            nlohmann::json perception;
            if (selectNativePerception) {
                constexpr std::uint32_t authoredListener = 91;
                const auto sense = nativeSense->sense.identity.Value();
                const auto units = nativeSense->workUnits + AI::PerceptionSenseGeneration::ValidationWorkUnits + 1;
                const nlohmann::json endpoint{{"controller", controller.descriptor.controller.Value()},
                                              {"listeners", {authoredListener}},
                                              {"sources", {sense}},
                                              {"layers", 1},
                                              {"mask", 1}};
                const nlohmann::json listener{{"id", authoredListener}, {"sense", sense}};
                perception =
                    {{"schema", 2},
                     {"sceneAsset", sceneId.ToString()},
                     {"sceneRevision", definition.Value().Revision().value},
                     {"worldOriginMillimeters", {100, 200, -300}},
                     {"controllers", {endpoint}},
                     {"native",
                      {{"listeners", {listener}}, {"radiusMillimeters", 4000}, {"visibleLayers", 1}, {"workUnitsPerTick", units * 2}}}};
            }
            Output::TempDir directory;
            if (runTerminal) {
                const auto sceneType = Output::Type("core.scene");
                auto bytes = SceneCook::EncodeCookedSceneDefinition(definition.Value());
                REQUIRE(bytes.HasValue());
                const auto sceneDigest = Put(raw, sceneId, sceneType, target, std::move(bytes).Value());
                auto cookedScene = raw.Load(sceneId, {});
                REQUIRE(cookedScene.HasValue());
                artifacts.push_back(std::move(cookedScene).Value());
                manifest.push_back({sceneId, sceneType, sceneId.ToString() + ".cooked", sceneDigest});
                records.push_back({sceneId, sceneType, ProjectPath::Parse("assets/level.horo").Value(),
                                   ProjectPath::Parse("assets/level.horo.horo").Value()});
                const auto published = Output::PublishFixture(directory.path, target, manifest, artifacts);
                if (published.HasError())
                    UNSCOPED_INFO(published.ErrorValue().message);
                REQUIRE(published.HasValue());
                Output::TempDir project;
                WriteDecisionProductSources(project.path, records);
                DecisionProductTerminalOptions options{.prefab = prefabOnly ? std::optional{prefabId} : std::nullopt};
                if (selectNativePerception) {
                    options.perceptionPolicy = "perception.json";
                    const auto write = [&] {
                        std::ofstream stream{project.path / options.perceptionPolicy};
                        stream << perception;
                        REQUIRE(stream.good());
                    };
                    write();
                    ExpectDecisionProductExit(0, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                              artifactId, options);
                    const auto admittedBudget = perception["native"]["workUnitsPerTick"];
                    perception["native"]["workUnitsPerTick"] = 1;
                    write();
                    options.refusal = AI::AIErrors::SchedulerBudgetExhausted.summary;
                    ExpectDecisionProductExit(3, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                              artifactId, options);
                    perception["native"]["workUnitsPerTick"] = admittedBudget;
                    perception["native"]["listeners"][0]["sense"] = nativeSense->sense.identity.Value() + 1;
                    write();
                    options.refusal = "selected native module did not";
                    ExpectDecisionProductExit(3, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                              artifactId, options);
                    return;
                }
                if (malformed) {
                    options.missingController = "degrade";
                    options.refusal = AI::AIErrors::DecisionAssetSchemaInvalid.summary;
                    ExpectDecisionProductExit(3, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                              artifactId, options);
                    return;
                }
                if (preserveInactive) {
                    auto strict = options;
                    strict.missingController = "reject";
                    strict.refusal = AI::AIErrors::ControllerDescriptorMissing.summary;
                    ExpectDecisionProductExit(3, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                              artifactId, strict);
                    options.missingController = "degrade";
                }
                ExpectDecisionProductExit(0, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                          artifactId, options);
                ExpectDecisionProductExit(3, project.path, directory.path, target, sceneId, *gameplay, controller.descriptor.controller,
                                          sceneId, options);
                return;
            }
            REQUIRE(Output::PublishFixture(directory.path, target, manifest, artifacts).HasValue());
            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            auto committed =
                Internal::CommittedSceneAssets::Capture(NativeDurableFileSystem{}, {directory.path, target}, registry.Snapshot());
            REQUIRE(committed.HasValue());
            JobSystem jobs{{1, 8}};
            Assets::AssetLoadService loader{jobs, committed.Value()->SceneProvider()};
            Assets::AssetLoadService prefabLoader{jobs, committed.Value()->ArtifactProvider()};
            CancellationSource startup;
            const Internal::HeadlessSceneLoadBoundary load{startup.Token(), std::chrono::steady_clock::now() + std::chrono::seconds{5}};
            const std::array catalog{controller.descriptor};
            const auto selectedController =
                unknownController ? Decisions::MakeIdentity<AI::ControllerTypeId>(999) : controller.descriptor.controller;
            auto selected = Internal::HeadlessCookedDecisionInput{*committed.Value(),
                                                                  ai.Environment(),
                                                                  {{selectedController, requestedId,
                                                                    prefabOnly ? std::optional{selectedPrefab} : std::nullopt}}};
            if (duplicateSelection)
                selected.bindings.push_back({selectedController, requestedId});
            selected.querySchema = queryRegistry;
            if (loadSchemaFromCook)
                selected.environment.schemas.clear();
            if (prepareNative) {
                selected.environment.nodes.reset();
                selected.provider = Internal::HeadlessDecisionProvider::SelectedNativeGameplay;
                plan.reset();
            }
            AI::AiTaskSchedulerSettings schedulerSettings;
            if (rollbackPrefab) {
                schedulerSettings.maximumAgents = 1;
                schedulerSettings.evaluationsPerTick = 1;
            }
            if (executionBudget)
                selected.execution.emplace(
                    AI::AiSceneCookedDecisionExecution{jobs, schedulerSettings, {.workUnitsPerTick = *executionBudget}});
            const auto decisions = std::make_shared<const Internal::HeadlessCookedDecisionInput>(std::move(selected));
            Internal::HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                                         .settings = Physics::Settings(),
                                                         .registry = registry,
                                                         .loads = loader,
                                                         .load = load,
                                                         .aiControllers =
                                                             std::span{catalog}.first(sceneDeclaresController ? 0 : catalog.size()),
                                                         .aiSettings = {.missingController =
                                                                            preserveInactive
                                                                                ? AI::AiMissingControllerPolicy::PreserveInactive
                                                                                : AI::AiMissingControllerPolicy::Reject},
                                                         .prefabLoads = prefabOnly ? &prefabLoader : nullptr,
                                                         .cookedDecisions = decisions};
            if (selectNativePerception) {
                auto captured = CaptureProjectPerception(definition.Value(), sceneId, perception.dump());
                REQUIRE(captured.HasValue());
                input.perceptionProject =
                    Internal::HeadlessPerceptionProjectAdmission{.policy = std::move(captured).Value(),
                                                                 .provider = Internal::HeadlessPerceptionProvider::SelectedNativeGameplay};
            }
            auto scene = std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(definition).Value());
            if (captureReplay) {
                ExpectDecisionReplay(std::move(scene), std::move(input), controller.agent.agent.agent, plan->Digest(), abortReplay,
                                     replayReferenceGeneration);
                return;
            }
            const auto composed = Internal::ComposeHeadlessSceneNetworkServices(std::move(scene), std::move(input), {}, gameplay);
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
                if (nativeProbe) {
                    CHECK((nativeProbe->module == std::array<std::uint32_t, 4>{}));
                    CHECK(nativeProbe->decision[0] == 0);
                }
                if (cancelBeforeNativeActivation) {
                    startup.RequestCancellation();
                    REQUIRE(physics.Value()->Activate().HasError());
                } else {
                    REQUIRE(physics.Value()->Activate().HasValue());
                }
                if (selectNativePerception) {
                    CHECK((nativeProbe->module == std::array<std::uint32_t, 4>{2, 0, 0, 1}));
                    const auto scene = Internal::InspectHeadlessScene(*physics.Value());
                    REQUIRE(scene);
                    REQUIRE(physics.Value()->RunFixedTick({1, Duration::FromNanoseconds(16'666'667), startup.Token()}).HasValue());
                    CHECK((nativeProbe->module == std::array<std::uint32_t, 4>{2, 2, 0, 1}));
                    const auto recipient = scene->Find({1});
                    REQUIRE(recipient);
                    const auto memory = Internal::InspectHeadlessPerceptionMemory(*physics.Value(), *recipient);
                    REQUIRE(memory.HasValue());
                    REQUIRE(memory.Value().count == 1);
                    CHECK(memory.Value().entries.front().lastKnownPosition == Math::WorldCoordinate64::FromMillimeters(1100, 200, -300));
                } else if (nativeProbe && prefabQuery) {
                    RunSelectedQueryPrefab(*physics.Value(), *nativeProbe, prefabSource, startup.Token(), queryKey);
                } else if (nativeProbe && prefabOnly) {
                    CHECK(nativeProbe->module[3] == 1);
                    const auto degraded =
                        preserveInactive ? std::optional{UnavailableController(*controller.agent.controller)} : std::nullopt;
                    RunSelectedDecisionPrefab(*physics.Value(), *nativeProbe, prefabSource, startup.Token(), rollbackPrefab, degraded);
                } else if (nativeProbe && !cancelBeforeNativeActivation) {
                    CHECK(nativeProbe->module[3] == 1);
                    CHECK(nativeProbe->decision[0] == 0);
                    const Runtime::FixedStepContext fixed{1, Duration::FromNanoseconds(16'666'667), startup.Token()};
                    REQUIRE(physics.Value()->RunFixedTick(fixed).HasValue());
                    CHECK(nativeProbe->decision[0] == 1);
                }
                if (executionBudget && !prepareNative) {
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
            const bool ranCaptured = executionBudget && !expected && !prepareNative;
            CHECK(ai.probe.factories == (ranCaptured ? 1 : 0));
            built.Value()->Shutdown();
            CHECK(ai.probe.aborts == (ranCaptured ? 1 : 0));
            CHECK(ai.probe.cleanups == (ranCaptured ? 1 : 0));
            if (nativeProbe) {
                const auto modules = cancelBeforeNativeActivation || expected ? 0u : 1u;
                const auto activeDecisions = selectNativePerception || (prefabOnly && !preserveInactive) ? 2u : 1u;
                const auto tasks = modules == 0 || rollbackPrefab || prefabQuery ? 0u : activeDecisions;
                CHECK(nativeProbe->module[3] == modules);
                if (selectNativePerception)
                    CHECK((nativeProbe->module == std::array<std::uint32_t, 4>{2, 2, 2, 1}));
                CHECK((nativeProbe->decision == std::array{tasks, tasks, tasks}));
                REQUIRE(gameplay->moduleRetirements->Close().HasValue());
            }
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

        TEST_CASE("Native Perception instances roll back and retire with actual cooked prefab aggregate owners",
                  "[prefab][integration][atomic][ai][perception][native]") {
            AI::PerceptionSpatialBroadphase spatial;
            NativePerceptionFixture native;
            const AI::AiScenePerceptionActivationInput initial{spatial, {}, PerceptionPositions, native.execution};
            bool reject = false;
            SECTION("published agents lose exact source memory and release their providers at despawn") {}
            SECTION("a later Gameplay owner rejects the aggregate and destroys only detached candidate providers") {
                reject = true;
            }
            const AI::AiScenePerceptionGroupInput group{spatial,
                                                        {},
                                                        [](const Runtime::RuntimeSceneView &,
                                                           const std::span<const Runtime::RuntimeEntityView> prospective) {
                std::vector<AI::AiScenePerceptionPosition> positions;
                for (const auto &entity : prospective)
                    positions.push_back({entity.entity, Math::WorldCoordinate64::FromMillimeters(entity.entity.entity.index * 1000, 0, 0)});
                return Result<std::vector<AI::AiScenePerceptionPosition>>::Success(std::move(positions));
            }};
            Fixture fixture{true,
                            reject ? Fault::GameplayDescriptor : Fault::None,
                            {.requirePerceptionEndpoints = true},
                            native.endpoints,
                            group,
                            nullptr,
                            {},
                            initial};
            const auto baseline = *fixture.scenes.ActiveScene();
            const auto resident = *baseline.Find({1});
            const auto quantum = Duration::FromNanoseconds(native.execution.memory.fixedStep.count());
            CHECK(native.probe.module[0] == 1);
            fixture.Tick(1);
            if (reject) {
                REQUIRE(fixture.AwaitRejection().Failure());
                CHECK(baseline.IsCurrent());
                CHECK(native.probe.module[0] == 3);
                CHECK(native.probe.module[1] == 0);
                CHECK(native.probe.module[2] == 2);
                REQUIRE(fixture.ai.PollPerceptionAtSensePoll(baseline, spatial, 1, quantum).HasValue());
                CHECK(native.probe.module[1] == 1);
                return;
            }
            fixture.Reach(3);
            const auto spawned = *fixture.scenes.ActiveScene();
            CHECK(native.probe.module[0] == 3);
            REQUIRE(fixture.ai.PollPerceptionAtSensePoll(spawned, spatial, 1, quantum).HasValue());
            REQUIRE(fixture.ai.PollPerceptionAtSensePoll(spawned, spatial, 1, quantum).HasValue());
            CHECK(native.probe.module[1] == 3);
            const auto observed = fixture.ai.ReadPerceptionAtSafePoint(spawned, resident);
            REQUIRE(observed.HasValue());
            REQUIRE(observed.Value().count == 1);
            const auto source = observed.Value().entries.front().key.source;
            const Runtime::EntityRef former{spawned.RuntimeId(), {source.slot, source.generation}};
            const auto prefabMemory = fixture.ai.ReadPerceptionAtSafePoint(spawned, former);
            REQUIRE(prefabMemory.HasValue());
            CHECK(prefabMemory.Value().count == 1);
            fixture.Tick(2);
            fixture.Reach(1);
            const auto retired = *fixture.scenes.ActiveScene();
            CHECK(native.probe.module[2] == 2);
            CHECK(fixture.ai.ReadPerceptionAtSafePoint(retired, former).HasError());
            const auto survivor = fixture.ai.ReadPerceptionAtSafePoint(retired, resident);
            REQUIRE(survivor.HasValue());
            CHECK(survivor.Value().count == 0);
            REQUIRE(fixture.ai.PollPerceptionAtSensePoll(retired, spatial, 2, quantum).HasValue());
            CHECK(native.probe.module[1] == 4);
            fixture.scenes.Shutdown();
            CHECK(native.probe.module[2] == 3);
        }

        TEST_CASE("Committed cooked prefab despawn frees stimulus storage before the next native sense pass",
                  "[prefab][integration][atomic][ai][perception][retirement]") {
            constexpr Runtime::SceneObjectId listenerId{1};
            constexpr Runtime::SceneObjectId witnessId{2};
            constexpr Runtime::SceneObjectId replacementId{3};
            AI::PerceptionSpatialBroadphase spatial;
            NativePerceptionFixture native;
            native.execution.memory.profileMaximumEntries = 2;
            native.execution.workUnitsPerTick = native.RequiredUnits() * 6;
            bool witnessInRange = true;
            const auto position = [&](const Runtime::RuntimeEntityView &entity) {
                std::int64_t x = 1000;
                if (entity.authoredObject == listenerId)
                    x = 0;
                else if ((entity.authoredObject == witnessId && !witnessInRange) || entity.parent)
                    x = 5000;
                return AI::AiScenePerceptionPosition{entity.entity, Math::WorldCoordinate64::FromMillimeters(x, 0, 0)};
            };
            const auto positions = [&](const Runtime::RuntimeSceneView &scene) {
                std::vector<AI::AiScenePerceptionPosition> result;
                for (std::size_t index = 0; index < scene.SlotCount(); ++index)
                    if (const auto entity = scene.EntityAt(index); entity && entity->components->aiController)
                        result.push_back(position(*entity));
                return Result<std::vector<AI::AiScenePerceptionPosition>>::Success(std::move(result));
            };
            const AI::AiScenePerceptionActivationInput initial{spatial, {}, positions, native.execution};
            const AI::AiScenePerceptionGroupInput group{spatial,
                                                        {},
                                                        [&](const Runtime::RuntimeSceneView &,
                                                            const std::span<const Runtime::RuntimeEntityView> prospective) {
                std::vector<AI::AiScenePerceptionPosition> result;
                for (const auto &entity : prospective)
                    result.push_back(position(entity));
                return Result<std::vector<AI::AiScenePerceptionPosition>>::Success(std::move(result));
            }};
            auto fault = Fault::None;
            SECTION("published despawn retains the older unrelated memory under a tight capacity") {}
            SECTION("cancelled despawn preserves both observations") {
                fault = Fault::CancelRetirement;
            }
            Fixture fixture{true, fault, {.requirePerceptionEndpoints = true}, native.endpoints, group, nullptr, {}, initial};
            const auto addSource = [&](const Runtime::SceneObjectId object, const std::uint64_t agentId) {
                Runtime::SceneCommandBuffer pending;
                auto agent = fixture.controller.agent.agent;
                agent.agent = AI::TestSupport::MakeIdentity<AI::AgentId>(agentId);
                (void)pending.Create(
                    {.authoredObject = object, .components = {.aiAgent = agent, .aiController = fixture.controller.agent.controller}});
                REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(pending)).HasValue());
            };
            const auto poll = [&](const std::uint64_t tick) {
                const auto active = fixture.scenes.ActiveScene();
                REQUIRE(active);
                const auto fixedStep = Duration::FromNanoseconds(native.execution.memory.fixedStep.count());
                REQUIRE(fixture.ai.PollPerceptionAtSensePoll(*active, spatial, tick, fixedStep).HasValue());
            };

            addSource(witnessId, 2);
            fixture.Reach(2);
            const auto initialScene = *fixture.scenes.ActiveScene();
            const auto listener = initialScene.Find(listenerId);
            const auto witness = initialScene.Find(witnessId);
            REQUIRE(listener);
            REQUIRE(witness);
            const auto capability = fixture.ai.CapabilityAtSafePoint();
            const auto witnessAgent = AI::TestSupport::MakeIdentity<AI::AgentId>(2);
            const auto witnessHandle = capability.ResolveAgent(witnessAgent);
            REQUIRE(witnessHandle.HasValue());
            REQUIRE(fixture.ai.Snapshot().Value().Find(witnessHandle.Value()).Value().owner == *witness);
            poll(1);
            witnessInRange = false;
            REQUIRE(
                fixture.ai
                    .PublishPerceptionAtSafePoint(initialScene, spatial.Current()->Revision() + 1, positions(initialScene).Value(), spatial)
                    .HasValue());

            fixture.Tick(1);
            fixture.AwaitState(fixture.SpawnOperation(), Prefab::PrefabSpawnState::Committed);
            REQUIRE(fixture.Count() == 4);
            poll(2);
            fixture.Tick(2);
            if (fault == Fault::CancelRetirement) {
                fixture.retirementToCancel = fixture.DespawnOperation();
                fixture.AwaitState(fixture.retirementToCancel, Prefab::PrefabSpawnState::Cancelled);
                const auto memory = fixture.ai.ReadPerceptionAtSafePoint(*fixture.scenes.ActiveScene(), *listener);
                REQUIRE(memory.HasValue());
                CHECK(memory.Value().count == 2);
                REQUIRE(capability.ResolveAgent(witnessAgent).Value() == witnessHandle.Value());

                fixture.RestartGameplay();
                fixture.Tick(3);
                fixture.AwaitState(fixture.SpawnOperation(), Prefab::PrefabSpawnState::Committed);
                RequireAllOwners(fixture, 6, 2);
                REQUIRE(capability.ResolveAgent(AI::TestSupport::MakeIdentity<AI::AgentId>(5)).HasValue());
                REQUIRE(capability.ResolveAgent(AI::TestSupport::MakeIdentity<AI::AgentId>(6)).HasValue());
                return;
            }
            fixture.Reach(2);

            // Do not read memory between despawn and the next sense pass. A retained prefab entry would fill the profile
            // and incorrectly evict the older unrelated witness before a read could prune the stale generation.
            addSource(replacementId, 99);
            fixture.Reach(3);
            poll(3);
            const auto current = *fixture.scenes.ActiveScene();
            const auto replacement = current.Find(replacementId);
            REQUIRE(replacement);
            const auto memory = fixture.ai.ReadPerceptionAtSafePoint(current, *listener);
            REQUIRE(memory.HasValue());
            REQUIRE(memory.Value().count == 2);
            CHECK(memory.Value().entries[0].key.source == AI::ProjectPerceptionSource(*witness));
            CHECK(memory.Value().entries[0].lastSensedTick == 1);
            CHECK(memory.Value().entries[1].key.source == AI::ProjectPerceptionSource(*replacement));
            CHECK(memory.Value().entries[1].lastSensedTick == 3);

            const auto replacementAgent = AI::TestSupport::MakeIdentity<AI::AgentId>(99);
            REQUIRE(capability.ResolveAgent(replacementAgent).HasValue());
            Runtime::SceneCommandBuffer retirement;
            retirement.Destroy(*replacement);
            REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(retirement)).HasValue());
            fixture.Reach(2);
            CHECK(capability.ResolveAgent(replacementAgent).HasError());
            CHECK(capability.ResolveAgent(witnessAgent).Value() == witnessHandle.Value());
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
