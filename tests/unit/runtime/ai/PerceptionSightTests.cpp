#include "AllocationProbe.h"
#include "PerceptionSightTestSupport.h"

#include <limits>
#include <numbers>

namespace Horo::AI {
    namespace {
        using namespace SightTest;

        TEST_CASE("Sight rejects policy changes until explicit reset and bounds invalid geometry", "[unit][ai][sight]") {
            SightScene scene;
            const Physics::PhysicsQueryEventCapability unavailable;
            auto frame = Frame(scene.listener);
            PerceptionSightPolicy policy;
            PerceptionSight sight;
            PerceptionSightBudget budget{.simulationTick = 1};
            const auto first = sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget);
            REQUIRE(first.HasValue());
            CHECK(first.Value().observations[0].evidence == PerceptionSightEvidence::BudgetExhausted);
            budget.simulationTick = 2;
            ++policy.lossSamples;
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).HasError());
            sight.Reset();
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).HasValue());
            sight.Reset();
            policy.halfAngleRadians = std::numeric_limits<double>::quiet_NaN();
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).HasError());
            policy = {};
            frame.forward = {0, 0, -2};
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).HasError());
            frame = Frame(scene.listener);
            frame.physicsOrigin = Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::max(), 0, 0);
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).HasError());
        }

        TEST_CASE("Sight absent Physics binding fails with its exact typed error without revealing a target", "[unit][ai][sight]") {
            SightScene scene;
            const Physics::PhysicsQueryEventCapability absent;
            const auto frame = Frame(scene.listener);
            PerceptionSight sight;
            PerceptionSightBudget budget{.simulationTick = 1, .remainingRaycasts = 1};
            const auto failed = sight.Begin(scene.scene->View(), *scene.spatial, absent, frame, {}, budget);
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == Physics::PhysicsErrors::WorldInvalid.code.Value());
            CHECK(budget.remainingRaycasts == 1);
            CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
            const auto current = PerceptionSightTestAccess::Current(sight);
            REQUIRE(current.count == 1);
            CHECK(current.observations[0].evidence == PerceptionSightEvidence::Stale);
            CHECK_FALSE(current.observations[0].visible);
            const auto repeated = sight.Poll(scene.scene->View(), scene.revision, Publication(absent, frame));
            REQUIRE(repeated.HasError());
            CHECK(repeated.ErrorValue().code.Value() == Physics::PhysicsErrors::WorldInvalid.code.Value());
            const auto unboundSubmission = absent.SubmitBatch({});
            REQUIRE(unboundSubmission.HasError());
            CHECK(unboundSubmission.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityStale.code.Value());
        }

        TEST_CASE("Sight cone height radius and aim offset filter before costly queries", "[unit][ai][sight]") {
            SightScene scene;
            const Physics::PhysicsQueryEventCapability unavailable;
            const auto frame = Frame(scene.listener);
            PerceptionSightPolicy policy;
            PerceptionSight sight;
            PerceptionSightBudget budget{.simulationTick = 1};
            for (const auto &position :
                 {Math::WorldCoordinate64::FromMillimeters(0, 0, 10'000), Math::WorldCoordinate64::FromMillimeters(0, 2'000, -10'000)}) {
                scene.Publish(position);
                policy.maximumHeightMeters = 1;
                CHECK(
                    sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).Value().observations[0].evidence ==
                    PerceptionSightEvidence::OutsideCone);
                sight.Reset();
            }
            scene.Publish(Math::WorldCoordinate64::FromMillimeters(10'000, 0, 0));
            policy = {};
            policy.halfAngleRadians = std::numbers::pi / 2;
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).Value().observations[0].evidence ==
                  PerceptionSightEvidence::BudgetExhausted);
            sight.Reset();
            scene.Publish(Math::WorldCoordinate64::FromMillimeters(0, 0, -50'001));
            policy = {};
            policy.targetOffsetMeters = {0, 0, 1};
            const auto offset = sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget);
            REQUIRE(offset.HasValue());
            REQUIRE(offset.Value().count == 1);
            CHECK(offset.Value().observations[0].evidence == PerceptionSightEvidence::BudgetExhausted);
            sight.Reset();
            scene.Publish(Math::WorldCoordinate64::FromMillimeters(0, 0, -50'000));
            policy.targetOffsetMeters = {0, 0, -1};
            CHECK(sight.Begin(scene.scene->View(), *scene.spatial, unavailable, frame, policy, budget).Value().observations[0].evidence ==
                  PerceptionSightEvidence::OutsideCone);
        }

        TEST_CASE("Sight idle budget fallback and repeated polling allocate no sight storage", "[unit][ai][sight][allocation]") {
            SightScene scene;
            const Physics::PhysicsQueryEventCapability unavailable;
            const auto frame = Frame(scene.listener);
            PerceptionSight sight;
            PerceptionSightBudget budget{.simulationTick = 1};
            const auto view = scene.scene->View();
            std::size_t allocations{};
            bool succeeded{};
            {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                auto begun = sight.Begin(view, *scene.spatial, unavailable, frame, {}, budget);
                auto polled = sight.Poll(view, scene.revision, Publication(unavailable, frame));
                succeeded = begun.HasValue() && polled.HasValue();
                allocations = measurement.Snapshot().requests;
            }
            REQUIRE(succeeded);
            CHECK(allocations == 0);
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Sight rays preserve local separation near signed global coordinate limits", "[unit][ai][sight][canonical]") {
            SightScene scene;
            SightWorld physics;
            const auto global = std::numeric_limits<std::int64_t>::max();
            const auto listenerPosition = Math::WorldCoordinate64::FromMillimeters(global, 0, 0);
            scene.Publish(Math::WorldCoordinate64::FromMillimeters(global - 10'000, 0, 0), listenerPosition);
            auto frame = physics.CurrentFrame(scene.listener);
            frame.physicsOrigin = listenerPosition;
            frame.forward = {-1, 0, 0};
            PerceptionSight sight;
            PerceptionSightBudget budget{.simulationTick = physics.tick, .remainingRaycasts = 1};
            const auto begun = sight.Begin(scene.scene->View(), *scene.spatial, physics.capability, frame, {}, budget);
            REQUIRE(begun.HasValue());
            REQUIRE(begun.Value().pending);
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            const auto completed = sight.Poll(scene.scene->View(), scene.revision, Publication(physics.capability, frame));
            REQUIRE(completed.HasValue());
            CHECK(completed.Value().observations[0].evidence == PerceptionSightEvidence::Clear);
            CHECK(completed.Value().observations[0].visible);
        }

#endif
    }  // namespace
}  // namespace Horo::AI
