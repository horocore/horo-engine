#include "AiTestSupport.h"
#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/Gameplay/PerceptionEventSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        using Gameplay::CommittedPerceptionDelivery;
        using Gameplay::PerceptionEventSource;
        using Gameplay::PerceptionFilterDecision;
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        static_assert(!std::is_default_constructible_v<PerceptionEventAdmission>);

        [[nodiscard]] PerceptionListenerDescriptor Listener(const std::uint64_t id, const SenseTypeId sense,
                                                            const StimulusTypeId stimulus) {
            PerceptionListenerDescriptor descriptor;
            descriptor.identity = MakeIdentity<PerceptionListenerTypeId>(id);
            descriptor.origin = {.provider = MakeIdentity<PerceptionProviderId>(1)};
            descriptor.displayName = "event listener";
            descriptor.sense = sense;
            descriptor.stimuli[0].identity = stimulus;
            descriptor.stimulusCount = 1;
            return descriptor;
        }

        [[nodiscard]] PerceptionDescriptorRegistry Registry() {
            const PerceptionDescriptorOrigin origin{.provider = MakeIdentity<PerceptionProviderId>(1)};
            const std::array senses{SenseTypeDescriptor{SenseTypeIds::Damage, origin, "damage", {}},
                                    SenseTypeDescriptor{SenseTypeIds::Touch, origin, "touch", {}},
                                    SenseTypeDescriptor{SenseTypeIds::Team, origin, "team", {}}};
            const std::array stimuli{StimulusTypeDescriptor{StimulusTypeIds::Damage, origin, "damage", 1, {}},
                                     StimulusTypeDescriptor{StimulusTypeIds::Contact, origin, "contact", 1, {}},
                                     StimulusTypeDescriptor{StimulusTypeIds::Team, origin, "team", 1, {}}};
            const std::array listeners{Listener(10, SenseTypeIds::Damage, StimulusTypeIds::Damage),
                                       Listener(11, SenseTypeIds::Touch, StimulusTypeIds::Contact),
                                       Listener(12, SenseTypeIds::Team, StimulusTypeIds::Team)};
            auto result = PerceptionDescriptorRegistry::Capture({senses, stimuli, listeners}, {});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Fake service readiness; role validation executes in the production host composition. */
        class ReadyService final : public Network::INetworkModeService {
        public:
            Result<void> Prepare() override {
                return Result<void>::Success();
            }

            Result<void> Activate() override {
                return Result<void>::Success();
            }

            Result<void> RunPhase(Runtime::RuntimePhase) override {
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                // This readiness-only fixture owns no resources; the production host revokes its role grants.
            }
        };

        [[nodiscard]] Network::NetworkModeComposition Host(const Network::NetworkProjectRole mode) {
            Network::NetworkTargetAssessment assessment;
            auto &matrix = assessment.matrix;
            matrix.build = MakeIdentity<Network::NetworkProductBuildId>(10);
            matrix.productRevision = MakeIdentity<Network::NetworkProductCapabilityRevision>(11);
            matrix.hostRevision = MakeIdentity<Network::NetworkHostCapabilityRevision>(12);
            matrix.projectRevision = MakeIdentity<Network::NetworkProjectSettingsRevision>(13);
            matrix.roles[static_cast<std::size_t>(mode)].selected = true;
            assessment.diagnostic.reason = Network::NetworkTargetFailureReason::None;
            Network::NetworkTargetSelection selection{.role = mode,
                                                      .expectedBuild = matrix.build,
                                                      .expectedProductRevision = matrix.productRevision,
                                                      .expectedHostRevision = matrix.hostRevision,
                                                      .expectedProjectRevision = matrix.projectRevision};
            Network::NetworkModeWorld world{.kind = Network::NetworkModeWorldKind::Standalone, .scene = Runtime::SceneRuntimeId{99}};
            if (mode != Network::NetworkProjectRole::Standalone) {
                selection.provider = MakeIdentity<Network::NetworkTransportProviderId>(14);
                selection.protocolVersion = {1, 0};
                world.kind = mode == Network::NetworkProjectRole::Client ? Network::NetworkModeWorldKind::Client
                                                                         : Network::NetworkModeWorldKind::AuthorityServer;
                if (world.kind == Network::NetworkModeWorldKind::AuthorityServer)
                    world.authority = Network::ReplicationAuthorityEpoch::Create(1).Value();
            }
            const Network::NetworkModePresentation presentation{.localPlayer = mode == Network::NetworkProjectRole::Client};
            auto plan = Network::ResolveNetworkModePlan(assessment, selection, 17, std::span{&world, 1}, presentation);
            REQUIRE(plan.HasValue());
            Network::NetworkModeFactories factories;
            for (auto &factory : factories.services)
                factory = [](const Network::NetworkModeServiceRequest &) {
                    return Result<std::unique_ptr<Network::INetworkModeService>>::Success(std::make_unique<ReadyService>());
                };
            auto host = Network::NetworkModeComposition::Create(plan.Value(), std::move(factories));
            REQUIRE(host.HasValue());
            auto composition = std::move(host).Value();
            REQUIRE(composition.Start().HasValue());
            return composition;
        }

        [[nodiscard]] AiControllerDescriptor Controller() {
            BlackboardSchemaDescriptor schemaInput;
            schemaInput.identity = MakeIdentity<BlackboardSchemaId>(1);
            schemaInput.version = 1;
            const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(2),
                                                          .kind = BlackboardValueKind::Boolean,
                                                          .maximumCollectionElements = 1,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{false}}}};
            schemaInput.keys = keys;
            auto captured = BlackboardSchema::Capture(schemaInput);
            REQUIRE(captured.HasValue());
            auto schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
            const DecisionNodeTypeId nodeType = MakeIdentity<DecisionNodeTypeId>(3);
            DecisionNodeDescriptor node;
            node.type = nodeType;
            node.origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(4), 1};
            node.source = {"assets/ai/event_consumer.horo", 1, 1};
            DecisionAssetDescriptor asset;
            asset.asset = MakeIdentity<DecisionGraphAssetId>(5);
            asset.kind = DecisionPlanKind::BehaviorTree;
            asset.schemaVersion = CurrentDecisionAssetSchemaVersion;
            asset.blackboardSchema = schema->Identity();
            asset.requiredBlackboardSchemaVersion = {1, 1};
            asset.source = node.source;
            asset.nodes.push_back(
                {.id = MakeIdentity<DecisionNodeId>(6), .type = nodeType, .descriptorVersion = {1, 1}, .source = node.source});
            const auto compiled = DecisionAssetCompiler::Compile(asset, {}, std::array{node}, std::array{schema});
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().IsValid());
            return {.controller = MakeIdentity<ControllerTypeId>(7),
                    .decisionAsset = asset.asset,
                    .decisionKind = asset.kind,
                    .blackboardSchema = std::move(schema),
                    .decisionPlan = compiled.Value().plan};
        }

        struct Fixture final {
            std::unique_ptr<Runtime::RuntimeScene> scene;
            AiSceneRuntime ai{std::move(AiSceneRuntime::Create()).Value()};
            Network::NetworkModeComposition host;
            PerceptionDescriptorRegistry registry{Registry()};
            std::unique_ptr<AiSceneActivationCandidate> activation;
            AgentHandle recipient;
            AgentHandle other;

            explicit Fixture(const Network::NetworkProjectRole mode = Network::NetworkProjectRole::Standalone) : host(Host(mode)) {
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
                for (std::uint64_t object = 1; object <= 4; ++object) {
                    Runtime::RuntimeEntityDefinition entity;
                    entity.object = Runtime::SceneObjectId{object};
                    builder.Add(entity);
                }
                const auto definition = std::move(builder).Build();
                REQUIRE(definition.HasValue());
                auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{99});
                REQUIRE(created.HasValue());
                scene = std::move(created).Value();
                const auto controller = Controller();
                AiControllerComponent binding;
                binding.controller = controller.controller;
                binding.decisionAsset = controller.decisionAsset;
                binding.blackboardSchema = controller.blackboardSchema->Identity();
                binding.requiredCapabilities = AiCapabilitySet::Of(AiCapability::Perception);
                const std::array agents{AiSceneAgentDescriptor{Entity(1), {.agent = MakeIdentity<AgentId>(1)}, binding},
                                        AiSceneAgentDescriptor{Entity(4), {.agent = MakeIdentity<AgentId>(4)}, binding}};
                auto prepared = ai.PrepareScene({AiRuntimeIncarnation::Create(77).Value(), scene->View().RuntimeId()}, agents,
                                                std::span{&controller, 1});
                REQUIRE(prepared.HasValue());
                activation = std::move(prepared).Value();
                REQUIRE(activation->ValidatePublication().HasValue());
                activation->Publish();
                const auto snapshot = ai.Snapshot().Value();
                recipient = snapshot.Agents()[0].handle;
                other = snapshot.Agents()[1].handle;
            }

            [[nodiscard]] Runtime::EntityRef Entity(const std::uint64_t object) const {
                return scene->View().Find(Runtime::SceneObjectId{object}).value();
            }

            [[nodiscard]] PerceptionSourceRef Source(const std::uint64_t object) const {
                return ProjectPerceptionSource(Entity(object));
            }

            [[nodiscard]] AIPerceptionMemory Memory(const AgentHandle agent) const {
                return std::move(AIPerceptionMemory::Create(99, agent)).Value();
            }

            [[nodiscard]] CommittedPerceptionDelivery Team() const {
                return {{recipient, MakeIdentity<PerceptionListenerTypeId>(12),
                         TeamMessagePerceptionEvent{41, Source(3), Source(2), Math::WorldCoordinate64::FromMillimeters(5, 0, 0)}, 1},
                        PerceptionFilterDecision::Accept};
            }

            [[nodiscard]] PerceptionEventSource Capture(const std::span<const CommittedPerceptionDelivery> records) const {
                auto captured =
                    PerceptionEventSource::Capture(*scene, ai, host, Network::NetworkModeWorldKind::Standalone, registry, records);
                REQUIRE(captured.HasValue());
                return std::move(captured).Value();
            }
        };

        TEST_CASE("committed damage and contact enter only the declared recipient memory", "[unit][ai][perception][events]") {
            Fixture fixture;
            auto memory = fixture.Memory(fixture.recipient);
            auto otherMemory = fixture.Memory(fixture.other);
            std::array records{CommittedPerceptionDelivery{{fixture.recipient, MakeIdentity<PerceptionListenerTypeId>(10),
                                                            DamagePerceptionEvent{fixture.Source(2),
                                                                                  Math::WorldCoordinate64::FromMillimeters(7, 0, 0), 2},
                                                            1},
                                                           PerceptionFilterDecision::Accept},
                               CommittedPerceptionDelivery{{fixture.recipient, MakeIdentity<PerceptionListenerTypeId>(11),
                                                            ProximityPerceptionEvent{fixture.Source(3),
                                                                                     Math::WorldCoordinate64::FromMillimeters(9, 0, 0)},
                                                            1},
                                                           PerceptionFilterDecision::Accept}};
            auto source = fixture.Capture(records);
            REQUIRE(source.Deliver(0, memory).HasValue());
            REQUIRE(source.Deliver(1, memory).HasValue());
            CHECK(memory.StoredCount() == 2);
            CHECK(otherMemory.StoredCount() == 0);
            ExpectError(source.Deliver(0, otherMemory), AIErrors::PerceptionEventInvalid);
            CHECK(memory.SceneIncarnation() == 99);
            CHECK(memory.Agent().incarnation.Value() == 77);
            const auto snapshot = memory.Snapshot(1, PerceptionSceneLiveness(*fixture.scene)).Value();
            CHECK(snapshot.entries[0].lastKnownPosition.Millimeters()[0] == 7);
            CHECK(snapshot.entries[1].key.sense == SenseTypeIds::Touch);
            records[0].delivery.listener = MakeIdentity<PerceptionListenerTypeId>(11);
            auto wrongListener = fixture.Capture(records);
            ExpectError(wrongListener.Deliver(0, memory), AIErrors::PerceptionDependencyMissing);
        }

        TEST_CASE("team memory accepts only captured Gameplay deliveries and cannot forward itself", "[unit][ai][perception][events]") {
            Fixture fixture;
            auto memory = fixture.Memory(fixture.recipient);
            auto teammateMemory = fixture.Memory(fixture.other);
            auto record = fixture.Team();
            auto source = fixture.Capture(std::span{&record, 1});
            record.delivery.recipient = fixture.other;
            std::get<TeamMessagePerceptionEvent>(record.delivery.event).lastKnownPosition = {};
            REQUIRE(source.Deliver(0, memory).HasValue());
            CHECK(teammateMemory.StoredCount() == 0);
            const auto snapshot = memory.Snapshot(1, PerceptionSceneLiveness(*fixture.scene)).Value();
            REQUIRE(snapshot.count == 1);
            const auto &perceived = snapshot.entries[0];
            CHECK(perceived.provenance == fixture.Source(3));
            CHECK(perceived.lastKnownPosition.Millimeters()[0] == 5);
            PerceptionObservation forwarded{.key = perceived.key,
                                            .position = perceived.lastKnownPosition,
                                            .provenance = perceived.provenance};
            ExpectError(teammateMemory.Observe(forwarded, 1), AIErrors::PerceptionEventUnauthorized);
            forwarded.key.sense = SenseTypeIds::Sight;
            ExpectError(teammateMemory.Observe(forwarded, 1), AIErrors::PerceptionEventUnauthorized);
            forwarded.key = perceived.key;
            forwarded.key.stimulus = StimulusTypeIds::Damage;
            ExpectError(teammateMemory.Observe(forwarded, 1), AIErrors::PerceptionEventUnauthorized);
            ExpectError(source.Deliver(0, teammateMemory), AIErrors::PerceptionEventInvalid);
            source.Close();
            ExpectError(source.Deliver(0, memory), AIErrors::PerceptionEventUnauthorized);
        }

        TEST_CASE("host roles and their revocation control team knowledge admission", "[unit][ai][perception][events]") {
            Fixture client(Network::NetworkProjectRole::Client);
            const auto record = client.Team();
            ExpectError(PerceptionEventSource::Capture(*client.scene, client.ai, client.host, Network::NetworkModeWorldKind::Client,
                                                       client.registry, std::span{&record, 1}),
                        AIErrors::PerceptionEventUnauthorized);
            ExpectError(PerceptionEventSource::Capture(*client.scene, client.ai, client.host,
                                                       Network::NetworkModeWorldKind::AuthorityServer, client.registry,
                                                       std::span{&record, 1}),
                        AIErrors::PerceptionEventUnauthorized);
            Fixture server(Network::NetworkProjectRole::DedicatedServer);
            auto memory = server.Memory(server.recipient);
            const auto delivered = server.Team();
            auto captured =
                PerceptionEventSource::Capture(*server.scene, server.ai, server.host, Network::NetworkModeWorldKind::AuthorityServer,
                                               server.registry, std::span{&delivered, 1});
            REQUIRE(captured.HasValue());
            REQUIRE(captured.Value().Deliver(0, memory).HasValue());
            server.host.Shutdown();
            ExpectError(captured.Value().Deliver(0, memory), AIErrors::PerceptionEventUnauthorized);
            CHECK(memory.StoredCount() == 1);
        }

        TEST_CASE("current filters, payloads and agent grants gate publication", "[unit][ai][perception][events]") {
            Fixture fixture;
            auto memory = fixture.Memory(fixture.recipient);
            auto record = fixture.Team();
            record.filter = PerceptionFilterDecision::Reject;
            auto filtered = fixture.Capture(std::span{&record, 1});
            ExpectError(filtered.Deliver(0, memory), AIErrors::PerceptionEventFiltered);
            record.filter = PerceptionFilterDecision::Accept;
            std::get<TeamMessagePerceptionEvent>(record.delivery.event).deliveryId = 0;
            auto malformed = fixture.Capture(std::span{&record, 1});
            ExpectError(malformed.Deliver(0, memory), AIErrors::PerceptionEventInvalid);
            record = fixture.Team();
            auto revoked = fixture.Capture(std::span{&record, 1});
            REQUIRE(fixture.ai.DisableAtSafePoint(fixture.recipient).HasValue());
            ExpectError(revoked.Deliver(0, memory), AIErrors::PerceptionEventUnauthorized);
            CHECK(memory.StoredCount() == 0);
        }

        TEST_CASE("malformed damage, foreign provenance and stale recipient generations fail without publication",
                  "[unit][ai][perception][events]") {
            Fixture fixture;
            auto memory = fixture.Memory(fixture.recipient);
            CommittedPerceptionDelivery record{{fixture.recipient, MakeIdentity<PerceptionListenerTypeId>(10),
                                                DamagePerceptionEvent{fixture.Source(2), {}, 0}, 1},
                                               PerceptionFilterDecision::Accept};
            for (const double amount :
                 std::array{0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
                std::get<DamagePerceptionEvent>(record.delivery.event).amount = amount;
                auto source = fixture.Capture(std::span{&record, 1});
                ExpectError(source.Deliver(0, memory), AIErrors::PerceptionEventInvalid);
            }
            record = fixture.Team();
            std::get<TeamMessagePerceptionEvent>(record.delivery.event).subject.sceneIncarnation = 77;
            auto foreign = fixture.Capture(std::span{&record, 1});
            ExpectError(foreign.Deliver(0, memory), AIErrors::PerceptionEventInvalid);
            record = fixture.Team();
            ++record.delivery.recipient.slot.generation;
            auto replacementMemory = fixture.Memory(record.delivery.recipient);
            auto stale = fixture.Capture(std::span{&record, 1});
            ExpectError(stale.Deliver(0, replacementMemory), AIErrors::PerceptionEventStale);
            CHECK(memory.StoredCount() == 0);
            CHECK(replacementMemory.StoredCount() == 0);
        }

        TEST_CASE("bounded sensing windows reject invalid indices and expired AI publications", "[unit][ai][perception][events]") {
            Fixture fixture;
            auto memory = fixture.Memory(fixture.recipient);
            const auto record = fixture.Team();
            auto source = fixture.Capture(std::span{&record, 1});
            ExpectError(source.Deliver(1, memory), AIErrors::PerceptionEventInvalid);
            const std::vector oversized(PerceptionEventSource::MaximumDeliveries + 1, record);
            ExpectError(PerceptionEventSource::Capture(*fixture.scene, fixture.ai, fixture.host, Network::NetworkModeWorldKind::Standalone,
                                                       fixture.registry, oversized),
                        AIErrors::PerceptionEventInvalid);
            fixture.ai.BeginShutdown();
            ExpectError(source.Deliver(0, memory), AIErrors::PerceptionEventStale);
            CHECK(memory.StoredCount() == 0);
        }

        TEST_CASE("team sources and senders remain generation safe across destruction and slot reuse", "[unit][ai][perception][events]") {
            Fixture fixture;
            auto memory = fixture.Memory(fixture.recipient);
            const auto record = fixture.Team();
            auto source = fixture.Capture(std::span{&record, 1});
            REQUIRE(source.Deliver(0, memory).HasValue());
            const auto remembered = memory.Snapshot(1, PerceptionSceneLiveness(*fixture.scene)).Value().entries[0];
            Runtime::SceneCommandBuffer destroy;
            destroy.Destroy(fixture.Entity(3));
            REQUIRE(fixture.scene->Commit(destroy).HasValue());
            ExpectError(source.Deliver(0, memory), AIErrors::PerceptionEventStale);
            CHECK(remembered.provenance.generation == 1);
            Runtime::SceneCommandBuffer recycle;
            (void)recycle.Create({});
            const auto replaced = fixture.scene->Commit(recycle).Value().created[0].entity;
            CHECK(ProjectPerceptionSource(replaced).slot == remembered.provenance.slot);
            CHECK(ProjectPerceptionSource(replaced).generation != remembered.provenance.generation);
            ExpectError(source.Deliver(0, memory), AIErrors::PerceptionEventStale);
            Runtime::SceneCommandBuffer removeSubject;
            removeSubject.Destroy(fixture.Entity(2));
            REQUIRE(fixture.scene->Commit(removeSubject).HasValue());
            CHECK(memory.Snapshot(1, PerceptionSceneLiveness(*fixture.scene)).Value().count == 0);
            CHECK(remembered.key.source.generation == 1);
        }
    }  // namespace
}  // namespace Horo::AI
