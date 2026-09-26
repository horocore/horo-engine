#include "AiTestSupport.h"
#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/PerceptionMemory.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace Horo::AI {
    namespace {
        using TestSupport::MakeIdentity;

        [[nodiscard]] AgentHandle Agent() {
            return {.incarnation = AiRuntimeIncarnation::Create(77).Value(), .slot = {.index = 1, .generation = 1}};
        }

        [[nodiscard]] PerceptionMemoryKey Key(const std::uint32_t slot, const std::uint32_t generation = 1, const std::uint64_t scene = 77,
                                              const std::uint64_t listener = 9) {
            return {.listener = MakeIdentity<PerceptionListenerTypeId>(listener),
                    .sense = MakeIdentity<SenseTypeId>(1),
                    .stimulus = MakeIdentity<StimulusTypeId>(1),
                    .source = {.sceneIncarnation = scene, .slot = slot, .generation = generation}};
        }

        [[nodiscard]] PerceptionObservation Observation(const PerceptionMemoryKey key, const std::int64_t x = 0) {
            return {.key = key, .position = Math::WorldCoordinate64::FromMillimeters(x, 0, 0), .velocity = {1, 2, 3}};
        }

        [[nodiscard]] bool AlwaysAlive(void *, const PerceptionSourceRef &) {
            return true;
        }

        [[nodiscard]] PerceptionSourceLiveness Live() {
            return {.isAlive = AlwaysAlive};
        }

        [[nodiscard]] AIPerceptionMemory Memory(PerceptionMemoryPolicy policy = {}) {
            policy.fixedStep = std::chrono::seconds{1};
            auto created = AIPerceptionMemory::Create(77, Agent(), policy);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        TEST_CASE("perception memory obeys listener and profile caps and evicts weakest oldest facts", "[unit][ai][perception][memory]") {
            const PerceptionMemoryPolicy policy{.listenerMaximumEntries = 3, .profileMaximumEntries = 2};
            auto memory = Memory(policy);
            REQUIRE(memory.Capacity() == 2);
            REQUIRE(memory.ListenerCapacity() == 2);
            REQUIRE(memory.Agent() == Agent());
            REQUIRE(memory.Observe(Observation(Key(1)), 0).HasValue());
            REQUIRE(memory.Observe(Observation(Key(2)), 1).HasValue());
            REQUIRE(memory.MarkLost(Key(1), 1).HasValue());
            REQUIRE(memory.Observe(Observation(Key(3)), 1).HasValue());
            REQUIRE(memory.StoredCount() == 2);
            REQUIRE(!memory.Find(Key(1), 1, Live()).Value().has_value());
            REQUIRE(memory.Find(Key(2), 1, Live()).Value().has_value());
            REQUIRE(memory.Find(Key(3), 1, Live()).Value().has_value());

            for (std::uint32_t source = 4; source < 100; ++source)
                REQUIRE(memory.Observe(Observation(Key(source)), 2).HasValue());
            REQUIRE(memory.StoredCount() == 2);
            REQUIRE(memory.Snapshot(2, Live()).Value().count == 2);
        }

        TEST_CASE("perception listener limits apply within one globally bounded agent memory", "[unit][ai][perception][memory]") {
            const PerceptionMemoryPolicy policy{.listenerMaximumEntries = 2, .profileMaximumEntries = 3};
            auto memory = Memory(policy);
            REQUIRE(memory.Observe(Observation(Key(1)), 0).HasValue());
            REQUIRE(memory.Observe(Observation(Key(2)), 1).HasValue());
            REQUIRE(memory.Observe(Observation(Key(3)), 2).HasValue());
            REQUIRE(!memory.Find(Key(1), 2, Live()).Value().has_value());
            REQUIRE(memory.StoredCount() == 2);
            REQUIRE(memory.Observe(Observation(Key(4, 1, 77, 10)), 2).HasValue());
            REQUIRE(memory.StoredCount() == 3);
            REQUIRE(memory.Observe(Observation(Key(5, 1, 77, 10)), 3).HasValue());
            REQUIRE(memory.StoredCount() == 3);
            REQUIRE(!memory.Find(Key(2), 3, Live()).Value().has_value());
            REQUIRE(memory.Find(Key(3), 3, Live()).Value().has_value());
            REQUIRE(memory.Find(Key(4, 1, 77, 10), 3, Live()).Value().has_value());
            REQUIRE(memory.Find(Key(5, 1, 77, 10), 3, Live()).Value().has_value());
        }

        TEST_CASE("perception memory refreshes last known facts and forgets loss or maximum age", "[unit][ai][perception][memory]") {
            auto memory = Memory();
            REQUIRE(memory.Observe(Observation(Key(1), 100), 0).HasValue());
            REQUIRE(memory.MarkLost(Key(1), 2).HasValue());
            auto lost = memory.Find(Key(1), 3, Live());
            REQUIRE(lost.HasValue());
            REQUIRE(lost.Value().has_value());
            CHECK_FALSE(lost.Value()->isCurrentlySensed);
            CHECK(lost.Value()->ageSeconds == 3);
            CHECK(std::abs(lost.Value()->confidence - 0.7) < 1e-12);
            CHECK(lost.Value()->lastKnownPosition.Millimeters()[0] == 100);
            const std::array<double, 3> expectedVelocity{1, 2, 3};
            CHECK(lost.Value()->lastKnownVelocity == expectedVelocity);

            REQUIRE(memory.Observe(Observation(Key(1), 250), 4).HasValue());
            const auto refreshed = memory.Find(Key(1), 4, Live()).Value();
            REQUIRE(refreshed.has_value());
            CHECK(refreshed->firstSensedTick == 0);
            CHECK(refreshed->lastSensedTick == 4);
            CHECK(refreshed->ageSeconds == 0);
            CHECK(refreshed->confidence == 1);
            CHECK(refreshed->isCurrentlySensed);
            CHECK(refreshed->lastKnownPosition.Millimeters()[0] == 250);

            REQUIRE(memory.MarkLost(Key(1), 4).HasValue());
            REQUIRE(memory.Find(Key(1), 13, Live()).Value().has_value());
            REQUIRE(!memory.Find(Key(1), 14, Live()).Value().has_value());
            REQUIRE(memory.StoredCount() == 0);
            REQUIRE(memory.Observe(Observation(Key(1), 300), 15).HasValue());
            CHECK(memory.Find(Key(1), 15, Live()).Value()->firstSensedTick == 15);
        }

        TEST_CASE("perception memory forgets at the confidence threshold and expires unrefreshed visibility",
                  "[unit][ai][perception][memory]") {
            const PerceptionMemoryPolicy policy{.memoryDurationSeconds = 10, .decayPerSecond = 0.5, .forgetThreshold = 0.25};
            auto memory = Memory(policy);
            REQUIRE(memory.Observe(Observation(Key(1)), 0).HasValue());
            REQUIRE(memory.MarkLost(Key(1), 0).HasValue());
            REQUIRE(memory.Find(Key(1), 1, Live()).Value().has_value());
            REQUIRE(!memory.Find(Key(1), 2, Live()).Value().has_value());
            REQUIRE(memory.StoredCount() == 0);

            REQUIRE(memory.Observe(Observation(Key(2)), 2).HasValue());
            REQUIRE(memory.Find(Key(2), 11, Live()).Value()->isCurrentlySensed);
            REQUIRE(!memory.Find(Key(2), 12, Live()).Value().has_value());
            REQUIRE(memory.StoredCount() == 0);
        }

        TEST_CASE("perception memory pauses with simulation time and replays after explicit reset", "[unit][ai][perception][memory]") {
            auto memory = Memory();
            REQUIRE(memory.Observe(Observation(Key(1)), 0).HasValue());
            REQUIRE(memory.MarkLost(Key(1), 0).HasValue());
            const auto first = memory.Find(Key(1), 2, Live()).Value();
            REQUIRE(first.has_value());
            const auto paused = memory.Find(Key(1), 2, Live()).Value();
            REQUIRE(paused.has_value());
            CHECK(paused->ageSeconds == first->ageSeconds);
            CHECK(paused->confidence == first->confidence);
            CHECK(paused->lastSensedTick == first->lastSensedTick);
            REQUIRE(memory.AdvanceTo(1).HasError());
            CHECK(memory.Find(Key(1), 2, Live()).Value()->ageSeconds == 2);

            REQUIRE(memory.ResetAt(0).HasValue());
            CHECK(memory.StoredCount() == 0);
            REQUIRE(memory.Observe(Observation(Key(1)), 0).HasValue());
            REQUIRE(memory.MarkLost(Key(1), 0).HasValue());
            const auto replayed = memory.Find(Key(1), 2, Live()).Value();
            REQUIRE(replayed.has_value());
            CHECK(replayed->ageSeconds == first->ageSeconds);
            CHECK(replayed->confidence == first->confidence);
            CHECK(replayed->isCurrentlySensed == first->isCurrentlySensed);
        }

        TEST_CASE("perception memory validates hostile policy source time and liveness", "[unit][ai][perception][memory]") {
            REQUIRE(AIPerceptionMemory::Create(0, Agent()).HasError());
            REQUIRE(AIPerceptionMemory::Create(77, {}, {}).HasError());
            REQUIRE(AIPerceptionMemory::Create(77, Agent(), {.listenerMaximumEntries = 33}).HasError());
            REQUIRE(AIPerceptionMemory::Create(77, Agent(), {.decayPerSecond = -1}).HasError());
            REQUIRE(AIPerceptionMemory::Create(77, Agent(), {.fixedStep = std::chrono::nanoseconds{0}}).HasError());
            auto memory = Memory();
            REQUIRE(memory.Observe(Observation(Key(1, 1, 88)), 0).HasError());
            auto missingListener = Key(1);
            missingListener.listener = {};
            REQUIRE(memory.Observe(Observation(missingListener), 0).HasError());
            REQUIRE(memory.Observe(Observation(Key(1, 0)), 0).HasError());
            auto invalidVelocity = Observation(Key(1));
            invalidVelocity.velocity[0] = std::numeric_limits<double>::quiet_NaN();
            REQUIRE(memory.Observe(invalidVelocity, 0).HasError());
            REQUIRE(memory.Observe(Observation(Key(1)), 1).HasValue());
            REQUIRE(memory.Observe(Observation(Key(1)), 0).HasError());
            REQUIRE(memory.ResetAt(0).HasValue());
            REQUIRE(memory.Find(Key(1), 0, {}).HasError());
            REQUIRE(memory.Snapshot(0, {}).HasError());
            REQUIRE(memory.Observe(Observation(Key(2)), 0).HasValue());
            REQUIRE(memory.ForgetSource(Key(2).source).HasValue());
            REQUIRE(memory.StoredCount() == 0);
            REQUIRE(memory.Observe(Observation(Key(3)), 0).HasValue());
            REQUIRE(memory.AdvanceTo(std::numeric_limits<std::uint64_t>::max()).HasValue());
            REQUIRE(memory.StoredCount() == 0);
        }

        TEST_CASE("scene source adapter removes destroyed generations before perception reads", "[unit][ai][perception][memory][scene]") {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            builder.Add(entity);
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{77});
            REQUIRE(created.HasValue());
            auto scene = std::move(created).Value();
            const auto owner = scene->View().Find(Runtime::SceneObjectId{1});
            REQUIRE(owner.has_value());
            auto memory = Memory();
            auto key = Key(owner->entity.index, owner->entity.generation);
            key.source = ProjectPerceptionSource(*owner);
            REQUIRE(memory.Observe(Observation(key), 0).HasValue());
            REQUIRE(memory.Find(key, 0, PerceptionSceneLiveness(*scene)).Value().has_value());

            Runtime::SceneCommandBuffer destroy;
            destroy.Destroy(*owner);
            REQUIRE(scene->Commit(std::move(destroy)).HasValue());
            REQUIRE(!memory.Find(key, 1, PerceptionSceneLiveness(*scene)).Value().has_value());
            REQUIRE(memory.StoredCount() == 0);

            Runtime::SceneCommandBuffer reuse;
            const auto token = reuse.Create(Runtime::RuntimeEntityCreateInfo{});
            auto committed = scene->Commit(std::move(reuse));
            REQUIRE(committed.HasValue());
            REQUIRE(committed.Value().created.size() == 1);
            CHECK(committed.Value().created[0].deferred == token);
            const auto replacement = committed.Value().created[0].entity;
            CHECK(replacement.entity.index == owner->entity.index);
            CHECK(replacement.entity.generation != owner->entity.generation);
            REQUIRE(!PerceptionSceneLiveness(*scene).isAlive(scene.get(), key.source));
            REQUIRE(memory.Observe(Observation(key), 2).HasValue());
            REQUIRE(!memory.Find(key, 2, PerceptionSceneLiveness(*scene)).Value().has_value());
        }
    }  // namespace
}  // namespace Horo::AI
