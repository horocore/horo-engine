#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsEventProjection.h"
#include "PhysicsReferenceSceneCorpus.h"
#include "PhysicsTestUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace {
        constexpr std::array<std::uint8_t, 16> LayerBytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        constexpr std::array<std::uint8_t, 16> ProfileBytes{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
        constexpr std::array<std::uint8_t, 16> ChannelBytes{3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};

        [[nodiscard]] PhysicsWorldId ReferenceWorld() {
            return PhysicsWorldId::Create(17).Value();
        }

        [[nodiscard]] const Test::PhysicsReferenceSceneExpectation &Expectation(const Test::PhysicsReferenceSceneId scene) {
            return Test::PhysicsReferenceSceneCorpus[static_cast<std::size_t>(scene)];
        }

        [[nodiscard]] PhysicsEventEndpoint EventEndpoint(const std::uint32_t index, const std::uint32_t generation = 1) {
            return {.body = {ReferenceWorld(), {index, generation}},
                    .shape = {ReferenceWorld(), {index, generation}},
                    .subshape = PhysicsShapeSubresourceId::FromValue(index + 1),
                    .layer = CollisionLayerId::FromBytes(LayerBytes),
                    .profile = CollisionProfileId::FromBytes(ProfileBytes),
                    .filterSchemaGeneration = 1};
        }

        [[nodiscard]] PhysicsContactObservation EventObservation(const std::uint64_t tick, const std::uint32_t first,
                                                                 const std::uint32_t second, const bool sensor) {
            return {.simulationTick = tick,
                    .first = EventEndpoint(first),
                    .second = EventEndpoint(second),
                    .contact = {.position = {static_cast<float>(first), 0.5F, static_cast<float>(second)},
                                .normal = {0.0F, 1.0F, 0.0F},
                                .penetrationDepthMeters = 0.02F,
                                .normalImpulseNewtonSeconds = 0.0F},
                    .sensor = sensor};
        }

        [[nodiscard]] std::vector<PhysicsEventRecord> RunLifecycleReferenceFixture(const bool sensor) {
            Detail::PhysicsEventProjection projection{4, 4, PhysicsEventOverflowPolicy::DropNewest};
            std::vector<PhysicsEventRecord> records;
            records.reserve(3);
            for (std::uint64_t tick = 1; tick <= 3; ++tick) {
                projection.BeginTick(tick);
                if (tick < 3)
                    REQUIRE(projection.TryCapture(EventObservation(tick, 0, 1, sensor)));
                REQUIRE(projection.CompleteTick(tick).HasValue());
                for (const auto &record : projection.PublishedEvents())
                    records.push_back(record);
            }
            return records;
        }

        [[nodiscard]] Test::PhysicsReferenceObservation ObservationFromEvents(const Test::PhysicsReferenceSceneId scene,
                                                                              const PhysicsCapability requiredCapability,
                                                                              const std::vector<PhysicsEventRecord> &records) {
            Test::PhysicsReferenceObservation observation{.corpusVersion = Test::PhysicsReferenceSceneCorpusVersion,
                                                          .scene = scene,
                                                          .status = Test::PhysicsReferenceSceneStatus::Supported,
                                                          .requiredCapability = requiredCapability,
                                                          .eventCount = static_cast<std::uint32_t>(records.size())};
            REQUIRE(records.size() <= Test::MaximumPhysicsReferenceEvents);
            for (std::size_t index = 0; index < records.size(); ++index) {
                observation.eventKinds[index] = records[index].kind;
                observation.eventTicks[index] = records[index].simulationTick;
            }
            return observation;
        }

        [[nodiscard]] PhysicsQueryHit QueryHit(const std::uint32_t slot, const std::uint32_t generation, const float distance) {
            return {.body = {ReferenceWorld(), {slot, generation}},
                    .shape = {ReferenceWorld(), {slot, generation}},
                    .subshape = PhysicsShapeSubresourceId::FromValue(slot + 1),
                    .layer = CollisionLayerId::FromBytes(LayerBytes),
                    .profile = CollisionProfileId::FromBytes(ProfileBytes),
                    .channel = PhysicsQueryChannelId::FromBytes(ChannelBytes),
                    .filterSchemaGeneration = 1,
                    .response = PhysicsQueryResponse::Block,
                    .position = {0.0F, 0.0F, -distance},
                    .normal = Math::Vec3{0.0F, 1.0F, 0.0F},
                    .distanceMeters = distance};
        }

        [[nodiscard]] Test::PhysicsReferenceObservation ObservationFromQueryHits(const Test::PhysicsReferenceSceneId scene,
                                                                                 const PhysicsCapability requiredCapability,
                                                                                 const std::span<const PhysicsQueryHit> hits) {
            Test::PhysicsReferenceObservation observation{.corpusVersion = Test::PhysicsReferenceSceneCorpusVersion,
                                                          .scene = scene,
                                                          .status = Test::PhysicsReferenceSceneStatus::Supported,
                                                          .requiredCapability = requiredCapability,
                                                          .queryHitCount = static_cast<std::uint32_t>(hits.size())};
            REQUIRE(hits.size() <= Test::MaximumPhysicsReferenceQueryHits);
            for (std::size_t index = 0; index < hits.size(); ++index) {
                observation.queryBodySlots[index] = hits[index].body.slot.index;
                observation.queryBodyGenerations[index] = hits[index].body.slot.generation;
                observation.queryDistanceBits[index] = std::bit_cast<std::uint32_t>(hits[index].distanceMeters);
            }
            return observation;
        }

#if HORO_TEST_PHYSICS_NATIVE
        [[nodiscard]] PhysicsQueryFixtureDescriptor NativeFixture(const Math::Vec3 translation) {
            return {.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                    .pose = {.translation = translation, .rotation = Math::Quaternion::Identity()},
                    .layer = CollisionLayerId::FromBytes(LayerBytes),
                    .profile = CollisionProfileId::FromBytes(ProfileBytes),
                    .channel = PhysicsQueryChannelId::FromBytes(ChannelBytes),
                    .response = PhysicsQueryFixtureResponse::Block,
                    .subshape = PhysicsShapeSubresourceId::FromValue(11)};
        }
#endif
    }  // namespace

    TEST_CASE("Physics reference corpus is versioned and hash locked", "[physics][reference][headless]") {
        REQUIRE(Test::PhysicsReferenceSceneCorpusView().size() == Test::PhysicsReferenceSceneCount);
        for (std::size_t index = 0; index < Test::PhysicsReferenceSceneCorpusView().size(); ++index) {
            const auto &expectation = Test::PhysicsReferenceSceneCorpusView()[index];
            INFO("reference scene: " << std::string(expectation.name));
            REQUIRE(expectation.observation.scene == static_cast<Test::PhysicsReferenceSceneId>(index));
            REQUIRE(Test::IsValidPhysicsReferenceObservation(expectation.observation));
            REQUIRE(expectation.expectedHash != 0);
            REQUIRE(Test::PhysicsReferenceObservationHash(expectation.observation) == expectation.expectedHash);
            for (std::size_t other = 0; other < index; ++other)
                REQUIRE(expectation.name != Test::PhysicsReferenceSceneCorpusView()[other].name);
        }
    }

    TEST_CASE("Physics contact reference scene reproduces exact lifecycle observation", "[physics][reference][events][headless]") {
        const auto &expected = Expectation(Test::PhysicsReferenceSceneId::ContactLifecycle);
        const auto actual = ObservationFromEvents(Test::PhysicsReferenceSceneId::ContactLifecycle, PhysicsCapability::WorldCreation,
                                                  RunLifecycleReferenceFixture(false));
        INFO("reference scene: " << std::string(expected.name));
        REQUIRE(actual == expected.observation);
        REQUIRE(Test::PhysicsReferenceObservationHash(actual) == expected.expectedHash);
    }

    TEST_CASE("Physics trigger reference scene reproduces exact lifecycle observation", "[physics][reference][events][headless]") {
        const auto &expected = Expectation(Test::PhysicsReferenceSceneId::TriggerLifecycle);
        const auto actual = ObservationFromEvents(Test::PhysicsReferenceSceneId::TriggerLifecycle, PhysicsCapability::WorldCreation,
                                                  RunLifecycleReferenceFixture(true));
        INFO("reference scene: " << std::string(expected.name));
        REQUIRE(actual == expected.observation);
        REQUIRE(Test::PhysicsReferenceObservationHash(actual) == expected.expectedHash);
    }

    TEST_CASE("Physics query reference scene reproduces exact backend-neutral ordering", "[physics][reference][query][headless]") {
        std::array hits{QueryHit(1, 2, 9.5F), QueryHit(0, 1, 4.5F)};
        std::ranges::sort(hits, PhysicsQueryHitLess);
        const auto &expected = Expectation(Test::PhysicsReferenceSceneId::QueryOrdering);
        const auto actual =
            ObservationFromQueryHits(Test::PhysicsReferenceSceneId::QueryOrdering, PhysicsCapability::ImmediateQueries, hits);
        INFO("reference scene: " << std::string(expected.name));
        REQUIRE(actual == expected.observation);
        REQUIRE(Test::PhysicsReferenceObservationHash(actual) == expected.expectedHash);
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Canonical Physics query reference scene reproduces exact fixture ordering", "[physics][reference][query][native]") {
        auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings());
        REQUIRE(world.HasValue());
        auto candidate = std::move(world).Value();
        const auto identity = PhysicsWorldId::Create(704).Value();
        REQUIRE(candidate->Activate(identity).HasValue());
        const auto first = candidate->CreateQueryFixture(NativeFixture({0.0F, 0.0F, -5.0F}));
        REQUIRE(first.HasValue());
        const auto second = candidate->CreateQueryFixture(NativeFixture({0.0F, 0.0F, -10.0F}));
        REQUIRE(second.HasValue());
        REQUIRE(
            candidate->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                .HasValue());

        PhysicsQueryDescriptor descriptor{.world = identity,
                                          .sceneGeneration = 1,
                                          .geometry = PhysicsRayQuery{{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, 20.0F},
                                          .filter = {.channel = PhysicsQueryChannelId::FromBytes(ChannelBytes)},
                                          .collection = PhysicsQueryCollection::All,
                                          .ordering = PhysicsQueryOrdering::ClosestFirst,
                                          .maximumHitCount = 2};
        std::array<PhysicsQueryHit, 2> hits{};
        const auto result = candidate->Query(descriptor, hits);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().hitCount == 2);
        REQUIRE_FALSE(result.Value().truncated);

        const auto &expected = Expectation(Test::PhysicsReferenceSceneId::QueryOrdering);
        const auto actual =
            ObservationFromQueryHits(Test::PhysicsReferenceSceneId::QueryOrdering, PhysicsCapability::ImmediateQueries, hits);
        INFO("reference scene: " << std::string(expected.name));
        REQUIRE(actual == expected.observation);
        REQUIRE(Test::PhysicsReferenceObservationHash(actual) == expected.expectedHash);
        REQUIRE(candidate->DestroyQueryFixture(first.Value()).HasValue());
        REQUIRE(candidate->DestroyQueryFixture(second.Value()).HasValue());
    }
#endif

    TEST_CASE("Physics reference unsupported scenes fail closed on capability evidence", "[physics][reference][capability][headless]") {
        auto nullRuntime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value();
        for (std::size_t index = static_cast<std::size_t>(Test::PhysicsReferenceSceneId::CcdTunnelling);
             index <= static_cast<std::size_t>(Test::PhysicsReferenceSceneId::FixedJoint); ++index) {
            const auto &expectation = Test::PhysicsReferenceSceneCorpusView()[index];
            INFO("reference scene: " << std::string(expectation.name));
            REQUIRE(expectation.observation.status == Test::PhysicsReferenceSceneStatus::Unsupported);
            REQUIRE(expectation.observation.unsupportedReason == Test::PhysicsReferenceUnsupportedReason::RequiredCapabilityUnsupported);
            REQUIRE(nullRuntime->Capability(expectation.observation.requiredCapability) == PhysicsCapabilitySupport::Unsupported);
        }

#if HORO_TEST_PHYSICS_NATIVE
        auto canonicalRuntime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        for (std::size_t index = static_cast<std::size_t>(Test::PhysicsReferenceSceneId::CcdTunnelling);
             index <= static_cast<std::size_t>(Test::PhysicsReferenceSceneId::FixedJoint); ++index) {
            const auto &expectation = Test::PhysicsReferenceSceneCorpusView()[index];
            REQUIRE(canonicalRuntime->Capability(expectation.observation.requiredCapability) != PhysicsCapabilitySupport::Available);
        }
#endif
    }

    TEST_CASE("Physics reference event fixture keeps the prior publication on bounded overflow", "[physics][reference][events][headless]") {
        Detail::PhysicsEventProjection projection{1, 2, PhysicsEventOverflowPolicy::FailTick};
        projection.BeginTick(1);
        REQUIRE(projection.TryCapture(EventObservation(1, 0, 1, false)));
        REQUIRE(projection.CompleteTick(1).HasValue());
        REQUIRE(projection.PublishedTick() == 1);
        REQUIRE(projection.PublishedEvents().size() == 1);

        projection.BeginTick(2);
        REQUIRE(projection.TryCapture(EventObservation(2, 0, 1, false)));
        REQUIRE(projection.TryCapture(EventObservation(2, 2, 3, false)));
        const auto overflow = projection.CompleteTick(2);
        REQUIRE(overflow.HasError());
        REQUIRE(overflow.ErrorValue().code.Value() == PhysicsErrors::CapacityExceeded.code.Value());
        REQUIRE(projection.PublishedTick() == 1);
        REQUIRE(projection.PublishedEvents().size() == 1);
        REQUIRE(projection.DroppedRecordCount() == 1);

        const std::array diagnosticContext{
            PhysicsDiagnosticContextEntry{.key = PhysicsDiagnosticContextKey::World, .value = ReferenceWorld()},
            PhysicsDiagnosticContextEntry{.key = PhysicsDiagnosticContextKey::SceneGeneration, .value = std::uint64_t{1}},
            PhysicsDiagnosticContextEntry{.key = PhysicsDiagnosticContextKey::SimulationTick, .value = std::uint64_t{2}},
            PhysicsDiagnosticContextEntry{.key = PhysicsDiagnosticContextKey::Capacity, .value = std::uint64_t{1}},
        };
        const auto diagnostic = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Event, overflow.ErrorValue(), diagnosticContext);
        REQUIRE(diagnostic.HasValue());
        REQUIRE(diagnostic.Value().contextCount == diagnosticContext.size());
        REQUIRE(diagnostic.Value().context[0].key == PhysicsDiagnosticContextKey::World);
        REQUIRE(diagnostic.Value().context[1].key == PhysicsDiagnosticContextKey::SceneGeneration);
        REQUIRE(diagnostic.Value().context[2].key == PhysicsDiagnosticContextKey::SimulationTick);
        REQUIRE(diagnostic.Value().context[3].key == PhysicsDiagnosticContextKey::Capacity);
        projection.AbortTick();
    }
}  // namespace Horo::Physics
