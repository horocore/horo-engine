#include "AiTestSupport.h"
#include "Horo/AI/AIScenePerceptionSource.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        TEST_CASE("Typed perception policies resolve directional affiliations and disclosure", "[unit][ai][perception][filter]") {
            PerceptionFilteringPolicy policy;
            PerceptionParticipantFilter listener{.team = {1}};
            PerceptionParticipantFilter source{.team = {1}};
            PerceptionListenerFilter filter{.affiliations = 1};
            CHECK(AllowsPerception(policy, listener, source, filter));
            source.team = {2};
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            filter.affiliations = 4;
            CHECK(AllowsPerception(policy, listener, source, filter));
            source.team = {};
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            filter.affiliations = 2;
            CHECK(AllowsPerception(policy, listener, source, filter));
            listener.team = {};
            filter.teams = PerceptionTeamFilter::Same;
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            listener.team = {1};
            source.team = {2};
            filter.teams = PerceptionTeamFilter::Any;
            policy.relationships[0] = {{1}, {2}, {PerceptionAffiliation::Custom, {99}}};
            policy.relationshipCount = 1;
            REQUIRE(ValidatePerceptionFiltering(policy).HasValue());
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            filter.custom[0] = {99};
            filter.customCount = 1;
            CHECK(AllowsPerception(policy, listener, source, filter));
            CHECK_FALSE(AllowsPerception(policy, source, listener, filter));
            source.visibility = PerceptionGameplayVisibility::TeamOnly;
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            source.visibility = PerceptionGameplayVisibility::Hidden;
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            source.visibility = PerceptionGameplayVisibility::Everyone;
            source.mask = 2;
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            source.mask = 1;
            listener.mask = 2;
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
            listener.mask = 1;
            filter.custom[0] = {100};
            CHECK_FALSE(AllowsPerception(policy, listener, source, filter));
        }

        TEST_CASE("Malformed bounded perception policy is rejected", "[unit][ai][perception][filter]") {
            PerceptionFilteringPolicy policy;
            SECTION("oversized") {
                policy.relationshipCount = 65;
            }
            SECTION("duplicate directional teams") {
                policy.relationshipCount = 2;
                policy.relationships[0] = {{1}, {2}, {PerceptionAffiliation::Friendly, {}}};
                policy.relationships[1] = policy.relationships[0];
            }
            SECTION("missing custom identity") {
                policy.relationshipCount = 1;
                policy.relationships[0].affiliation.kind = PerceptionAffiliation::Custom;
            }
            SECTION("unexpected custom identity") {
                policy.relationshipCount = 1;
                policy.relationships[0].affiliation.custom = {1};
            }
            SECTION("unknown enum") {
                policy.relationshipCount = 1;
                policy.relationships[0].affiliation.kind = PerceptionAffiliation::Count;
            }
            CHECK(ValidatePerceptionFiltering(policy).HasError());
            PerceptionListenerFilter filter;
            filter.customCount = 9;
            CHECK(ValidatePerceptionParticipant({}, filter).HasError());
            filter.customCount = 2;
            filter.custom[0] = {1};
            filter.custom[1] = {1};
            CHECK(ValidatePerceptionParticipant({}, filter).HasError());
            filter.customCount = 9;
            CHECK_FALSE(AllowsPerception({}, {}, {}, filter));
        }

        struct PublicationFixture final {
            AgentHandle agent{AiRuntimeIncarnation::Create(7).Value(), {1, 1}};
            std::unique_ptr<Runtime::RuntimeScene> scene;
            std::unique_ptr<BlackboardInstance> blackboard;
            std::shared_ptr<const BlackboardSchema> schema;
            AIPerceptionMemory memory{
                std::move(AIPerceptionMemory::Create(77, agent, {.listenerMaximumEntries = 32, .profileMaximumEntries = 32})).Value()};
            PerceptionSpatialBroadphase broadphase;
            PerceptionSpatialListener listener;
            PerceptionSpatialSource source;
            std::vector<PerceptionSpatialSource> additional;
            std::vector<PerceptionSpatialListener> additionalListeners;
            PerceptionPublication publication;
            PerceptionBlackboardProjection keys{MakeIdentity<BlackboardKeyId>(1), MakeIdentity<BlackboardKeyId>(2)};

            explicit PublicationFixture(const std::size_t sourceCount = 1) {
                InitializeParticipants(sourceCount);
                Publish(1);
                InitializeBlackboard();
                SeedMemory();
            }

            void InitializeParticipants(const std::size_t sourceCount) {
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
                for (std::size_t index = 1; index <= sourceCount + 1; ++index)
                    builder.Add({.object = Runtime::SceneObjectId{index}});
                builder.Add({.object = Runtime::SceneObjectId{1000}});
                const auto definition = std::move(builder).Build();
                REQUIRE(definition.HasValue());
                auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{77});
                REQUIRE(created.HasValue());
                scene = std::move(created).Value();
                listener.entity = *scene->View().Find(Runtime::SceneObjectId{1});
                listener.senses[0] = SenseTypeIds::Sight;
                listener.senseCount = 1;
                source.entity = *scene->View().Find(Runtime::SceneObjectId{2});
                source.layers = 1;
                source.senses[0] = SenseTypeIds::Sight;
                source.senseCount = 1;
                for (std::size_t index = 2; index <= sourceCount; ++index) {
                    auto next = source;
                    next.entity = *scene->View().Find(Runtime::SceneObjectId{index + 1});
                    additional.push_back(next);
                }
            }

            void InitializeBlackboard() {
                const std::array descriptors{BlackboardKeyDescriptor{.key = keys.sources,
                                                                     .kind = BlackboardValueKind::EntityReference,
                                                                     .cardinality = BlackboardValueCardinality::Collection,
                                                                     .maximumCollectionElements = 16,
                                                                     .defaultValue = BlackboardValue{BlackboardCollectionValue{
                                                                         .elementKind = BlackboardValueKind::EntityReference}}},
                                             BlackboardKeyDescriptor{.key = keys.positions,
                                                                     .kind = BlackboardValueKind::WorldCoordinate,
                                                                     .cardinality = BlackboardValueCardinality::Collection,
                                                                     .maximumCollectionElements = 16,
                                                                     .defaultValue = BlackboardValue{BlackboardCollectionValue{
                                                                         .elementKind = BlackboardValueKind::WorldCoordinate}}},
                                             BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(3),
                                                                     .kind = BlackboardValueKind::Boolean,
                                                                     .maximumCollectionElements = 1,
                                                                     .defaultValue = BlackboardValue{BlackboardScalarValue{false}}}};
                auto capturedSchema =
                    BlackboardSchema::Capture({MakeIdentity<BlackboardSchemaId>(1), 1, BlackboardUnknownValuePolicy::Reject, descriptors});
                REQUIRE(capturedSchema.HasValue());
                auto owned = std::make_shared<const BlackboardSchema>(std::move(capturedSchema).Value());
                auto instance = BlackboardInstance::Create({agent, owned->Identity(), 1, 1, 1}, owned);
                REQUIRE(instance.HasValue());
                schema = owned;
                blackboard = std::move(instance).Value();
            }

            void SeedMemory() {
                REQUIRE(memory
                            .Observe({.key = {MakeIdentity<PerceptionListenerTypeId>(1), SenseTypeIds::Sight, StimulusTypeIds::Visual,
                                              ProjectPerceptionSource(source.entity)},
                                      .position = Math::WorldCoordinate64::FromMillimeters(999, 0, 0)},
                                     1)
                            .HasValue());
                for (const auto &extra : additional)
                    REQUIRE(memory
                                .Observe({.key = {MakeIdentity<PerceptionListenerTypeId>(1), SenseTypeIds::Sight, StimulusTypeIds::Visual,
                                                  ProjectPerceptionSource(extra.entity)},
                                          .position = {}},
                                         1)
                                .HasValue());
            }

            void Publish(std::uint64_t revision) {
                std::vector<PerceptionSpatialSource> declared{source};
                declared.insert(declared.end(), additional.begin(), additional.end());
                std::vector<PerceptionSpatialListener> listeners{listener};
                listeners.insert(listeners.end(), additionalListeners.begin(), additionalListeners.end());
                REQUIRE(broadphase.Publish(scene->View(), revision, listeners, declared).HasValue());
            }

            [[nodiscard]] Result<PerceptionPublicationSnapshot> Sync(std::uint64_t tick = 1) {
                return publication.PublishAtBlackboardSync(*scene, broadphase, listener.entity, memory, *blackboard, keys, tick);
            }

            [[nodiscard]] BlackboardCollectionValue Read(BlackboardKeyId key) const {
                const auto snapshot = blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                const auto value = snapshot.Value().Read(key);
                REQUIRE(value.HasValue());
                REQUIRE(value.Value().has_value());
                return std::get<BlackboardCollectionValue>(*value.Value());
            }
        };

        TEST_CASE("Denied memory cannot reach blackboard or debug and policy replacement publishes once",
                  "[unit][ai][perception][filter]") {
            PublicationFixture fixture;
            const auto first = fixture.Sync();
            REQUIRE(first.HasValue());
            CHECK(first.Value().memory.count == 1);
            ExpectError(fixture.blackboard->CaptureCanonical(), AIErrors::BlackboardInstanceStale);
            CHECK(fixture.Read(fixture.keys.sources).count == 1);
            CHECK(std::get<Math::WorldCoordinate64>(fixture.Read(fixture.keys.positions).elements[0]) ==
                  Math::WorldCoordinate64::FromMillimeters(999, 0, 0));
            SECTION("hidden") {
                fixture.source.visibility = PerceptionGameplayVisibility::Hidden;
            }
            SECTION("listener mask") {
                fixture.listener.filtering.mask = 2;
            }
            SECTION("source mask") {
                fixture.source.listenerMask = 2;
            }
            SECTION("affiliation") {
                fixture.listener.admission.affiliations = 4;
            }
            SECTION("same team required") {
                fixture.listener.admission.teams = PerceptionTeamFilter::Same;
            }
            fixture.Publish(2);
            ExpectError(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard),
                        AIErrors::PerceptionSpatialStale);
            const auto denied = fixture.Sync();
            REQUIRE(denied.HasValue());
            CHECK(denied.Value().revision == first.Value().revision + 1);
            CHECK(denied.Value().memory.count == 0);
            CHECK_FALSE(denied.Value().memory.entries[0].key.source.IsValid());
            CHECK(fixture.Read(fixture.keys.sources).count == 0);
            CHECK(fixture.Read(fixture.keys.positions).count == 0);
            const auto again = fixture.Sync();
            REQUIRE(again.HasValue());
            CHECK(again.Value().revision == denied.Value().revision);
            CHECK(again.Value().blackboardRevision == denied.Value().blackboardRevision);
            CHECK(
                fixture.broadphase.Current()
                    ->Query(
                        {.listener = fixture.listener.entity, .sense = SenseTypeIds::Sight, .radiusMillimeters = 1000, .visibleLayers = 1})
                    .Value()
                    .count == 0);
        }

        TEST_CASE("17 through 32 admitted records fail without partial blackboard or debug publication", "[unit][ai][perception][filter]") {
            for (std::size_t count = 17; count <= 32; ++count) {
                INFO("memory entries: " << count);
                PublicationFixture fixture(count);
                ExpectError(fixture.Sync(), AIErrors::PerceptionDescriptorLimitExceeded);
                CHECK(fixture.Read(fixture.keys.sources).count == 0);
                CHECK(fixture.Read(fixture.keys.positions).count == 0);
                CHECK(fixture.blackboard->CaptureCanonical().HasValue());
                CHECK(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard).HasError());
                for (auto &extra : fixture.additional)
                    extra.visibility = PerceptionGameplayVisibility::Hidden;
                fixture.Publish(2);
                const auto first = fixture.Sync();
                REQUIRE(first.HasValue());
                REQUIRE(first.Value().memory.count == 1);
                for (auto &extra : fixture.additional)
                    extra.visibility = PerceptionGameplayVisibility::Everyone;
                fixture.Publish(3);
                ExpectError(fixture.Sync(), AIErrors::PerceptionDescriptorLimitExceeded);
                ExpectError(fixture.blackboard->Snapshot().Value().Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
                ExpectError(fixture.blackboard->Snapshot().Value().Read(fixture.keys.positions), AIErrors::BlackboardInstanceStale);
                CHECK(fixture.blackboard->Snapshot().Value().Revision().Value() == first.Value().blackboardRevision);
                CHECK(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard).HasError());
                for (auto &extra : fixture.additional)
                    extra.visibility = PerceptionGameplayVisibility::Hidden;
                fixture.Publish(4);
                const auto recovered = fixture.Sync();
                REQUIRE(recovered.HasValue());
                CHECK(recovered.Value().revision == first.Value().revision + 1);
            }
        }

        TEST_CASE("Accepted policy denial revokes ordinary reads even when 17 replacement facts overflow",
                  "[unit][ai][perception][filter]") {
            PublicationFixture fixture(18);
            for (auto &extra : fixture.additional)
                extra.visibility = PerceptionGameplayVisibility::Hidden;
            fixture.Publish(2);
            const auto allowed = fixture.Sync();
            REQUIRE(allowed.HasValue());
            const auto retained = fixture.blackboard->Snapshot().Value();
            REQUIRE(std::get<BlackboardCollectionValue>(*retained.Read(fixture.keys.sources).Value()).count == 1);
            fixture.source.visibility = PerceptionGameplayVisibility::Hidden;
            for (auto &extra : fixture.additional)
                extra.visibility = PerceptionGameplayVisibility::Everyone;
            fixture.Publish(3);
            ExpectError(fixture.Sync(), AIErrors::PerceptionDescriptorLimitExceeded);
            ExpectError(retained.Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            ExpectError(retained.Read(fixture.keys.positions), AIErrors::BlackboardInstanceStale);
            const auto current = fixture.blackboard->Snapshot().Value();
            ExpectError(current.Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            ExpectError(current.Read(fixture.keys.positions), AIErrors::BlackboardInstanceStale);
            REQUIRE(current.Read(MakeIdentity<BlackboardKeyId>(3)).HasValue());
            CHECK_FALSE(std::get<bool>(std::get<BlackboardScalarValue>(*current.Read(MakeIdentity<BlackboardKeyId>(3)).Value())));
            ExpectError(fixture.blackboard->CaptureCanonical(), AIErrors::BlackboardInstanceStale);
            CHECK(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard).HasError());
            for (auto &extra : fixture.additional)
                extra.visibility = PerceptionGameplayVisibility::Hidden;
            fixture.Publish(4);
            const auto recovered = fixture.Sync();
            REQUIRE(recovered.HasValue());
            CHECK(recovered.Value().revision == allowed.Value().revision + 1);
            CHECK(recovered.Value().blackboardRevision == allowed.Value().blackboardRevision + 1);
            CHECK(fixture.Read(fixture.keys.sources).count == 0);
        }

        TEST_CASE("Revoked policy data cannot escape a failed type projection or schema migration", "[unit][ai][perception][filter]") {
            PublicationFixture fixture;
            REQUIRE(fixture.Sync().HasValue());
            const auto retained = fixture.blackboard->Snapshot().Value();
            fixture.source.visibility = PerceptionGameplayVisibility::Hidden;
            fixture.Publish(2);
            PerceptionPublication wrongProjection;
            const PerceptionBlackboardProjection wrong{fixture.keys.sources, MakeIdentity<BlackboardKeyId>(3)};
            ExpectError(wrongProjection.PublishAtBlackboardSync(*fixture.scene, fixture.broadphase, fixture.listener.entity, fixture.memory,
                                                                *fixture.blackboard, wrong, 1),
                        AIErrors::BlackboardValueTypeMismatch);
            ExpectError(retained.Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            auto replacement = fixture.blackboard->Binding();
            ++replacement.instanceGeneration;
            ++replacement.schemaGeneration;
            REQUIRE(fixture.blackboard->ReplaceAtBlackboardSync(replacement, fixture.schema).HasValue());
            ExpectError(fixture.blackboard->Snapshot().Value().Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            ExpectError(fixture.blackboard->CaptureCanonical(), AIErrors::BlackboardInstanceStale);
            REQUIRE(fixture.blackboard->ResetAtBlackboardSync().HasValue());
            CHECK(fixture.blackboard->Snapshot().Value().Read(fixture.keys.sources).HasValue());
            CHECK(fixture.blackboard->CaptureCanonical().HasValue());
        }

        TEST_CASE("Selective derived defaults preserve canonical facts without restoring revoked permission",
                  "[unit][ai][perception][filter][canonical]") {
            PublicationFixture fixture;
            REQUIRE(fixture.Sync().HasValue());
            const auto unrelated = MakeIdentity<BlackboardKeyId>(3);
            auto createdUpdate = fixture.blackboard->BeginWriteBatch();
            REQUIRE(createdUpdate.HasValue());
            auto update = std::move(createdUpdate).Value();
            REQUIRE(update.Stage({unrelated, BlackboardValue{BlackboardScalarValue{true}}}).HasValue());
            REQUIRE(fixture.blackboard->CommitAtBlackboardSync(std::move(update)).HasValue());
            const auto retained = fixture.blackboard->Snapshot().Value();
            fixture.source.visibility = PerceptionGameplayVisibility::Hidden;
            fixture.Publish(2);
            const auto rejected = fixture.blackboard->CaptureCanonical();
            ExpectError(rejected, AIErrors::BlackboardInstanceStale);
            CHECK(rejected.ErrorValue().message.find("Reset only the derived keys") != std::string::npos);
            auto createdReset = fixture.blackboard->BeginWriteBatch();
            REQUIRE(createdReset.HasValue());
            auto reset = std::move(createdReset).Value();
            REQUIRE(reset
                        .Stage({fixture.keys.sources,
                                BlackboardValue{BlackboardCollectionValue{.elementKind = BlackboardValueKind::EntityReference}}})
                        .HasValue());
            REQUIRE(reset
                        .Stage({fixture.keys.positions,
                                BlackboardValue{BlackboardCollectionValue{.elementKind = BlackboardValueKind::WorldCoordinate}}})
                        .HasValue());
            REQUIRE(fixture.blackboard->CommitAtBlackboardSync(std::move(reset)).HasValue());
            CHECK(fixture.Read(fixture.keys.sources).count == 0);
            CHECK(fixture.Read(fixture.keys.positions).count == 0);
            ExpectError(retained.Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            const auto captured = fixture.blackboard->CaptureCanonical();
            REQUIRE(captured.HasValue());
            auto binding = fixture.blackboard->Binding();
            ++binding.instanceGeneration;
            const auto restored = BlackboardInstance::CreateFromCanonical(binding, fixture.schema, captured.Value());
            REQUIRE(restored.HasValue());
            const auto snapshot = restored.Value()->Snapshot().Value();
            CHECK(std::get<BlackboardCollectionValue>(*snapshot.Read(fixture.keys.sources).Value()).count == 0);
            CHECK(std::get<BlackboardCollectionValue>(*snapshot.Read(fixture.keys.positions).Value()).count == 0);
            CHECK(std::get<bool>(std::get<BlackboardScalarValue>(*snapshot.Read(unrelated).Value())));
            CHECK(restored.Value()->CaptureCanonical().HasValue());
        }

        TEST_CASE("One policy revokes all recipient leases and preserves unrelated blackboard values", "[unit][ai][perception][filter]") {
            PublicationFixture fixture;
            auto secondListener = fixture.listener;
            secondListener.entity = *fixture.scene->View().Find(Runtime::SceneObjectId{1000});
            fixture.additionalListeners.push_back(secondListener);
            fixture.Publish(2);
            REQUIRE(fixture.Sync().HasValue());
            const AgentHandle secondAgent{AiRuntimeIncarnation::Create(7).Value(), {2, 1}};
            auto secondBoard =
                std::move(BlackboardInstance::Create({secondAgent, fixture.schema->Identity(), 1, 1, 1}, fixture.schema)).Value();
            auto secondMemory = std::move(AIPerceptionMemory::Create(77, secondAgent)).Value();
            REQUIRE(secondMemory
                        .Observe({.key = {MakeIdentity<PerceptionListenerTypeId>(1), SenseTypeIds::Sight, StimulusTypeIds::Visual,
                                          ProjectPerceptionSource(fixture.source.entity)}},
                                 1)
                        .HasValue());
            PerceptionPublication secondPublisher;
            REQUIRE(secondPublisher
                        .PublishAtBlackboardSync(*fixture.scene, fixture.broadphase, secondListener.entity, secondMemory, *secondBoard,
                                                 fixture.keys, 1)
                        .HasValue());
            const auto first = fixture.blackboard->Snapshot().Value();
            const auto second = secondBoard->Snapshot().Value();
            fixture.source.visibility = PerceptionGameplayVisibility::Hidden;
            fixture.Publish(3);
            ExpectError(first.Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            ExpectError(second.Read(fixture.keys.sources), AIErrors::BlackboardInstanceStale);
            REQUIRE(first.Read(MakeIdentity<BlackboardKeyId>(3)).HasValue());
            REQUIRE(second.Read(MakeIdentity<BlackboardKeyId>(3)).HasValue());
        }

        TEST_CASE("Policy owner shutdown expires derived read leases held by snapshots", "[unit][ai][perception][filter]") {
            const auto retained = [] {
                PublicationFixture fixture;
                REQUIRE(fixture.Sync().HasValue());
                return fixture.blackboard->Snapshot().Value();
            }();
            ExpectError(retained.Read(MakeIdentity<BlackboardKeyId>(1)), AIErrors::BlackboardInstanceStale);
        }

        TEST_CASE("Invalid policy or projection cannot partially publish", "[unit][ai][perception][filter]") {
            PublicationFixture fixture;
            const auto first = fixture.Sync();
            REQUIRE(first.HasValue());
            const auto retained = fixture.blackboard->Snapshot().Value();
            fixture.listener.admission.customCount = 9;
            CHECK(fixture.broadphase.Publish(fixture.scene->View(), 2, std::span{&fixture.listener, 1}, std::span{&fixture.source, 1})
                      .HasError());
            CHECK(fixture.broadphase.Current()->Revision() == 1);
            REQUIRE(retained.Read(fixture.keys.sources).HasValue());
            CHECK(std::get<BlackboardCollectionValue>(*retained.Read(fixture.keys.sources).Value()).count == 1);
            CHECK(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard).Value().revision ==
                  first.Value().revision);
            fixture.keys.positions = MakeIdentity<BlackboardKeyId>(999);
            CHECK(fixture.Sync(2).HasError());
            CHECK(fixture.Read(fixture.keys.sources).count == 1);
            REQUIRE(fixture.blackboard->TeardownAtBlackboardSync().HasValue());
            CHECK(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard).HasError());
            fixture.publication.Reset();
            CHECK(fixture.publication.Current(fixture.scene->View(), fixture.broadphase, *fixture.blackboard).HasError());
        }
    }  // namespace
}  // namespace Horo::AI
