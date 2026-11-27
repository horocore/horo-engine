#include "AiSceneTestSupport.h"
#include "HeadlessNetworkServices.h"
#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "PhysicsSceneActivationTestSupport.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Application {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Publishes the fixture's three explicit world positions in entity order. */
        Result<std::vector<AI::AiScenePerceptionPosition>> HearingPositions(const Runtime::RuntimeSceneView &scene) {
            std::vector<AI::AiScenePerceptionPosition> positions;
            for (std::uint64_t identity : {1U, 2U, 3U}) {
                const auto entity = scene.Find({identity});
                REQUIRE(entity);
                positions.push_back({*entity, Math::WorldCoordinate64::FromMillimeters(static_cast<std::int64_t>(identity - 1) * 1000,
                                                                                     0, 0)});
            }
            return Result<std::vector<AI::AiScenePerceptionPosition>>::Success(std::move(positions));
        }

        /** @brief Admits the built-in hearing endpoints without loading an Audio or native sense provider. */
        std::shared_ptr<const AI::AiControllerPerceptionEndpoints> HearingEndpoints(const AI::PerceptionListenerTypeId listener) {
            const AI::PerceptionDescriptorOrigin origin{.provider = AI::TestSupport::MakeIdentity<AI::PerceptionProviderId>(71)};
            const std::array senses{AI::SenseTypeDescriptor{AI::SenseTypeIds::Hearing, origin, "Hearing"}};
            const std::array stimuli{AI::StimulusTypeDescriptor{AI::StimulusTypeIds::Sound, origin, "Sound"}};
            const std::array listeners{AI::PerceptionListenerDescriptor{.identity = listener,
                                                                        .origin = origin,
                                                                        .displayName = "Hearing",
                                                                        .sense = AI::SenseTypeIds::Hearing,
                                                                        .stimuli = {{AI::StimulusTypeIds::Sound}},
                                                                        .stimulusCount = 1}};
            auto registry = AI::PerceptionDescriptorRegistry::Capture({senses, stimuli, listeners}, {});
            REQUIRE(registry.HasValue());
            return std::make_shared<const AI::AiControllerPerceptionEndpoints>(
                AI::AiControllerPerceptionEndpoints{
                    .registry = std::make_shared<const AI::PerceptionDescriptorRegistry>(std::move(registry).Value()),
                    .listeners = {listener},
                    .sources = {AI::SenseTypeIds::Hearing}});
        }

        /** @brief Captures a committed batch from an approved producer at the live host's exact sensing boundary. */
        class HeadlessHearingProducer final : public Runtime::RuntimeLifecycleParticipant {
        public:
            using Window = Result<std::optional<Gameplay::PerceptionEventSource>>;

            HeadlessHearingProducer(const AI::AiControllerPerceptionEndpoints &endpoints, const AI::PerceptionListenerTypeId listener)
                : endpoints_(endpoints), listener_(listener) {}

            void Bind(Network::NetworkModeComposition &mode, Runtime::RuntimeDispatchSource dispatch,
                      const Runtime::RuntimeSceneView &scene) {
                host_ = &mode;
                scene_ = scene;
                publisher_.emplace(std::move(dispatch), scene);
            }

            [[nodiscard]] Window Capture(const Runtime::RuntimeSceneView &scene, const AI::AiSceneRuntime &ai,
                                         const AI::PerceptionSpatialBroadphase &spatial, const std::uint64_t tick) {
                if (tick == 1)
                    SelectGameplayDeliveries(scene, ai);
                const auto batch = publisher_->Read(scene, tick);
                if (batch.HasError())
                    return Window::Failure(batch.ErrorValue());
                if (batch.Value().empty())
                    return Window::Success(std::nullopt);
                auto captured = Gameplay::PerceptionEventSource::Capture(scene, ai, *host_, Network::NetworkModeWorldKind::Standalone,
                                                                           *endpoints_.registry, spatial, batch.Value());
                if (captured.HasError())
                    return Window::Failure(captured.ErrorValue());
                return Window::Success(std::move(captured).Value());
            }

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }
            Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
                return host_->RunPhase(phase, context.completedSimulationTick, context.cancellation);
            }
            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
                return host_->RunFixedTick(context);
            }
            Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
                if (context.simulationTick != 1)
                    return Result<void>::Success();
                return publisher_->Prepare(scene_, context.dispatchEvidence, staged_);
            }
            void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                if (context.simulationTick == 1)
                    committed = publisher_->Commit(context.dispatchEvidence);
            }
            void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                publisher_->Abort(context.dispatchEvidence);
            }
            void Shutdown() noexcept override {}

            inline static const auto FinalSound = Math::WorldCoordinate64::FromMillimeters(1900, 0, 0);
            bool committed{};

        private:
            /** @brief Selects the game scenario's sound facts and explicit disclosure before its first commit. */
            void SelectGameplayDeliveries(const Runtime::RuntimeSceneView &scene, const AI::AiSceneRuntime &ai) {
                scene_ = scene;
                const auto source = scene.Find({3});
                REQUIRE(source);
                const auto admission = ai.CapabilityAtSafePoint();
                const auto allowed = admission.ResolveAgent(AI::TestSupport::MakeIdentity<AI::AgentId>(1));
                const auto denied = admission.ResolveAgent(AI::TestSupport::MakeIdentity<AI::AgentId>(2));
                REQUIRE(allowed.HasValue());
                REQUIRE(denied.HasValue());
                using enum Gameplay::PerceptionFilterDecision;
                const auto older = AI::HearingPerceptionEvent{AI::ProjectPerceptionSource(*source),
                                                              Math::WorldCoordinate64::FromMillimeters(1800, 0, 0),
                                                              0.5, 4, 1, 0, 1};
                auto latest = older;
                latest.location = FinalSound;
                staged_ = {{{allowed.Value(), listener_, older, Accept},
                            {allowed.Value(), listener_, latest, Accept},
                            {denied.Value(), listener_, latest, Reject}}};
            }

            const AI::AiControllerPerceptionEndpoints &endpoints_;
            AI::PerceptionListenerTypeId listener_;
            Network::NetworkModeComposition *host_{};
            Runtime::RuntimeSceneView scene_;
            std::array<Gameplay::GameplayHearingDelivery, 3> staged_{};
            std::optional<Gameplay::GameplayHearingPublisher> publisher_;
        };

        /** @brief Selects the real standalone fixed lifecycle with no Audio presentation factory. */
        std::unique_ptr<Network::NetworkModeComposition> CreateSilentHearingHost(
            Network::NetworkModeFactories factories, Network::INetworkModeService *&worldScene) {
            using namespace Network;
            using AI::TestSupport::MakeIdentity;
            auto &sceneFactory = factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
            sceneFactory = [create = std::move(sceneFactory), &worldScene](const NetworkModeServiceRequest &request) {
                auto service = create(request);
                if (service.HasValue())
                    worldScene = service.Value().get();
                return service;
            };
            CHECK_FALSE(factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Audio)]);
            const NetworkModePlan plan{.mode = NetworkProjectRole::Standalone,
                                       .hostGeneration = 1,
                                       .build = MakeIdentity<NetworkProductBuildId>(10),
                                       .productRevision = MakeIdentity<NetworkProductCapabilityRevision>(11),
                                       .hostRevision = MakeIdentity<NetworkHostCapabilityRevision>(12),
                                       .projectRevision = MakeIdentity<NetworkProjectSettingsRevision>(13),
                                       .worlds = {{{NetworkModeWorldKind::Standalone, {7}}}},
                                       .worldCount = 1};
            auto created = NetworkModeComposition::Create(plan, std::move(factories));
            REQUIRE(created.HasValue());
            return std::make_unique<NetworkModeComposition>(std::move(created).Value());
        }

        /** @brief Confirms the sound is absent before commitment and observable only on the following fixed tick. */
        void AdvanceHearingProbe(Runtime::RuntimeLifecycle &lifecycle, std::unique_ptr<HeadlessHearingProducer> producer,
                                 Network::NetworkModeComposition &host, Network::INetworkModeService &worldScene) {
            DeterministicClock clock;
            const auto quantum = Duration::FromNanoseconds(16'666'667);
            auto scheduler = Runtime::FrameScheduler::Create(clock, {.fixedStep = quantum});
            REQUIRE(scheduler.HasValue());
            const auto scene = Internal::InspectHeadlessScene(worldScene);
            REQUIRE(scene);
            const auto allowed = scene->Find({1});
            REQUIRE(allowed);
            auto &probe = *producer;
            probe.Bind(host, scheduler.Value()->DispatchSource(), *scene);
            const CancellationToken cancel;
            REQUIRE(lifecycle.AddParticipant(std::move(producer)).HasValue());
            REQUIRE(lifecycle.Startup(cancel).HasValue());
            REQUIRE(scheduler.Value()->RunFrame(lifecycle, cancel, false).HasValue());
            clock.Advance(quantum);
            REQUIRE(scheduler.Value()->RunFrame(lifecycle, cancel, false).HasValue());
            REQUIRE(probe.committed);
            REQUIRE(Internal::InspectHeadlessPerceptionMemory(worldScene, *allowed).Value().count == 0);
            clock.Advance(quantum);
            REQUIRE(scheduler.Value()->RunFrame(lifecycle, cancel, false).HasValue());
            lifecycle.Shutdown();
        }

        /** @brief Checks coalescing for the permitted recipient and disclosure denial for the other agent. */
        void RequireHeadlessHearingOutcome(const Network::INetworkModeService &worldScene) {
            const auto scene = Internal::InspectHeadlessScene(worldScene);
            REQUIRE(scene);
            const auto allowed = scene->Find({1});
            const auto denied = scene->Find({2});
            REQUIRE(allowed);
            REQUIRE(denied);
            const auto accepted = Internal::InspectHeadlessPerceptionMemory(worldScene, *allowed);
            REQUIRE(accepted.HasValue());
            REQUIRE(accepted.Value().count == 1);
            CHECK(accepted.Value().entries.front().lastKnownPosition == HeadlessHearingProducer::FinalSound);
            const auto withheld = Internal::InspectHeadlessPerceptionMemory(worldScene, *denied);
            REQUIRE(withheld.HasValue());
            CHECK(withheld.Value().count == 0);
        }

        TEST_CASE("Headless fixed sensing hears committed Gameplay sound without an Audio service",
                  "[prefab][integration][ai][perception][hearing]") {
            using namespace Network;
            using AI::TestSupport::MakeIdentity;
            const auto listener = MakeIdentity<AI::PerceptionListenerTypeId>(91);
            auto controller = AI::TestSupport::MakeFixture(1, 11, 1, 0);
            controller.agent.controller->requiredCapabilities = AI::AiCapabilitySet::Of(AI::AiCapability::Perception);
            controller.descriptor.perceptionEndpoints = HearingEndpoints(listener);
            Runtime::RuntimeLifecycle lifecycle;
            auto producer = std::make_unique<HeadlessHearingProducer>(*controller.descriptor.perceptionEndpoints, listener);

            Runtime::SceneDefinitionBuilder source{{7}, {1}};
            for (std::uint64_t identity : {1U, 2U, 3U}) {
                auto agent = controller.agent.agent;
                agent.agent = MakeIdentity<AI::AgentId>(identity);
                source.Add({.object = {identity}, .components = {.aiAgent = agent, .aiController = controller.agent.controller}});
            }
            auto definition = std::move(source).Build();
            REQUIRE(definition.HasValue());
            JobSystem jobs{{1, 8}};
            Assets::MemoryAssetProvider bytes;
            Assets::AssetLoadService loader{jobs, bytes};
            Assets::AssetRegistry registry;
            const Internal::HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{3}};
            const std::array catalog{controller.descriptor};
            auto generation = AI::PerceptionSenseGeneration::Capture({}, std::make_shared<const std::atomic_bool>(true));
            REQUIRE(generation.HasValue());
            auto composed = Internal::ComposeHeadlessSceneNetworkServices(
                std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(definition).Value()),
                {.mode = Physics::PhysicsRuntimeMode::Canonical,
                 .settings = Physics::Settings(),
                 .registry = registry,
                 .loads = loader,
                 .load = load,
                 .aiControllers = catalog,
                 .perceptionPositions = HearingPositions,
                 .perceptionExecution = AI::AiScenePerceptionExecution{
                     .generation = std::move(generation).Value(), .visibleLayers = 1, .workUnitsPerTick = 1},
                 .perceptionEvents = [producer = producer.get()](const auto &scene, const auto &ai, const auto &spatial, const auto tick) {
                     return producer->Capture(scene, ai, spatial, tick);
                 }});
            REQUIRE(composed.HasValue());

            INetworkModeService *worldScene{};
            auto host = CreateSilentHearingHost(std::move(composed).Value().factories, worldScene);
            REQUIRE(host->Start().HasValue());
            REQUIRE(worldScene);
            AdvanceHearingProbe(lifecycle, std::move(producer), *host, *worldScene);
            RequireHeadlessHearingOutcome(*worldScene);
        }
    }  // namespace
#endif
}  // namespace Horo::Application
