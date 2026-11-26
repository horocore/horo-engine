#include "../../integration/network/NetworkModeHostTestSupport.h"
#include "../runtime/ai/AITaskSchedulerTestSupport.h"
#include "../runtime/ai/AiSceneTestSupport.h"
#include "../runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "CommittedSceneAssets.h"
#include "DecisionGraphCompilerTestSupport.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Navigation/NavigationDefinition.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "MountedWorldHost.h"
#include "ProjectSceneNavigationComposition.h"
#include "ProjectScenePerception.h"
#include "WorldCookPublicationTestSupport.h"
#include "navigation/NavigationContentPolicyFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace Horo::WorldCook {
    namespace {
        using namespace Application::Internal;
        using namespace PublicationTestSupport;
        using Stage = MountedWorldLiveCellState;

        /** @brief Observes the exact cell publication through the ordinary Scene owner protocol. */
        struct ObservedCellNavigation final : S::SceneGroupPublication {
            struct Candidate final : S::SceneStructuralCandidate {
                Candidate(std::size_t &published, std::size_t desired) : published_(published), desired_(desired) {}

                Result<void> ValidatePublication() const override {
                    return Result<void>::Success();
                }

                void Publish() noexcept override {
                    published_ = desired_;
                }

                Result<void> AfterPublication() override {
                    return Result<void>::Success();
                }

                std::size_t &published_;
                std::size_t desired_;
            };

            ObservedCellNavigation(std::size_t &published, std::size_t desired) : published_(published), desired_(desired) {}

            Result<std::unique_ptr<S::SceneStructuralCandidate>> Prepare(const S::RuntimeSceneView &, std::span<const S::EntityRef>,
                                                                         bool creating) const override {
                CHECK(creating == (desired_ != 0));
                return Result<std::unique_ptr<S::SceneStructuralCandidate>>::Success(std::make_unique<Candidate>(published_, desired_));
            }

            std::size_t &published_;
            std::size_t desired_;
        };

        /** @brief Runs the real mount and Scene cutoff in their ordinary owner order under scheduler-issued evidence. */
        class MountedCellDriver final : public S::RuntimeLifecycleParticipant {
        public:
            MountedCellDriver(MountedWorld &world, S::RuntimeSceneService &scene) : world_(world), scene_(scene) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            Result<void> OnFixedUpdate(const S::FixedStepContext &context) override {
                return scene_.OnFixedUpdate(context);
            }

            Result<void> OnPhase(const S::RuntimePhase phase, const S::FrameContext &context) override {
                if (advanceMount && phase == S::RuntimePhase::CommitDeferredLifecycleChanges) {
                    last = world_.AdvanceLiveCell(context.dispatchEvidence);
                    if (probeDuplicate) {
                        probeDuplicate = false;
                        duplicate = world_.AdvanceLiveCell(context.dispatchEvidence);
                    }
                }
                return scene_.OnPhase(phase, context);
            }

            Result<Stage> last{Result<Stage>::Success(Stage::Idle)};
            std::optional<Result<Stage>> duplicate;
            bool probeDuplicate{};
            bool advanceMount{true};

        private:
            MountedWorld &world_;
            S::RuntimeSceneService &scene_;
        };

        /** @brief Owns the committed population asset and the exact captured controller for the mounted product fixture. */
        struct MountedReducedPopulation final {
            static constexpr S::SceneObjectId Anchor{11}, Agent{51}, DistantAgent{52};
            AI::DecisionGraphTestSupport::Fixture decisions;
            std::array<AI::AiControllerDescriptor, 1> controllers;
            std::unique_ptr<CommittedSceneAssets> assets;
            std::shared_ptr<const HeadlessCookedDecisionInput> input;
            T::TempDir storage;

            MountedReducedPopulation(A::AssetRegistry &registry, JobSystem &jobs) {
                namespace D = AI::DecisionGraphTestSupport;
                auto source = D::TaskTree(10);
                source.executionTask = D::MakeIdentity<AI::TaskId>(9000);
                const auto plan = decisions.Compile(std::span{&source, 1}, 10);
                const auto controller = D::MakeIdentity<AI::ControllerTypeId>(4);
                controllers.front() = {.controller = controller,
                                       .decisionAsset = plan->Root(),
                                       .blackboardSchema = decisions.schema,
                                       .decisionPlan = plan->Graphs().front().bindings,
                                       .cookedPlan = plan};
                const auto manifestId = Asset(90);
                S::SceneDefinitionBuilder builder{SceneCook::SceneDefinitionIdForAsset(manifestId), {1}};
                for (const auto member : {Agent, DistantAgent}) {
                    S::RuntimeEntityDefinition entity{.object = member};
                    entity.localTransform.translation.x = member == Agent ? -2.0F : 20.0F;
                    entity.components.aiAgent = AI::AiAgentComponent{.agent = D::MakeIdentity<AI::AgentId>(member.value)};
                    entity.components.aiController = AI::AiControllerComponent{.controller = controller,
                                                                                 .decisionAsset = plan->Root(),
                                                                                 .blackboardSchema = decisions.schema->Identity(),
                                                                                 .enabled = true};
                    builder.Add(std::move(entity));
                }
                const auto definition = std::move(builder).Build();
                REQUIRE(definition.HasValue());
                const auto payload = SceneCook::EncodeCookedSceneDefinition(definition.Value());
                REQUIRE(payload.HasValue());
                const auto target = T::Target("headless-null");
                const auto type = A::AssetTypeId::Parse("core.scene").Value();
                const auto envelope = A::EncodeCookedArtifact(
                    {.id = manifestId, .type = type, .target = target, .payloadDigest = T::DigestOf(payload.Value()), .payload = payload.Value()});
                REQUIRE(envelope.HasValue());
                const std::array entries{A::AssetCookManifestEntry{manifestId, type, manifestId.ToString() + ".cooked",
                                                                    T::DigestOf(envelope.Value())}};
                const std::array artifacts{envelope.Value()};
                REQUIRE(T::PublishFixture(storage.path, target, entries, artifacts).HasValue());
                REQUIRE(registry.Publish({A::AssetRecord{manifestId, type, ProjectPath::Parse("Assets/population.horo").Value(),
                                                         ProjectPath::Parse("Assets/population.horo.horo").Value()}}).status ==
                        A::AssetRegistryBuildStatus::Complete);
                auto committed = CommittedSceneAssets::Capture(NativeDurableFileSystem{}, {storage.path, target}, registry.Snapshot());
                REQUIRE(committed.HasValue());
                assets = std::move(committed).Value();
                auto population = assets->DecodeScene(manifestId);
                REQUIRE(population.HasValue());
                AI::AiSceneCookedDecisionExecution execution{jobs, {}, {}};
                execution.scheduler.mode = AI::AiSchedulingMode::BestEffort;
                execution.scheduler.simulationProfile = AI::AiDecisionSimulationProfile{200, 400, 600, 2, 1};
                input = std::make_shared<const HeadlessCookedDecisionInput>(HeadlessCookedDecisionInput{
                    .committed = *assets,
                    .environment = decisions.Environment(),
                    .execution = execution,
                    .simulationAnchor = Anchor,
                    .reducedPopulation = HeadlessReducedPopulationSelection{
                        std::make_shared<const S::RuntimeSceneDefinition>(std::move(population).Value()), 1}});
            }
        };

        /** @brief Composes the ordinary standalone product over a durable cooked World and observes its exact Scene facade. */
        class MountedCellProduct final {
        public:
            explicit MountedCellProduct(const bool enableLive, const bool firstDemand, const std::size_t maximumResidents = 1,
                                        const bool withReducedPopulation = false,
                                        const std::optional<GameplayWorldSelection> &gameplay = std::nullopt) {
                namespace N = Network;
                if (withReducedPopulation) {
                    reduced_.emplace(registry_, jobs_);
                    // These two mutually exclusive cells represent the same moving gameplay anchor.
                    fixture_.entities.front().localTransform.translation.x = -2;
                    fixture_.entities.back().localTransform.translation.x = -1;
                    fixture_.entities.back().object = MountedReducedPopulation::Anchor;
                    std::get<SpatialObjectBinding>(fixture_.objects.back()).entity = MountedReducedPopulation::Anchor;
                }
                const auto published = fixture_.Publish();
                REQUIRE(published.HasValue());
                REQUIRE(published.Value().publication);
                const auto release = fixture_.Release();
                identity_ = fixture_.inputs.front().expected;
                demand_ = {identity_.cell, {900}, identity_.scene, identity_.revision};
                siblingIdentity_ = fixture_.inputs.back().expected;
                sibling_ = {siblingIdentity_.cell, {901}, siblingIdentity_.scene, siblingIdentity_.revision};
                CommittedWorldSelection selection{release.targetRoot,
                                                  release.target,
                                                  published.Value().publication->manifestDigest,
                                                  release.indexArtifact,
                                                  release.dependencyArtifact,
                                                  Math::WorldCoordinate64::FromMillimeters(withReducedPopulation ? 1000 : 0, 0, 0)};
                if (enableLive) {
                    const MountedWorldLiveLimits limits{.maximumFrameNanoseconds = 20'000'000,
                                                        .maximumUnitNanoseconds = 1'000'000,
                                                        .maximumCellEntities = 4,
                                                        .maximumResidentCells = maximumResidents};
                    selection.liveCell = CommittedWorldLiveSelection{limits, firstDemand ? std::optional{demand_} : std::nullopt};
                }
                S::SceneDefinitionBuilder root{{99}, {1}};
                S::RuntimeEntityDefinition parent{.object = parentObject_};
                if (gameplay)
                    parent.components.behaviors.push_back(
                        {{1}, Gameplay::BehaviorTypeId::Parse("game.tests.replicated_presentation_motion").Value()});
                root.Add(std::move(parent));
                auto built = std::move(root).Build();
                REQUIRE(built.HasValue());
                HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Null,
                                                   .settings = Physics::Settings(),
                                                   .registry = registry_,
                                                   .loads = loads_,
                                                   .load = load_,
                                                   .aiControllers = reduced_ ? std::span<const AI::AiControllerDescriptor>{reduced_->controllers}
                                                                             : std::span<const AI::AiControllerDescriptor>{},
                                                   .cookedDecisions = reduced_ ? reduced_->input : nullptr,
                                                   .mountedWorld = std::make_shared<const CommittedWorldSelection>(std::move(selection)),
                                                   .worldJobs = &jobs_};
                auto composition =
                    ComposeHeadlessSceneNetworkServices(std::make_shared<const S::RuntimeSceneDefinition>(std::move(built).Value()),
                                                        std::move(input), {}, gameplay);
                REQUIRE(composition.HasValue());
                auto services = std::move(composition).Value();
                auto &scene = services.factories.services[static_cast<std::size_t>(N::NetworkModeServiceKind::Scene)];
                scene = [factory = std::move(scene), this](const N::NetworkModeServiceRequest &request) {
                    auto owned = factory(request);
                    if (owned.HasValue())
                        scene_ = owned.Value().get();
                    return owned;
                };

                auto settings = N::DefaultNetworkProjectSettings(N::Id<N::NetworkProjectSettingsId>(10));
                REQUIRE(settings.HasValue());
                auto project = N::NetworkProjectSettings::Create(settings.Value());
                REQUIRE(project.HasValue());
                constexpr auto role = N::NetworkProjectRole::Standalone;
                const auto facts = N::ProductFacts(project.Value(), role, false);
                const std::array worlds{N::World(N::NetworkModeWorldKind::Standalone, runtime_.value)};
                const auto inputs = NetworkProductHostInputs{.project = project.Value(),
                                                             .product = facts.product,
                                                             .inventory = facts.inventory,
                                                             .hostFacts = facts.host,
                                                             .requirements = facts.requirements,
                                                             .selection = facts.selection,
                                                             .hostGeneration = 5,
                                                             .worlds = worlds,
                                                             .fixedParticipant = std::move(services.fixedParticipant),
                                                             .pollRetirement = std::move(services.pollRetirement)};
                auto created = NetworkProductHost::Create(clock_, inputs, std::move(services.factories));
                REQUIRE(created.HasValue());
                host_ = std::move(created).Value();
                Check(host_->Startup());
                REQUIRE(scene_);
                const auto origin = InspectHeadlessWorldOrigin(*scene_);
                REQUIRE(origin);
                const auto frame = origin->Get();
                REQUIRE(frame.HasValue());
                epoch_ = IdentityFrom<W::PartitionEpoch>(frame.Value().Binding().identity.Value());
            }

            void Request() {
                Check(RequestHeadlessWorldCell(*scene_, demand_));
            }

            void Withdraw() {
                Check(WithdrawHeadlessWorldCell(*scene_));
            }

            void RequestSibling() const {
                Check(RequestHeadlessWorldCell(*scene_, sibling_));
            }

            void WithdrawFirstOccurrence() const {
                Check(WithdrawHeadlessWorldCell(*scene_, demand_.occurrence));
            }

            void ForceEvictFirstOccurrence() const {
                Check(ForceEvictHeadlessWorldCell(*scene_, demand_.occurrence));
            }

            Result<void> ProbeRequest() const {
                return RequestHeadlessWorldCell(*scene_, demand_);
            }

            Result<MountedWorldCellReadiness> FirstReadiness() const {
                return InspectHeadlessWorldCell(*scene_, demand_.occurrence);
            }

            std::optional<S::EntityRef> Find(const S::SceneObjectId object) const {
                const auto scene = InspectHeadlessScene(*scene_);
                REQUIRE(scene);
                return scene->Find(object);
            }

            Result<Math::WorldCoordinate64> WorldPosition(const S::EntityRef entity) const {
                const auto scene = InspectHeadlessScene(*scene_);
                const auto lease = InspectHeadlessWorldOrigin(*scene_);
                REQUIRE(scene);
                REQUIRE(lease);
                const auto origin = lease->Get();
                REQUIRE(origin.HasValue());
                return CaptureProjectSceneWorldPosition(*scene, entity, origin.Value().Origin());
            }

            /** @brief Reads the vertical position driven by the native fixture's ordinary gameplay callback. */
            std::int64_t ParentHeight() const {
                const auto parent = Find(parentObject_);
                REQUIRE(parent);
                const auto position = WorldPosition(*parent);
                REQUIRE(position.HasValue());
                return position.Value().Millimeters()[1];
            }

            const AI::DecisionGraphTestSupport::Probe &ReducedDecisions() const {
                REQUIRE(reduced_);
                return reduced_->decisions.probe;
            }

            void CheckTravelRefusal() {
                const auto *original = scene_;
                const std::array replacements{Network::World(Network::NetworkModeWorldKind::Standalone, runtime_.value + 1)};
                Check(host_->RequestTravel(replacements));
                static_cast<void>(RunFrame());
                const auto rejected = host_->TakeTravelFailure();
                REQUIRE(rejected);
                INFO(rejected->message);
                CHECK(rejected->code.Value() == W::WorldArtifactErrors::Unsupported.code.Value());
                REQUIRE(scene_ == original);
                REQUIRE(host_->Role(Network::NetworkModeWorldKind::Standalone, runtime_).HasValue());
                REQUIRE(host_->Role(Network::NetworkModeWorldKind::Standalone, replacements.front().scene).HasError());
            }

            bool Attached(const bool sibling = false) const {
                const auto scene = InspectHeadlessScene(*scene_);
                REQUIRE(scene);
                REQUIRE(scene->RuntimeId() == runtime_);
                REQUIRE(scene->Find(parentObject_));
                const auto &selected = sibling ? sibling_ : demand_;
                return S::FindStreamedSceneInstance(*scene,
                                                    {runtime_, selected.occurrence, selected.definition, S::SceneInstanceMode::Streamed},
                                                    sibling ? siblingIdentity_ : identity_, epoch_)
                    .has_value();
            }

            std::int64_t RunFrame() {
                clock_.Advance(Duration::FromMilliseconds(17));
                const auto start = std::chrono::steady_clock::now();
                const auto advanced = host_->RunFrame();
                const auto elapsed = std::chrono::steady_clock::now() - start;
                Check(advanced);
                return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
            }

            std::int64_t UntilAttached(const bool expected, const bool sibling = false) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    const auto sample = RunFrame();
                    if (Attached(sibling) == expected)
                        return sample;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (std::chrono::steady_clock::now() < deadline);
                FAIL("Standalone mounted frame did not publish the expected residency before the fixture deadline");
                return 0;
            }

        private:
            static void Check(const Result<void> &result) {
                if (result.HasError())
                    UNSCOPED_INFO(result.ErrorValue().message);
                REQUIRE(result.HasValue());
            }

            static constexpr S::SceneRuntimeId runtime_{71};
            static constexpr S::SceneObjectId parentObject_{42};
            Fixture fixture_{false, false};
            S::SceneCellPayloadIdentity identity_;
            S::SceneCellPayloadIdentity siblingIdentity_;
            W::PartitionEpoch epoch_;
            CommittedWorldInitialCell demand_;
            CommittedWorldInitialCell sibling_;
            DeterministicClock clock_;
            JobSystem jobs_{{1, 4}};
            A::MemoryAssetProvider provider_;
            A::AssetRegistry registry_;
            A::AssetLoadService loads_{jobs_, provider_};
            HeadlessSceneLoadBoundary load_{{}, std::chrono::steady_clock::now() + std::chrono::seconds(30)};
            std::optional<MountedReducedPopulation> reduced_;
            const Network::INetworkModeService *scene_{};
            std::unique_ptr<NetworkProductHost> host_;
        };

        /** @brief Prints observed frame quantiles without filtering out outliers or imposing machine-specific thresholds. */
        void PrintFrameSamples(const std::string_view stage, std::span<std::int64_t> samples) {
            std::ranges::sort(samples);
            const auto percentile = [&](const std::size_t percent) {
                return samples[(percent * samples.size() + 99) / 100 - 1];
            };
            std::cout << stage << ",samples=" << samples.size() << ",p50_ns=" << percentile(50) << ",p95_ns=" << percentile(95)
                      << ",p99_ns=" << percentile(99) << ",max_ns=" << samples.back() << '\n';
        }

        /** @brief Gives the two cells independent native colliders backed by the same verified material envelope. */
        void PopulateCellMaterial(Fixture &fixture, const A::AssetDependency &material) {
            for (std::size_t index = 0; index < fixture.entities.size(); ++index) {
                auto &entity = fixture.entities[index];
                const auto body = 100 + index;
                entity.components.rigidBody = S::RigidBodyComponent{.id = {10 + index}, .body = {body}};
                entity.components.colliders = {
                    Physics::Collider(20 + index, 200 + index, entity.object.value, body, material.id,
                                      S::PhysicsColliderSource{S::PhysicsAnalyticCollider{S::PhysicsSphereCollider{0.5F}}})};
                fixture.inputs[index].source.dependencies = {&material, 1};
            }
            const auto descriptor = Physics::PhysicsMaterialAsset::Create(
                {.id = Physics::PhysicsMaterialAssetId::FromAssetId(material.id), .sourceRevision = 1});
            REQUIRE(descriptor.HasValue());
            const auto encoded = Physics::EncodePhysicsMaterialAsset(descriptor.Value());
            REQUIRE(encoded.HasValue());
            const auto digest = ComputeSha256(encoded.Value());
            const auto bytes = A::EncodeCookedArtifact(
                {material.id, material.expectedType, fixture.Release().target, digest, digest, digest,
                 std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(encoded.Value().data()),
                                           reinterpret_cast<const std::uint8_t *>(encoded.Value().data() + encoded.Value().size()))});
            REQUIRE(bytes.HasValue());
            fixture.baseArtifacts.push_back(bytes.Value());
            fixture.dependencyArtifacts.push_back({.id = material.id, .content = T::DigestOf(bytes.Value()), .revision = 1});
            const std::array chunks{A::AssetChunkDefinition{A::AssetChunkId::Parse("base").Value(),
                                                            A::AssetChunkKind::Base,
                                                            {Asset(4), Asset(5), Asset(6), Asset(7), material.id}}};
            auto packages = A::AssetChunkPlan::Create(chunks);
            REQUIRE(packages.HasValue());
            fixture.packages = std::move(packages).Value();
        }
    }  // namespace

    TEST_CASE("Mounted World streams a verified cell through the real structural cutoff and detaches only its occurrence",
              "[world_cook][native][mounted_world][live_cell]") {
        const auto parentPoint = Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0);
        const W::WorldSpatialAssignmentCandidate parent{Endpoint(3).address,
                                                        Endpoint(3).revision,
                                                        {parentPoint, parentPoint},
                                                        W::TestSupport::Layer(),
                                                        0};
        Fixture fixture{false, false, parent};
        const auto controller = AI::TestSupport::MakeFixture(813, 812, 101, 0);
        auto agentEntity = fixture.entities.front();
        agentEntity.components.aiAgent = controller.agent.agent;
        agentEntity.components.aiController = controller.agent.controller;
        agentEntity.parent = S::SceneObjectId{10};
        agentEntity.localTransform.translation.x = 0.25F;
        const std::array cellEntities{S::RuntimeEntityDefinition{.object = {10}, .localTransform = {.translation = {-1.0F, 0, 0}}},
                                      agentEntity};
        fixture.inputs.front().source.entities = cellEntities;
        fixture.objects.emplace_back(SpatialObjectBinding{Endpoint(3), fixture.inputs.front().expected.cell, {10}});
        const auto published = fixture.Publish();
        REQUIRE(published.HasValue());
        for (const auto &finding : published.Value().validation->findings)
            UNSCOPED_INFO(finding.cause.message);
        REQUIRE(published.Value().publication);
        const auto release = fixture.Release();
        const auto identity = fixture.inputs.front().expected;
        const CommittedWorldInitialCell request{identity.cell, {900}, identity.scene, identity.revision};
        const auto siblingIdentity = fixture.inputs.back().expected;
        const CommittedWorldInitialCell sibling{siblingIdentity.cell, {901}, siblingIdentity.scene, siblingIdentity.revision};
        const CommittedWorldSelection selection{release.targetRoot,
                                                release.target,
                                                published.Value().publication->manifestDigest,
                                                release.indexArtifact,
                                                release.dependencyArtifact,
                                                Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};

        JobSystem jobs{JobSystemConfig{1, 4}};
        A::MemoryAssetProvider provider;
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        using AI::TestSupport::MakeIdentity;
        const AI::PerceptionDescriptorOrigin origin{AI::PerceptionDescriptorSourceKind::Native, MakeIdentity<AI::PerceptionProviderId>(91),
                                                    1};
        const auto sense = AI::SenseTypeIds::Sight;
        const auto stimulus = MakeIdentity<AI::StimulusTypeId>(92);
        const auto listener = MakeIdentity<AI::PerceptionListenerTypeId>(93);
        const std::array senses{AI::SenseTypeDescriptor{sense, origin, "Sight"}};
        const std::array stimuli{AI::StimulusTypeDescriptor{stimulus, origin, "Observation"}};
        const std::array listeners{AI::PerceptionListenerDescriptor{.identity = listener,
                                                                    .origin = origin,
                                                                    .displayName = "Sight",
                                                                    .sense = sense,
                                                                    .stimuli = {{stimulus}},
                                                                    .stimulusCount = 1}};
        auto descriptors = AI::PerceptionDescriptorRegistry::Capture({senses, stimuli, listeners}, {});
        REQUIRE(descriptors.HasValue());
        const auto perception = std::make_shared<const ProjectScenePerceptionPolicy>(
            ProjectScenePerceptionPolicy{.definition = {99},
                                         .revision = {1},
                                         .controllers = {{.controller = controller.descriptor.controller,
                                                          .listeners = {listener},
                                                          .sources = {sense}}}});
        auto catalog =
            BindProjectPerceptionEndpoints(*perception,
                                           std::make_shared<const AI::PerceptionDescriptorRegistry>(std::move(descriptors).Value()),
                                           std::array{controller.descriptor});
        REQUIRE(catalog.HasValue());
        const auto positions = MakeProjectPerceptionPositionSources(perception);
        AI::PerceptionSpatialBroadphase spatial;
        auto createdAi = AI::AiSceneRuntime::Create();
        REQUIRE(createdAi.HasValue());
        auto ai = std::move(createdAi).Value();
        S::RuntimeSceneService scene{registry, loads, S::RuntimeSceneAssetLimits{}, S::SceneRuntimeId{101}};
        REQUIRE(
            scene
                .AddActivationParticipant(
                    std::make_unique<AI::AiSceneActivationParticipant>(ai, catalog.Value(),
                                                                       AI::AiScenePerceptionActivationInput{spatial, {}, positions.scene}))
                .HasValue());
        REQUIRE(scene
                    .AddStructuralParticipant(
                        ai.MakeStructuralParticipant(catalog.Value(),
                                                     AI::AiScenePerceptionGroupInput{.spatial = spatial,
                                                                                     .positionsWithHierarchy = positions.group}))
                    .HasValue());
        S::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add({.object = {42}});
        auto definition = std::move(startup).Build();
        REQUIRE(definition.HasValue());
        const HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(scene, std::move(definition).Value(), boundary).HasValue());
        REQUIRE(scene.ActiveScene());
        const auto root = scene.ActiveScene()->RuntimeId();
        const S::SceneInstanceAddress address{root, request.occurrence, request.definition, S::SceneInstanceMode::Streamed};
        const auto epoch = IdentityFrom<W::PartitionEpoch>(5);
        const auto attached = [&] {
            REQUIRE(scene.ActiveScene());
            REQUIRE(scene.ActiveScene()->RuntimeId() == root);
            REQUIRE(scene.ActiveScene()->Find({42}));
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), address, identity, epoch).has_value();
        };

        auto prepared = MountedWorld::Prepare(NativeDurableFileSystem{}, selection, 5, scene);
        REQUIRE(prepared.HasValue());
        auto mount = std::move(prepared).Value();
        DeterministicClock clock;
        auto created = S::RuntimeHost::Create(clock);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto driver = std::make_unique<MountedCellDriver>(*mount, scene);
        auto &progress = *driver;
        REQUIRE(host->AddParticipant(std::move(driver)).HasValue());
        REQUIRE(mount
                    ->BindLiveCell(host->DispatchSource(), jobs,
                                   {.maximumFrameNanoseconds = 20'000'000,
                                    .maximumUnitNanoseconds = 1'000'000,
                                    .maximumCellEntities = 4,
                                    .maximumResidentCells = 2})
                    .HasValue());
        REQUIRE(host->Startup().HasValue());
        W::TestSupport::RequireError(mount->AdvanceLiveCell({}), W::WorldArtifactErrors::Stale);
        const auto frame = [&] {
            clock.Advance(Duration::FromMilliseconds(17));
            REQUIRE(host->RunFrame().HasValue());
        };
        const auto until = [&](auto done) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do {
                frame();
                if (done(progress.last))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Mounted cell failed to settle before the fixture deadline");
        };
        const auto stage = [&](const Stage expected) {
            REQUIRE(progress.last.HasValue());
            REQUIRE(progress.last.Value() == expected);
        };

        REQUIRE_FALSE(attached());
        REQUIRE(mount->RequestLiveCell(request).HasValue());
        progress.probeDuplicate = true;
        frame();
        stage(Stage::Loading);
        REQUIRE(progress.duplicate);
        W::TestSupport::RequireError(*progress.duplicate, W::WorldArtifactErrors::Stale);
        REQUIRE_FALSE(attached());

        SECTION("Cold teardown drains an in-flight preparation without shutting down the unrelated parent") {
            host->Shutdown();
            mount.reset();
            REQUIRE_FALSE(attached());
            return;
        }

        SECTION("Cancelling submitted preparation cannot attach a Scene or block fresh demand") {
            REQUIRE(mount->WithdrawLiveCell().HasValue());
            until([&](const Result<Stage> &last) {
                return last.HasError();
            });
            W::TestSupport::RequireError(progress.last, W::WorldArtifactErrors::Cancelled);
            REQUIRE_FALSE(attached());
            REQUIRE(mount->RequestLiveCell(request).HasValue());
            frame();
            stage(Stage::Loading);
        }

        until([](const Result<Stage> &last) {
            return last.HasError() || last.Value() != Stage::Loading;
        });
        stage(Stage::Prepared);
        REQUIRE_FALSE(attached());
        frame();
        stage(Stage::Attaching);
        REQUIRE(attached());
        frame();
        stage(Stage::Active);
        const auto capability = ai.CapabilityAtSafePoint();
        const auto authoredAgent = controller.agent.agent.agent;
        const auto original = capability.ResolveAgent(authoredAgent);
        REQUIRE(original.HasValue());
        const auto originalOwner = capability.ResolveAgentOwner(original.Value());
        REQUIRE(originalOwner.HasValue());
        const auto originalSpatial = spatial.Current();
        REQUIRE(originalSpatial);
        const auto originalPosition = originalSpatial->ListenerPosition(originalOwner.Value());
        REQUIRE(originalPosition.HasValue());
        CHECK(originalPosition.Value() == Math::WorldCoordinate64::FromMillimeters(-750, 0, 0));
        const auto cellReadiness = mount->InspectLiveCell(request.occurrence);
        REQUIRE(cellReadiness.HasValue());
        CHECK(cellReadiness.Value().geometryVisible);
        CHECK(cellReadiness.Value().navigation == MountedWorldCellNavigationReadiness::NotDeclared);
        const auto originalScene = scene.ActiveScene();
        REQUIRE(originalScene);
        const auto originalFence = cellReadiness.Value().fence;
        CHECK(mount->IsLiveRegionPublished(*originalScene, request.occurrence, originalFence));
        CHECK_FALSE(mount->IsLiveRegionPublished(*originalScene, sibling.occurrence, originalFence));
        const auto originalPolicy = originalSpatial->ReadPolicyAuthorization();
        REQUIRE(originalPolicy->IsCurrent());
        const auto originalLifetime = capability.CaptureAgentLifetime(original.Value());
        REQUIRE(originalLifetime.HasValue());
        const auto taskId = AI::TestSupport::MakeIdentity<AI::TaskId>(814);
        const auto originalTask = ai.StartTaskAtSafePoint(original.Value(), taskId);
        REQUIRE(originalTask.HasValue());
        REQUIRE(ai.Find(original.Value()).Value().hasRunningTask);
        CHECK_FALSE(originalLifetime.Value().IsCancellationRequested());

        const S::SceneInstanceAddress siblingAddress{root, sibling.occurrence, sibling.definition, S::SceneInstanceMode::Streamed};
        const auto siblingAttached = [&] {
            REQUIRE(scene.ActiveScene());
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), siblingAddress, siblingIdentity, epoch).has_value();
        };
        REQUIRE(mount->RequestLiveCell(request).HasError());
        REQUIRE(mount->RequestLiveCell(sibling).HasValue());
        REQUIRE(mount->WithdrawLiveCell().HasError());
        until([](const Result<Stage> &last) {
            return last.HasError() || last.Value() == Stage::Active;
        });
        stage(Stage::Active);
        REQUIRE(attached());
        REQUIRE(siblingAttached());
        REQUIRE(ai.ConfigureTaskSchedulerAtSafePoint(jobs).HasValue());
        const auto scheduler = ai.TaskSchedulerAtSafePoint();
        REQUIRE(scheduler.HasValue());
        const auto image = std::make_shared<int>(1);
        const auto oldDecision = std::make_shared<AI::SchedulerTestSupport::Probe>();
        REQUIRE(ai.RegisterDecisionAtSafePoint(original.Value(), {}, image, oldDecision).HasValue());
        REQUIRE(scheduler.Value()->EvaluateAtDecision(1).HasValue());
        REQUIRE(oldDecision->intents == 1);
        REQUIRE(mount->WithdrawLiveCell(request.occurrence).HasValue());
        frame();
        stage(Stage::Retiring);
        REQUIRE_FALSE(attached());
        REQUIRE(siblingAttached());
        REQUIRE(scheduler.Value()->CommitAtIntentDispatch(1).HasValue());
        CHECK(oldDecision->cancelled == 1);
        CHECK(oldDecision->committed == 0);
        CHECK(originalLifetime.Value().IsCancellationRequested());
        CHECK_FALSE(originalPolicy->IsCurrent());
        REQUIRE(spatial.Current());
        CHECK(spatial.Current()->ListenerPosition(originalOwner.Value()).HasError());
        CHECK(capability.ResolveAgent(authoredAgent).HasError());
        CHECK(capability.CompleteTask(original.Value(), originalTask.Value()).HasError());
        REQUIRE(scene.ActiveScene());
        CHECK(scene.ActiveScene()->Get(originalOwner.Value()).HasError());
        CHECK_FALSE(mount->IsLiveRegionPublished(*scene.ActiveScene(), request.occurrence, originalFence));
        frame();
        stage(Stage::Active);
        REQUIRE(mount->WithdrawLiveCell(request.occurrence).HasError());
        REQUIRE(mount->WithdrawLiveCell().HasValue());
        frame();
        stage(Stage::Retiring);
        REQUIRE_FALSE(siblingAttached());
        frame();
        stage(Stage::Idle);
        REQUIRE_FALSE(attached());
        REQUIRE_FALSE(siblingAttached());

        REQUIRE(mount->RequestLiveCell(request).HasValue());
        until([](const Result<Stage> &last) {
            return last.HasError() || last.Value() == Stage::Active;
        });
        stage(Stage::Active);
        REQUIRE(attached());
        const auto freshRegion = mount->InspectLiveCell(request.occurrence);
        REQUIRE(freshRegion.HasValue());
        REQUIRE(scene.ActiveScene());
        CHECK(mount->IsLiveRegionPublished(*scene.ActiveScene(), request.occurrence, freshRegion.Value().fence));
        CHECK_FALSE(mount->IsLiveRegionPublished(*scene.ActiveScene(), request.occurrence, originalFence));
        CHECK_FALSE(mount->IsLiveRegionPublished(*originalScene, request.occurrence, freshRegion.Value().fence));
        const auto replacement = capability.ResolveAgent(authoredAgent);
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() != original.Value());
        const auto replacementOwner = capability.ResolveAgentOwner(replacement.Value());
        REQUIRE(replacementOwner.HasValue());
        CHECK(replacementOwner.Value() != originalOwner.Value());
        REQUIRE(spatial.Current());
        CHECK(spatial.Current()->ListenerPosition(originalOwner.Value()).HasError());
        const auto replacementPosition = spatial.Current()->ListenerPosition(replacementOwner.Value());
        REQUIRE(replacementPosition.HasValue());
        CHECK(replacementPosition.Value() == originalPosition.Value());
        CHECK_FALSE(ai.Find(replacement.Value()).Value().hasRunningTask);
        CHECK(capability.CompleteTask(replacement.Value(), originalTask.Value()).HasError());
        CHECK(capability.CompleteTask(original.Value(), originalTask.Value()).HasError());
        CHECK(originalLifetime.Value().IsCancellationRequested());
        const auto newDecision = std::make_shared<AI::SchedulerTestSupport::Probe>();
        REQUIRE(ai.RegisterDecisionAtSafePoint(replacement.Value(), {}, image, newDecision).HasValue());
        REQUIRE(scheduler.Value()->EvaluateAtDecision(2).HasValue());
        REQUIRE(scheduler.Value()->CommitAtIntentDispatch(2).HasValue());
        CHECK(newDecision->committed == 1);
        CHECK(oldDecision->cancelled == 1);
    }

    TEST_CASE("Mounted headless World transfers reduced agents as the streamed anchor moves",
              "[world_cook][native][mounted_world][live_cell][product][reduced-population]") {
        using Population = MountedReducedPopulation;
        std::optional<GameplayWorldSelection> gameplay;
        SECTION("with only Scene and AI owners") {}
        SECTION("with a selected native Gameplay owner") {
            gameplay.emplace();
            gameplay->moduleId = "game.tests";
            gameplay->nativeArtifact = HORO_TEST_GAME_MODULE_PATH;
            gameplay->moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>();
            std::ifstream revision{HORO_TEST_GAME_MODULE_REVISION_PATH};
            REQUIRE((revision >> gameplay->descriptorRevision));
        }
        MountedCellProduct product{true, false, 1, true, gameplay};
        const auto initialHeight = product.ParentHeight();
        REQUIRE_FALSE(product.Find(Population::Anchor));
        REQUIRE_FALSE(product.Find(Population::Agent));
        product.Request();
        static_cast<void>(product.UntilAttached(true));
        for (int frame = 0; frame < 3; ++frame)
            static_cast<void>(product.RunFrame());
        const auto leftAnchor = product.Find(Population::Anchor);
        REQUIRE(leftAnchor);
        const auto leftPosition = product.WorldPosition(*leftAnchor);
        REQUIRE(leftPosition.HasValue());
        CHECK(leftPosition.Value() == Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0));
        const auto first = product.Find(Population::Agent);
        REQUIRE(first);
        REQUIRE_FALSE(product.Find(Population::DistantAgent));
        REQUIRE(product.ReducedDecisions().starts == 1);
        CHECK(product.WorldPosition(*first).Value() == Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0));
        if (gameplay)
            CHECK(product.ParentHeight() < initialHeight);

        product.Withdraw();
        static_cast<void>(product.UntilAttached(false));
        static_cast<void>(product.RunFrame());
        CHECK_FALSE(product.Find(Population::Anchor));
        CHECK(product.Find(Population::Agent) == first);
        product.RequestSibling();
        static_cast<void>(product.UntilAttached(true, true));
        for (int frame = 0; frame < 3; ++frame)
            static_cast<void>(product.RunFrame());
        REQUIRE(product.Find(Population::Anchor));
        CHECK_FALSE(product.Find(Population::Agent));

        product.Withdraw();
        static_cast<void>(product.UntilAttached(false, true));
        static_cast<void>(product.RunFrame());
        product.Request();
        static_cast<void>(product.UntilAttached(true));
        for (int frame = 0; frame < 3; ++frame)
            static_cast<void>(product.RunFrame());
        const auto replacement = product.Find(Population::Agent);
        REQUIRE(replacement);
        CHECK(replacement != first);
        CHECK(product.WorldPosition(*replacement).Value() == Math::WorldCoordinate64::FromMillimeters(-1000, 0, 0));
        CHECK(product.ReducedDecisions().starts == 2);
        CHECK_FALSE(product.Find(Population::DistantAgent));
    }

    TEST_CASE("Standalone mounted CoreEcs demand starts in the product frame and withdraws through the same Scene owner",
              "[world_cook][native][mounted_world][live_cell][product]") {
        SECTION("Legacy mounted worlds stay idle and refuse live demand") {
            MountedCellProduct product{false, false};
            W::TestSupport::RequireError(product.ProbeRequest(), W::WorldArtifactErrors::Stale);
            static_cast<void>(product.RunFrame());
            REQUIRE_FALSE(product.Attached());
        }
        SECTION("An explicitly selected first demand is deferred until real frames, with retirement and fresh demand") {
            MountedCellProduct product{true, true};
            REQUIRE_FALSE(product.Attached());
            static_cast<void>(product.UntilAttached(true));
            static_cast<void>(product.RunFrame());  // Reconcile the exact positive Scene receipt.
            const auto before = product.FirstReadiness();
            REQUIRE(before.HasValue());
            CHECK(before.Value().geometryVisible);
            CHECK(before.Value().navigation == MountedWorldCellNavigationReadiness::NotDeclared);
            product.Withdraw();
            static_cast<void>(product.UntilAttached(false));
            static_cast<void>(product.RunFrame());  // Acknowledge retirement before issuing a fresh generation.
            W::TestSupport::RequireError(product.FirstReadiness(), W::WorldArtifactErrors::Stale);
            product.Request();
            static_cast<void>(product.UntilAttached(true));
            const auto after = product.FirstReadiness();
            REQUIRE(after.HasValue());
            CHECK(after.Value().geometryVisible);
            CHECK(after.Value().fence.generation > before.Value().fence.generation);
        }
        SECTION("An in-frame replacement cannot drain a live mounted worker or revoke its parent") {
            MountedCellProduct product{true, true};
            REQUIRE_FALSE(product.Attached());
            product.CheckTravelRefusal();
            static_cast<void>(product.UntilAttached(true));
        }
        SECTION("Configured standalone hosts can withdraw an exact occurrence while its sibling stays published") {
            MountedCellProduct product{true, true, 2};
            static_cast<void>(product.UntilAttached(true));
            static_cast<void>(product.RunFrame());  // Reconcile the first positive Scene receipt.
            product.RequestSibling();
            static_cast<void>(product.UntilAttached(true, true));
            static_cast<void>(product.RunFrame());
            product.WithdrawFirstOccurrence();
            static_cast<void>(product.UntilAttached(false));
            REQUIRE(product.Attached(true));
        }
        SECTION("The authoritative standalone facade can force retirement of an exact resident occurrence") {
            MountedCellProduct product{true, true};
            static_cast<void>(product.UntilAttached(true));
            static_cast<void>(product.RunFrame());
            product.ForceEvictFirstOccurrence();
            static_cast<void>(product.UntilAttached(false));
        }
    }

    TEST_CASE("Mounted live cells charge one committed allocation and retire it after the last native detach",
              "[world_cook][native][mounted_world][live_cell]") {
        auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical);
        if (runtime.HasError() && runtime.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value())
            SKIP("The native Physics provider is omitted from this test build.");
        REQUIRE(runtime.HasValue());
        const A::AssetDependency material{Asset(9), Physics::AssetType()};
        Fixture fixture{false, false};
        PopulateCellMaterial(fixture, material);
        const auto published = fixture.Publish();
        if (published.HasError())
            UNSCOPED_INFO(published.ErrorValue().code.Value() << ": " << published.ErrorValue().message);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const auto release = fixture.Release();
        const auto identity = fixture.inputs.front().expected;
        const CommittedWorldInitialCell request{identity.cell, {900}, identity.scene, identity.revision};
        const auto siblingIdentity = fixture.inputs.back().expected;
        const CommittedWorldInitialCell sibling{siblingIdentity.cell, {901}, siblingIdentity.scene, siblingIdentity.revision};
        const auto bytes = fixture.baseArtifacts.front().size();
        MountedWorldLiveLimits limits{20'000'000, 1'000'000, 4, 1, bytes, 2};
        bool expectCapacity{};
        bool retainReader{};
        SECTION("One byte less than the exact envelope never publishes resources or native bodies") {
            --limits.maximumCellAssetBytes;
            expectCapacity = true;
        }
        SECTION("Both live cells share the complete envelope through exact native detach receipts") {
            // The configured budget and reader lifetime are used as-is for the normal detach sequence.
        }
        SECTION("A remaining payload pin delays budget credit after the last native detach") {
            retainReader = true;
        }
        const CommittedWorldSelection selection{release.targetRoot,
                                                release.target,
                                                published.Value().publication->manifestDigest,
                                                release.indexArtifact,
                                                release.dependencyArtifact,
                                                Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                                {},
                                                CommittedWorldLiveSelection{limits, std::nullopt}};
        JobSystem jobs{JobSystemConfig{1, 4}};
        A::MemoryAssetProvider provider;
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        REQUIRE(registry.Publish({Physics::Record(material.id, material.expectedType)}).status == A::AssetRegistryBuildStatus::Complete);
        Physics::PhysicsSceneActivationAuthority authority;
        ScenePhysicsHost native;
        S::RuntimeSceneService scene{registry, loads, S::RuntimeSceneAssetLimits{}, S::SceneRuntimeId{101}};
        auto activation = native.MakeParticipant(*runtime.Value(), authority, Physics::Settings());
        REQUIRE(activation.HasValue());
        REQUIRE(scene.AddActivationParticipant(std::move(activation).Value()).HasValue());
        auto structural = native.MakeStructuralParticipant();
        REQUIRE(structural.HasValue());
        REQUIRE(scene.AddStructuralParticipant(std::move(structural).Value()).HasValue());
        S::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add({.object = {42}});
        auto definition = std::move(startup).Build();
        REQUIRE(definition.HasValue());
        const HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(scene, std::move(definition).Value(), boundary).HasValue());
        REQUIRE(scene.ActiveScene());
        const auto root = scene.ActiveScene()->RuntimeId();
        const S::SceneInstanceAddress address{root, request.occurrence, request.definition, S::SceneInstanceMode::Streamed};
        const S::SceneInstanceAddress siblingAddress{root, sibling.occurrence, sibling.definition, S::SceneInstanceMode::Streamed};
        const auto epoch = IdentityFrom<W::PartitionEpoch>(5);
        const auto attached = [&] {
            REQUIRE(scene.ActiveScene());
            REQUIRE(scene.ActiveScene()->Find({42}));
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), address, identity, epoch);
        };
        const auto bodyCount = [&] {
            const auto scope = native.Borrow();
            REQUIRE(scope.HasValue());
            const auto observed = scope.Value().physics.ReadSceneActivation();
            REQUIRE(observed.HasValue());
            return observed.Value().staticBodies;
        };
        const auto siblingAttached = [&] {
            REQUIRE(scene.ActiveScene());
            return S::FindStreamedSceneInstance(*scene.ActiveScene(), siblingAddress, siblingIdentity, epoch);
        };
        auto prepared = MountedWorld::Prepare(NativeDurableFileSystem{}, selection, 5, scene);
        REQUIRE(prepared.HasValue());
        auto mount = std::move(prepared).Value();
        DeterministicClock clock;
        auto created = S::RuntimeHost::Create(clock);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto driver = std::make_unique<MountedCellDriver>(*mount, scene);
        const auto &progress = *driver;
        REQUIRE(host->AddParticipant(std::move(driver)).HasValue());
        REQUIRE(mount->BindLiveCell(host->DispatchSource(), jobs, limits, registry.Snapshot()).HasValue());
        REQUIRE(host->Startup().HasValue());
        REQUIRE(bodyCount() == 0);
        REQUIRE(mount->RequestLiveCell(request).HasValue());
        const auto frame = [&] {
            clock.Advance(Duration::FromMilliseconds(17));
            REQUIRE(host->RunFrame().HasValue());
        };
        const auto load = [&] {
            frame();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (progress.last.HasValue() && progress.last.Value() == Stage::Loading && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                frame();
            }
        };
        const auto chargedBytes = [&] {
            const auto charged = mount->InspectLiveAssetBytes();
            REQUIRE(charged.HasValue());
            return charged.Value();
        };
        load();
        if (expectCapacity) {
            W::TestSupport::RequireError(progress.last, S::SceneCellPayloadErrors::CapacityExceeded);
            CHECK_FALSE(attached());
            CHECK(bodyCount() == 0);
            CHECK(chargedBytes() == 0);
            return;
        }
        REQUIRE(progress.last.HasValue());
        REQUIRE(progress.last.Value() == Stage::Prepared);
        CHECK(bodyCount() == 0);
        CHECK(chargedBytes() == bytes);
        frame();
        const auto baseline = attached();
        REQUIRE(baseline);
        REQUIRE(baseline->resources.size() == 1);
        CHECK(baseline->resources.front().metadata == material);
        CHECK(baseline->resources.front().artifact.Digest() == T::DigestOf(fixture.baseArtifacts.front()));
        CHECK(bodyCount() == 1);
        frame();
        REQUIRE(progress.last.HasValue());
        REQUIRE(progress.last.Value() == Stage::Active);
        REQUIRE(mount->RequestLiveCell(sibling).HasValue());
        load();
        REQUIRE(progress.last.HasValue());
        REQUIRE(progress.last.Value() == Stage::Prepared);
        CHECK(chargedBytes() == bytes);
        frame();
        A::AssetPayloadLease reader;
        {
            const auto first = attached();
            const auto second = siblingAttached();
            REQUIRE(first);
            REQUIRE(second);
            REQUIRE(second->resources.size() == 1);
            CHECK(first->resources.front().artifact.SharesAllocationWith(second->resources.front().artifact));
            CHECK(bodyCount() == 2);
            if (retainReader)
                reader = second->resources.front().artifact;
        }
        frame();
        REQUIRE(mount->WithdrawLiveCell(request.occurrence).HasValue());
        frame();
        CHECK_FALSE(attached());
        REQUIRE(siblingAttached());
        CHECK(bodyCount() == 1);
        frame();
        REQUIRE(progress.last.HasValue());
        CHECK(progress.last.Value() == Stage::Active);
        CHECK(chargedBytes() == bytes);
        REQUIRE(mount->WithdrawLiveCell(sibling.occurrence).HasValue());
        frame();
        CHECK_FALSE(siblingAttached());
        CHECK(bodyCount() == 0);
        CHECK(chargedBytes() == bytes);
        frame();
        REQUIRE(progress.last.HasValue());
        CHECK(progress.last.Value() == Stage::Idle);
        if (retainReader) {
            CHECK(chargedBytes() == bytes);
            reader = {};
            CHECK(chargedBytes() == bytes);
            frame();
        }
        CHECK(chargedBytes() == 0);
    }

    TEST_CASE("Measure mounted CoreEcs frame impact through the standalone product", "[.mounted_world_benchmark]") {
        constexpr std::size_t warmup = 16;
        constexpr std::size_t sampleCount = 128;
        MountedCellProduct product{true, false};
        std::array<std::int64_t, sampleCount> idle{};
        std::array<std::int64_t, sampleCount> attach{};
        std::array<std::int64_t, sampleCount> detach{};
        for (std::size_t index = 0; index < warmup + sampleCount; ++index) {
            const auto idleFrame = product.RunFrame();
            product.Request();
            const auto attachFrame = product.UntilAttached(true);
            static_cast<void>(product.RunFrame());
            product.Withdraw();
            const auto detachFrame = product.UntilAttached(false);
            static_cast<void>(product.RunFrame());
            if (index >= warmup) {
                idle[index - warmup] = idleFrame;
                attach[index - warmup] = attachFrame;
                detach[index - warmup] = detachFrame;
            }
        }
        std::cout << "mounted_coreecs,entities_per_cell=1,worker_threads=1,physics=null,provider=memory,clock=steady_clock\n";
        PrintFrameSamples("idle_frame", idle);
        PrintFrameSamples("attach_frame", attach);
        PrintFrameSamples("detach_frame", detach);
    }

    TEST_CASE("Mounted live cell submits verified Navigation membership with its Scene transaction",
              "[world_cook][mounted_world][navigation]") {
        Fixture fixture{true, false};
        const auto tiles = CaptureNavigationCell(fixture);
        const auto identity = fixture.inputs.front().expected;
        fixture.navigationCells.push_back({identity.cell, tiles});
        auto limits = Limits();
        limits.cell.maximumPayloads = 2;
        const auto published = fixture.Publish(limits);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);
        const CommittedWorldSelection selection{.targetRoot = fixture.directory.path,
                                                .target = fixture.Release().target,
                                                .manifest = published.Value().publication->manifestDigest,
                                                .index = Asset(6),
                                                .dependencies = Asset(7)};
        S::RuntimeSceneService scene;
        S::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add({.object = {42}});
        const HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(scene, std::move(startup).Build().Value(), boundary).HasValue());
        JobSystem jobs{JobSystemConfig{1, 4}};
        const auto prepared = MountedWorld::Prepare(NativeDurableFileSystem{}, selection, 5, scene);
        REQUIRE(prepared.HasValue());
        auto &mount = *prepared.Value();
        DeterministicClock clock;
        auto host = S::RuntimeHost::Create(clock).Value();
        auto driver = std::make_unique<MountedCellDriver>(mount, scene);
        const auto &progress = *driver;
        REQUIRE(host->AddParticipant(std::move(driver)).HasValue());
        REQUIRE(mount.BindLiveCell(host->DispatchSource(), jobs, {20'000'000, 1'000'000, 4}).HasValue());
        const CommittedWorldInitialCell request{identity.cell, {101}, identity.scene, identity.revision};
        W::TestSupport::RequireError(mount.RequestLiveCell(request), W::WorldArtifactErrors::Unsupported);
        std::size_t residentTiles{};
        REQUIRE(mount
                    .BindLiveNavigation([&](const S::RuntimeSceneView &view, std::span<const MountedWorldNavigationCell> desired,
                                            MountedWorldNavigationChange change) {
            CHECK(view.Find({42}));
            CHECK(change == (desired.empty() ? MountedWorldNavigationChange::Retire : MountedWorldNavigationChange::Attach));
            if (!desired.empty()) {
                REQUIRE(desired.size() == 1);
                CHECK(desired.front().cell == identity.cell);
                CHECK(desired.front().selection.tiles == tiles.tiles);
                CHECK(ComputeSha256(std::as_bytes(desired.front().envelope)) == tiles.cookedEnvelope);
            }
            return Result<MountedWorldNavigationPreparation>::Success(
                {std::make_shared<ObservedCellNavigation>(residentTiles, desired.size()), false});
        }).HasValue());
        REQUIRE(host->Startup().HasValue());
        const auto until = [&](Stage expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do {
                clock.Advance(Duration::FromMilliseconds(17));
                REQUIRE(host->RunFrame().HasValue());
                REQUIRE(progress.last.HasValue());
                if (progress.last.Value() == expected)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Live navigation cell did not settle");
        };
        REQUIRE(mount.RequestLiveCell(request).HasValue());
        until(Stage::Active);
        CHECK(residentTiles == 1);
        CHECK(scene.ActiveScene()->Find({11}));
        const auto visible = mount.InspectLiveCell(request.occurrence);
        REQUIRE(visible.HasValue());
        CHECK(visible.Value().geometryVisible);
        CHECK(visible.Value().fence.cell == identity.cell);
        CHECK(visible.Value().navigation == MountedWorldCellNavigationReadiness::Unavailable);
        REQUIRE(mount.WithdrawLiveCell(request.occurrence).HasValue());
        until(Stage::Idle);
        CHECK(residentTiles == 0);
        CHECK_FALSE(scene.ActiveScene()->Find({11}));
        host->Shutdown();
    }
#if HORO_TEST_NAVIGATION_NATIVE
    TEST_CASE("Mounted Detour exposes coverage only while its verified cell is resident",
              "[world_cook][mounted_world][navigation][native]") {
        namespace N = Navigation;
        Fixture fixture{true, false};
        const auto profile = IdentityFrom<N::NavigationAgentProfileId>(7);
        const auto surface = IdentityFrom<N::SurfaceId>(9);
        const auto area = IdentityFrom<N::NavigationAreaId>(11);
        const auto filter = IdentityFrom<N::NavigationFilterId>(13);
        const auto scope = N::NavigationBakeScopeId::ForScene(Asset(42).Bytes()).Value();
        const auto digest = W::CandidateTestSupport::Hash();
        const N::NavigationAgentBuildGeometry geometry{.cellSizeMeters = 0.5F};
        auto definition =
            N::NavigationDefinition::Create({.profiles = {{profile, "Grounded", geometry}},
                                             .areas = {{area, {.id = IdentityFrom<N::NavigationDescriptorSourceId>(1)}, 1.0F, {.bits = 1}}},
                                             .filters = {{filter, {.id = IdentityFrom<N::NavigationDescriptorSourceId>(1)}, {.bits = 1}}}});
        REQUIRE(definition.HasValue());
        N::NavigationProjectProfileInput policyInput{.id = IdentityFrom<N::NavigationProjectProfileId>(5),
                                                     .revision = IdentityFrom<N::NavigationProjectProfileRevision>(3),
                                                     .capacities = {8, 1, 4, 2, 65536, 16U * 1024U * 1024U, 8192},
                                                     .maximumQuery = {N::NavigationQueryKind::Path,
                                                                      N::NavigationQualityLevel::Balanced,
                                                                      {64, 16, 100}}};
        policyInput.capabilities.fill(N::NavigationCapabilityRequirement::Optional);
        policyInput.capabilities[static_cast<std::size_t>(N::NavigationCapability::GroundedQueries)] =
            N::NavigationCapabilityRequirement::Required;
        const auto policy = N::NavigationProjectProfile::Create(policyInput).Value();
        const N::NavigationPreparedTile input{.tile = {.key = {profile, surface, {}, scope},
                                                       .bounds = {{0, -1, 0}, {8, 1, 8}},
                                                       .tileSizeMeters = 8},
                                              .geometry = geometry,
                                              .borderSizeCells = 4,
                                              .dependencyKey = digest};
        auto tile =
            N::NavigationCookedTile::Create(input, {.state = N::NavigationTileBuildState::Built,
                                                    .bounds = input.tile.bounds,
                                                    .vertices = {{0, 0, 0}, {8, 0, 0}, {0, 0, 8}},
                                                    .polygons = {{{0, 3}, {0, 3}, area}},
                                                    .polygonVertexIndices = {0, 1, 2},
                                                    .polygonAdjacencies = {N::NavMeshBoundaryAdjacency, N::NavMeshBoundaryAdjacency,
                                                                           N::NavMeshBoundaryAdjacency},
                                                    .provenance = {{.producer = IdentityFrom<N::NavigationSourceProducerId>(1),
                                                                    .contribution = IdentityFrom<N::NavigationSourceContributionId>(1),
                                                                    .revision = IdentityFrom<N::NavigationSourceRevision>(1),
                                                                    .sourceDigest = digest,
                                                                    .polygons = {0, 1}}}});
        REQUIRE(tile.HasValue());
        auto emptyInput = input;
        emptyInput.tile.key.tile.x = 1;
        emptyInput.tile.bounds = {{8, -1, 0}, {16, 1, 8}};
        const auto empty = N::NavigationCookedTile::Create(emptyInput, {.key = emptyInput.tile.key.tile, .bounds = emptyInput.tile.bounds});
        REQUIRE(empty.HasValue());
        const N::NavigationCookedTileSet closure{digest,
                                                 {tile.Value(), empty.Value()},
                                                 N::NavigationCookedContentProvenance{{digest, digest, digest}, policy}};
        auto &base = fixture.baseArtifacts.front();
        base = N::TestSupport::ContentEnvelope(closure, Asset(8), fixture.Release().target);
        const auto rootDigest = T::DigestOf(base);
        std::get<BaseObjectBinding>(fixture.objects.back()).artifactHash = rootDigest;
        const auto identity = fixture.inputs.front().expected;
        const auto sibling = fixture.inputs.back().expected;
        fixture.navigationCells.push_back({identity.cell, {Asset(8), rootDigest, {tile.Value()->Key()}}});
        fixture.navigationCells.push_back({sibling.cell, {Asset(8), rootDigest, {empty.Value()->Key()}}});
        auto limits = Limits();
        limits.cell.maximumPayloads = 2;
        const auto published = fixture.Publish(limits);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().publication);

        JobSystem jobs{JobSystemConfig{1, 4}};
        auto selected = std::make_shared<const ProjectSceneNavigationSelection>(
            ProjectSceneNavigationSelection{{surface, scope},
                                            Asset(8),
                                            profile,
                                            fixture.Release().target,
                                            policy,
                                            std::make_shared<const N::NavigationDefinition>(std::move(definition).Value()),
                                            jobs,
                                            1});
        auto navigation = CreateProjectSceneNavigationWorld(*selected).Value();
        A::MemoryAssetProvider provider;
        provider.Insert(Asset(8), base);
        A::AssetLoadService loads{jobs, provider};
        A::AssetRegistry registry;
        REQUIRE(registry.Publish({Physics::Record(Asset(8), T::Type("core.navmesh"))}).status == A::AssetRegistryBuildStatus::Complete);
        S::RuntimeSceneService scene{registry, loads, {}, {101}};
        const CommittedWorldSelection world{.targetRoot = fixture.directory.path,
                                            .target = fixture.Release().target,
                                            .manifest = published.Value().publication->manifestDigest,
                                            .index = Asset(6),
                                            .dependencies = Asset(7)};
        auto mount = MountedWorld::Prepare(NativeDurableFileSystem{}, world, 5, scene).Value();
        REQUIRE(mount->HasLiveNavigationContent());
        REQUIRE(RegisterProjectSceneNavigation(scene, *navigation, selected, true).HasValue());
        S::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add(
            {.object = {42},
             .components = {
                 .navigationSurface =
                     S::NavigationSurfaceComponent{.id = surface, .definition = Asset(8), .generation = 1, .profiles = {profile}}}});
        REQUIRE(startup.RequireAsset({Asset(8), T::Type("core.navmesh")}).HasValue());
        const HeadlessSceneLoadBoundary boundary{{}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
        const auto definitionForStartup = std::move(startup).Build().Value();
        REQUIRE(PrepareHeadlessSceneAtLoadBoundary(scene, definitionForStartup, boundary).HasValue());
        W::TestSupport::RequireError(navigation->assets->Acquire(), N::NavigationErrors::NoNavigationData);
        CHECK(navigation->cache->Snapshot().retainedPayloadBytes == 0);

        DeterministicClock clock;
        auto host = S::RuntimeHost::Create(clock).Value();
        auto driver = std::make_unique<MountedCellDriver>(*mount, scene);
        auto &progress = *driver;
        REQUIRE(host->AddParticipant(std::move(driver)).HasValue());
        REQUIRE(mount->BindLiveCell(host->DispatchSource(), jobs, {20'000'000, 1'000'000, 4, 0, 0, 2}).HasValue());
        REQUIRE(BindMountedWorldNavigation(*mount, *navigation, selected).HasValue());
        REQUIRE(host->Startup().HasValue());
        const auto frame = [&] {
            clock.Advance(Duration::FromMilliseconds(17));
            REQUIRE(host->RunFrame().HasValue());
        };
        const auto until = [&](Stage expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do {
                frame();
                REQUIRE(progress.last.HasValue());
                if (progress.last.Value() == expected)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Native navigation cell did not settle");
        };
        REQUIRE(mount->RequestLiveCell({identity.cell, {101}, identity.scene, identity.revision}).HasValue());
        until(Stage::Active);
        const auto liveReadiness = mount->InspectLiveCell({101});
        REQUIRE(liveReadiness.HasValue());
        CHECK(liveReadiness.Value().geometryVisible);
        CHECK(liveReadiness.Value().navigation == MountedWorldCellNavigationReadiness::Ready);
        REQUIRE(mount->RequestLiveCell({sibling.cell, {102}, sibling.scene, sibling.revision}).HasValue());
        until(Stage::Active);
        const auto siblingReadiness = mount->InspectLiveCell({102});
        REQUIRE(siblingReadiness.HasValue());
        CHECK(siblingReadiness.Value().geometryVisible);
        CHECK(siblingReadiness.Value().fence.cell == sibling.cell);
        CHECK(siblingReadiness.Value().navigation == MountedWorldCellNavigationReadiness::Unavailable);
        auto old = navigation->assets->Acquire();
        REQUIRE(old.HasValue());
        auto retained = std::move(old).Value();
        const auto active = retained.Descriptor();
        const N::NavigationPathRequest request{.world = active.world,
                                               .topology = active.topology,
                                               .start = {1, 0, 1},
                                               .destination = {2, 0, 2},
                                               .filter = filter,
                                               .requirement = policy.MaximumQuery()};
        const auto path = retained.Backend().FindPath(request, {});
        REQUIRE(path.HasValue());
        CHECK(path.Value().status == N::NavigationPathStatus::Reachable);
        const auto lifecycle = navigation->assets->ActiveWorld();
        const std::array callers{
            N::NavigationPathCaller{IdentityFrom<N::NavigationDynamicOwnerId>(1), IdentityFrom<N::NavigationDynamicOwnerGeneration>(1)}};
        const auto epoch = active.topology.Value();
        const N::NavigationOutcomeProvenance source{IdentityFrom<N::NavigationSnapshotToken>(epoch),
                                                    active.world,
                                                    active.topology,
                                                    epoch,
                                                    epoch,
                                                    policy.Revision().Value(),
                                                    epoch};
        const auto queued = navigation->coordinator.Submit(*lifecycle,
                                                           {.caller = callers.front(),
                                                            .request = request,
                                                            .source = source,
                                                            .capabilityRevision = policy.Revision().Value(),
                                                            .targetTick = 1,
                                                            .deadlineTick = 8},
                                                           1);
        REQUIRE(queued.HasValue());
        W::TestSupport::RequireError(mount->ForceEvictLiveCell({999}), W::WorldArtifactErrors::Stale);
        REQUIRE(mount->ForceEvictLiveCell({101}).HasValue());
        until(Stage::Retiring);
        const auto retiringReadiness = mount->InspectLiveCell({101});
        REQUIRE(retiringReadiness.HasValue());
        CHECK_FALSE(retiringReadiness.Value().geometryVisible);
        CHECK(retiringReadiness.Value().fence == liveReadiness.Value().fence);
        CHECK(retiringReadiness.Value().navigation == MountedWorldCellNavigationReadiness::Unavailable);
        W::TestSupport::RequireError(navigation->assets->Acquire(), N::NavigationErrors::NoNavigationData);
        CHECK(retained.Revocation() == N::NavigationWorldRevocation::TileEvicted);
        CHECK(retained.Cancellation().IsCancellationRequested());
        CHECK(navigation->assets->Snapshot().reservedProviderBytes > 0);
        until(Stage::Retiring);
        CHECK_FALSE(scene.ActiveScene()->Find({11}));
        const auto publishedTopology = lifecycle->ActiveDescriptor().Value();
        CHECK(publishedTopology.world == active.world);
        auto currentSource = source;
        currentSource.topology = publishedTopology.topology;
        currentSource.snapshot = IdentityFrom<N::NavigationSnapshotToken>(publishedTopology.topology.Value());
        REQUIRE(navigation->coordinator.Commit({publishedTopology, currentSource, callers, 2}) == 1);
        const auto completion = navigation->coordinator.Take(queued.Value());
        REQUIRE(completion);
        W::TestSupport::RequireError(completion->result, N::NavigationErrors::TileEvicted);
        CHECK(completion->activation == active);
        CHECK(completion->source.snapshot == source.snapshot);
        CHECK(completion->source.topology == source.topology);
        retained = {};
        until(Stage::Active);
        CHECK(navigation->assets->Snapshot().reservedProviderBytes == 0);
        CHECK(mount->InspectLiveCell({102}).Value().geometryVisible);
        REQUIRE(mount->WithdrawLiveCell({102}).HasValue());
        until(Stage::Idle);

        const auto sourceScene = scene.ActiveScene();
        REQUIRE(sourceScene);
        const CommittedWorldInitialCell demand{identity.cell, {101}, identity.scene, identity.revision};
        REQUIRE(mount->RequestLiveCell(demand).HasValue());
        until(Stage::Prepared);
        // Hold the decoded result across a committed root replacement, then resume its ordinary owner cutoff.
        progress.advanceMount = false;
        REQUIRE(scene.QueuePreparation(definitionForStartup).HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            frame();
            REQUIRE_FALSE(scene.TakeOperationError());
            if (const auto activeScene = scene.ActiveScene(); activeScene && !sourceScene->SameIncarnation(*activeScene))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        const auto replacement = scene.ActiveScene();
        REQUIRE(replacement);
        REQUIRE_FALSE(sourceScene->SameIncarnation(*replacement));
        progress.advanceMount = true;
        frame();
        W::TestSupport::RequireError(progress.last, S::SceneCellPayloadErrors::Stale);
        W::TestSupport::RequireError(mount->InspectLiveCell(demand.occurrence), W::WorldArtifactErrors::Stale);
        W::TestSupport::RequireError(navigation->assets->Acquire(), N::NavigationErrors::NoNavigationData);

        REQUIRE(mount->RequestLiveCell(demand).HasValue());
        until(Stage::Active);
        const auto replacementCell = mount->InspectLiveCell(demand.occurrence);
        REQUIRE(replacementCell.HasValue());
        CHECK(replacementCell.Value().geometryVisible);
        CHECK(replacementCell.Value().navigation == MountedWorldCellNavigationReadiness::Ready);
        CHECK(replacementCell.Value().fence.generation > liveReadiness.Value().fence.generation);
        host->Shutdown();
    }
#endif
}  // namespace Horo::WorldCook
