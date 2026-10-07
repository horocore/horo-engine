#include "PerceptionSightTestSupport.h"

namespace Horo::AI {
    namespace {
        using namespace SightTest;

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Sight batches canonical LOS and distinguishes overlap from blocking occlusion", "[unit][ai][sight][canonical]") {
            SightScene scene;
            SightWorld physics;
            (void)physics.Box(-2, Physics::PhysicsQueryFixtureResponse::Overlap);
            (void)physics.Box(-5);
            physics.Advance();
            PerceptionSight sight;
            auto result = physics.Sample(sight, scene);
            REQUIRE(result.count == 1);
            CHECK(result.observations[0].evidence == PerceptionSightEvidence::Occluded);
            CHECK_FALSE(result.observations[0].visible);
        }

        TEST_CASE("Sight gain loss hysteresis ignores budget gaps and repeated reads", "[unit][ai][sight][canonical]") {
            SightScene scene;
            SightWorld physics;
            PerceptionSight sight;
            PerceptionSightPolicy policy{.gainSamples = 2, .lossSamples = 2};
            CHECK_FALSE(physics.Sample(sight, scene, policy).observations[0].visible);
            physics.Advance();
            CHECK(physics.Sample(sight, scene, policy).observations[0].visible);
            physics.Advance();
            const auto gap = physics.Sample(sight, scene, policy, 0);
            CHECK(gap.observations[0].evidence == PerceptionSightEvidence::BudgetExhausted);
            CHECK(gap.observations[0].visible);
            (void)physics.Box(-5);
            physics.Advance();
            CHECK(physics.Sample(sight, scene, policy).observations[0].visible);
            const auto frame = physics.CurrentFrame(scene.listener);
            CHECK(sight.Poll(scene.scene->View(), scene.revision, Publication(physics.capability, frame)).Value().observations[0].visible);
            physics.Advance();
            CHECK_FALSE(physics.Sample(sight, scene, policy).observations[0].visible);
        }

        TEST_CASE("Sight unknown evidence breaks consecutive gain and loss confirmations", "[unit][ai][sight][canonical][fault]") {
            SightScene scene;
            SightWorld physics;
            PerceptionSight sight;
            const PerceptionSightPolicy policy{.gainSamples = 2, .lossSamples = 2};
            CHECK_FALSE(physics.Sample(sight, scene, policy).observations[0].visible);
            physics.Advance();
            SECTION("budget interrupts gain") {
                CHECK_FALSE(physics.Sample(sight, scene, policy, 0).observations[0].visible);
            }
            SECTION("truncation interrupts gain") {
                (void)physics.Queue(sight, scene, policy, 1);
                REQUIRE(physics.world->ProcessQueryBatch().HasValue());
                auto unknown = PerceptionSightTestAccess::Completion(sight);
                unknown.entries[0].completion.result.truncated = true;
                const auto result = PerceptionSightTestAccess::Consume(sight, unknown);
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value().observations[0].visible);
            }
            physics.Advance();
            CHECK_FALSE(physics.Sample(sight, scene, policy).observations[0].visible);
            physics.Advance();
            REQUIRE(physics.Sample(sight, scene, policy).observations[0].visible);
            (void)physics.Box(-5);
            physics.Advance();
            REQUIRE(physics.Sample(sight, scene, policy).observations[0].visible);
            physics.Advance();
            CHECK(physics.Sample(sight, scene, policy, 0).observations[0].visible);
            physics.Advance();
            CHECK(physics.Sample(sight, scene, policy).observations[0].visible);
            physics.Advance();
            CHECK_FALSE(physics.Sample(sight, scene, policy).observations[0].visible);
        }

        TEST_CASE("Sight validates the whole batch before committing any clear visibility", "[unit][ai][sight][canonical][fault]") {
            SightScene scene;
            scene.Publish(Math::WorldCoordinate64::FromMillimeters(0, 0, -10'000), {}, true);
            SightWorld physics;
            PerceptionSight sight;
            (void)physics.Queue(sight, scene);
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            auto completion = PerceptionSightTestAccess::Completion(sight);
            REQUIRE(completion.entries.size() == 2);
            REQUIRE(completion.entries[0].hits.empty());
            ++completion.entries[1].completion.publicationRevision;
            CHECK(PerceptionSightTestAccess::Consume(sight, completion).HasError());
            CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
            const auto failed = PerceptionSightTestAccess::Current(sight);
            for (std::size_t index = 0; index < failed.count; ++index) {
                CHECK_FALSE(failed.observations[index].visible);
                CHECK(failed.observations[index].evidence == PerceptionSightEvidence::Stale);
            }
        }

        TEST_CASE("Sight shared budget bounds rays and does not equate rejected admission with occlusion", "[unit][ai][sight][canonical]") {
            SightScene scene;
            scene.Publish(Math::WorldCoordinate64::FromMillimeters(0, 0, -10'000), {}, true);
            SightWorld physics;
            PerceptionSight sight;
            PerceptionSight competing;
            const auto frame = physics.CurrentFrame(scene.listener);
            PerceptionSightBudget budget{.simulationTick = physics.tick, .remainingRaycasts = 1};
            const auto first = sight.Begin(scene.scene->View(), *scene.spatial, physics.capability, frame, {}, budget);
            REQUIRE(first.HasValue());
            CHECK(budget.remainingRaycasts == 0);
            CHECK(first.Value().observations[1].evidence == PerceptionSightEvidence::BudgetExhausted);
            const auto denied = competing.Begin(scene.scene->View(), *scene.spatial, physics.capability, frame, {}, budget);
            REQUIRE(denied.HasValue());
            CHECK_FALSE(denied.Value().pending);
            CHECK(denied.Value().observations[0].evidence == PerceptionSightEvidence::BudgetExhausted);
            PerceptionSight queued;
            PerceptionSightBudget another{.simulationTick = physics.tick, .remainingRaycasts = 2};
            const auto capacity = queued.Begin(scene.scene->View(), *scene.spatial, physics.capability, frame, {}, another);
            REQUIRE(capacity.HasValue());
            CHECK_FALSE(capacity.Value().pending);
            CHECK(another.remainingRaycasts == 2);
            CHECK(capacity.Value().observations[0].evidence == PerceptionSightEvidence::BudgetExhausted);
        }

        TEST_CASE("Sight stale target completions cannot reveal a replacement entity", "[unit][ai][sight][canonical]") {
            SightScene scene;
            SightWorld physics;
            PerceptionSight sight;
            const auto frame = physics.CompletePending(sight, scene);
            const auto oldTarget = scene.target;
            Runtime::SceneCommandBuffer commands;
            commands.Destroy(oldTarget);
            REQUIRE(commands.Create({}).IsValid());
            const auto committed = scene.scene->Commit(commands);
            REQUIRE(committed.HasValue());
            REQUIRE(committed.Value().created.size() == 1);
            scene.target = committed.Value().created[0].entity;
            REQUIRE(scene.target != oldTarget);
            const auto old = sight.Poll(scene.scene->View(), scene.revision, Publication(physics.capability, frame));
            REQUIRE(old.HasValue());
            CHECK(old.Value().observations[0].source == oldTarget);
            CHECK(old.Value().observations[0].evidence == PerceptionSightEvidence::Stale);
            CHECK_FALSE(old.Value().observations[0].visible);
            scene.Publish();
            physics.Advance();
            const auto replacement = physics.Sample(sight, scene);
            CHECK(replacement.observations[0].source == scene.target);
            CHECK(replacement.observations[0].visible);
        }

        TEST_CASE("Sight completion requires current spatial Physics and capability generations", "[unit][ai][sight][canonical]") {
            SightScene scene;
            SightWorld physics;
            PerceptionSight sight;
            const auto frame = physics.CompletePending(sight, scene);
            auto publication = Publication(physics.capability, frame);
            auto spatialRevision = scene.revision;
            SECTION("spatial replacement") {
                ++spatialRevision;
            }
            SECTION("Physics publication") {
                ++publication.publicationRevision;
            }
            SECTION("Physics scene") {
                ++publication.sceneGeneration;
            }
            SECTION("capability replacement") {
                ++publication.identity.capabilityGeneration;
            }
            SECTION("world replacement") {
                publication.identity.world = Physics::PhysicsWorldId::Create(52).Value();
            }
            SECTION("tick replacement") {
                ++publication.completedTick;
            }
            const auto stale = sight.Poll(scene.scene->View(), spatialRevision, publication);
            REQUIRE(stale.HasValue());
            CHECK_FALSE(stale.Value().pending);
            CHECK(stale.Value().observations[0].evidence == PerceptionSightEvidence::Stale);
            CHECK_FALSE(stale.Value().observations[0].visible);
        }

        TEST_CASE("Sight reset destruction revocation and world teardown close pending work", "[unit][ai][sight][canonical]") {
            SightScene scene;
            SightWorld physics;
            PerceptionSight sight;
            const auto frame = physics.Queue(sight, scene, {}, 1);
            SECTION("reset then re-admit") {
                sight.Reset();
                CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
                physics.Advance();
                CHECK(physics.Sample(sight, scene).observations[0].visible);
                return;
            }
            SECTION("scene replacement") {
                SightScene replacement{88};
                scene.scene.reset();
                const auto stale = sight.Poll(replacement.scene->View(), scene.revision, Publication(physics.capability, frame));
                REQUIRE(stale.HasValue());
                CHECK_FALSE(stale.Value().observations[0].visible);
                CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
                return;
            }
            SECTION("destructor cancels") {
                sight.Reset();
                {
                    PerceptionSight temporary;
                    (void)physics.Queue(temporary, scene, {}, 1);
                }
                CHECK(physics.world->ProcessQueryBatch().HasValue());
                return;
            }
            SECTION("revocation") {
                REQUIRE(physics.world->RevokeQueryEventCapability(physics.capability).HasValue());
            }
            SECTION("world teardown") {
                physics.world->Shutdown();
                physics.world.reset();
                physics.runtime.reset();
            }
            const auto failed = sight.Poll(scene.scene->View(), scene.revision, Publication(physics.capability, frame));
            CHECK(failed.HasError());
            CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
            CHECK(sight.Poll(scene.scene->View(), scene.revision, Publication(physics.capability, frame)).HasError());
        }

        TEST_CASE("Sight malformed copied completions fail terminally without publishing visibility",
                  "[unit][ai][sight][canonical][fault]") {
            SightScene scene;
            SightWorld physics;
            (void)physics.Box(-5);
            physics.Advance();
            PerceptionSight sight;
            const auto frame = physics.CompletePending(sight, scene);
            auto malformed = PerceptionSightTestAccess::Completion(sight);
            REQUIRE(malformed.entries.size() == 1);
            REQUIRE(malformed.entries[0].hits.size() == 1);
            SECTION("wrong entry count") {
                malformed.entries.clear();
            }
            SECTION("wrong hit count") {
                malformed.entries[0].hits.clear();
            }
            SECTION("missing filter schema") {
                malformed.entries[0].completion.result.filterSchemaGeneration = 0;
            }
            SECTION("missing broadphase generation") {
                malformed.entries[0].completion.result.broadphaseSnapshotGeneration = 0;
            }
            SECTION("wrong completion tick") {
                ++malformed.entries[0].completion.completedTick;
            }
            SECTION("wrong publication") {
                ++malformed.entries[0].completion.publicationRevision;
            }
            SECTION("foreign hit") {
                malformed.entries[0].hits[0].body.world = Physics::PhysicsWorldId::Create(52).Value();
            }
            SECTION("wrong hit schema") {
                ++malformed.entries[0].hits[0].filterSchemaGeneration;
            }
            SECTION("invalid hit response") {
                malformed.entries[0].hits[0].response = static_cast<Physics::PhysicsQueryResponse>(255);
            }
            const auto failed = PerceptionSightTestAccess::Consume(sight, malformed);
            CHECK(failed.HasError());
            CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
            CHECK_FALSE(PerceptionSightTestAccess::Current(sight).observations[0].visible);
            CHECK(sight.Poll(scene.scene->View(), scene.revision, Publication(physics.capability, frame)).HasError());
            physics.Advance();
            CHECK(physics.Sample(sight, scene).observations[0].evidence == PerceptionSightEvidence::Occluded);
        }

        TEST_CASE("Sight truncated empty evidence remains unknown and does not produce Clear", "[unit][ai][sight][canonical][fault]") {
            SightScene scene;
            SightWorld physics;
            PerceptionSight sight;
            REQUIRE(physics.Sample(sight, scene).observations[0].visible);
            physics.Advance();
            (void)physics.CompletePending(sight, scene);
            auto truncated = PerceptionSightTestAccess::Completion(sight);
            truncated.entries[0].completion.result.truncated = true;
            const auto result = PerceptionSightTestAccess::Consume(sight, truncated);
            REQUIRE(result.HasValue());
            CHECK(result.Value().observations[0].evidence == PerceptionSightEvidence::QueryTruncated);
            CHECK(result.Value().observations[0].visible);
            CHECK_FALSE(PerceptionSightTestAccess::Pending(sight));
        }
#endif
    }  // namespace
}  // namespace Horo::AI
