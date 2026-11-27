#include "AllocationProbe.h"
#include "Horo/AI/EnvironmentQueryPhysics.h"
#include "PerceptionSightTestSupport.h"

#include <algorithm>
#include <array>
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

        TEST_CASE("EQS Physics probes reject a mismatched binding before enqueueing", "[unit][ai][sight][eqs]") {
            const Physics::PhysicsQueryEventCapability absent;
            const QueryPhysicsPublication publication{.identity = {.capabilityGeneration = 1},
                                                      .sceneGeneration = 7,
                                                      .revision = 1,
                                                      .completedTick = 1};
            QueryPhysicsProbe probe;
            const auto outcome = probe.Start(absent, publication, QueryStageId::Create(1).Value(), Physics::PhysicsRayQuery{}, {});
            REQUIRE(outcome.HasError());
            CHECK(outcome.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityStale.code.Value());
            CHECK_FALSE(probe.Poll(publication).Value().has_value());
        }

        TEST_CASE("EQS visibility projects precise local rays and keeps coincident candidate slots", "[unit][ai][sight][eqs]") {
            const auto far = std::numeric_limits<std::int64_t>::max();
            const auto origin = Math::WorldCoordinate64::FromMillimeters(far - 1000, 0, 0);
            const auto eye = Math::WorldCoordinate64::FromMillimeters(far - 500, 0, 0);
            const std::array points{eye, Math::WorldCoordinate64::FromMillimeters(far, 0, 0),
                                    Math::WorldCoordinate64::FromMillimeters(far - 1000, 0, 0)};
            const auto projected = ProjectQueryPhysicsVisibilityRays({.eye = eye, .points = points}, origin);
            REQUIRE(projected.HasValue());
            REQUIRE(projected.Value().count == 2);
            CHECK(projected.Value().indices[0] == 1);
            CHECK(projected.Value().indices[1] == 2);
            CHECK(projected.Value().rays[0].origin.x == 0.5F);
            CHECK(projected.Value().rays[0].direction.x == 1.0F);
            CHECK(projected.Value().rays[1].direction.x == -1.0F);
            CHECK(projected.Value().rays[0].maximumDistanceMeters == 0.5F);
        }

        TEST_CASE("EQS visibility retries admission and drops detached results from a retired owner", "[unit][ai][sight][eqs]") {
            using Capture = Result<std::optional<QueryPhysicsVisibilityResult>>;

            class Provider final : public IQueryPhysicsVisibilityOwner {
            public:
                bool live{true};
                std::size_t attempts{};

                bool RetainsVisibilityAuthority(const Runtime::RuntimeSceneView &,
                                                const QueryPhysicsPublication &) const noexcept override {
                    return live;
                }

                Capture CaptureVisibility(const QueryPhysicsVisibilityRequest &request) override {
                    if (++attempts == 1)
                        return Capture::Success(std::nullopt);
                    QueryPhysicsVisibilityResult result{
                        .publication = {.identity = {Physics::PhysicsWorldId::Create(1).Value(), 1},
                                        .sceneGeneration = request.querier.runtime.value,
                                        .revision = 4,
                                        .completedTick = 2},
                        .stage = request.stage,
                        .count = request.points.size(),
                    };
                    result.clear.front() = true;
                    return Capture::Success(result);
                }
            } provider;

            SightScene fixture;
            const auto scene = fixture.scene->View();
            const auto registry = CaptureBuiltinQuerySchema();
            REQUIRE(registry.HasValue());
            const auto initial = CreateBuiltinLatticeQuery(QueryId::Create(1).Value(), QueryStageId::Create(1).Value(),
                                                           QueryResultSchemaId::Create(1).Value(), "Visibility",
                                                           {.stepMillimeters = 500, .outerSteps = 0}, 1);
            REQUIRE(initial.HasValue());
            const auto source =
                AddBuiltinPointVisibilityTest(initial.Value(), QueryStageId::Create(2).Value(), VisibilityChannel().Bytes());
            REQUIRE(source.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(source.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry.Value());
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan.Value(), registry.Value(), providers.Value(), scene,
                                                  {.querier = fixture.listener, .executionRevision = 1});
            REQUIRE(contexts.HasValue());
            const AgentHandle agent{AiRuntimeIncarnation::Create(1).Value(), {1, 1}};
            auto admitted =
                AdmitBuiltinPointVisibilityQuery(plan.Value(), registry.Value(), scene, fixture.listener, {agent, contexts.Value()},
                                                 {.submittedTick = 1, .deadlineTick = 6, .maximumCandidates = 1, .bestCount = 1}, provider);
            REQUIRE(admitted.HasValue());
            auto &query = *admitted.Value();
            AiWorkBudget allowance{64};
            const QueryExecutionBudget budget{allowance, allowance, allowance};
            CHECK_FALSE(query.Sync(2));
            query.Advance(2, budget);
            CHECK(provider.attempts == 1);
            CHECK_FALSE(query.Sync(3));
            query.Advance(3, budget);
            CHECK(provider.attempts == 2);
            SECTION("the current Physics authority publishes the candidate") {
                REQUIRE(query.Sync(4));
                REQUIRE(query.Published());
                CHECK(query.Published()->status == QueryExecutionStatus::Completed);
                REQUIRE(query.Published()->winners.size() == 1);
            }
            SECTION("retirement takes precedence over a detached candidate") {
                provider.live = false;
                REQUIRE(query.Sync(4));
                REQUIRE(query.Published());
                CHECK(query.Published()->status == QueryExecutionStatus::StaleInputs);
                CHECK(query.Published()->winners.empty());
            }
        }

#if HORO_TEST_PHYSICS_NATIVE
        /** @brief Captures the exact Physics test world publication. */
        QueryPhysicsPublication ProbePublication(const SightWorld &physics) {
            const auto tick = physics.world->PublishedTick();
            return {physics.capability.Identity(), 7, tick.publicationRevision, tick.completedTick};
        }

        TEST_CASE("EQS Physics ray and capsule probes preserve blocking evidence", "[unit][ai][sight][eqs]") {
            SightWorld physics;
            const auto fixture = physics.Box(-5);
            physics.Advance();
            const auto publication = ProbePublication(physics);
            const auto stage = QueryStageId::Create(1).Value();
            const Physics::PhysicsRayQuery ray{.origin = {0, 0, 0}, .maximumDistanceMeters = 10};
            Physics::PhysicsQueryFilter filter{.channel = VisibilityChannel()};
            QueryPhysicsProbe probe;
            REQUIRE(probe.Start(physics.capability, publication, stage, ray, filter).Value());
            REQUIRE_FALSE(probe.Poll(publication).Value().has_value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            const auto blocked = probe.Poll(publication);
            REQUIRE(blocked.HasValue());
            REQUIRE(blocked.Value().has_value());
            CHECK(blocked.Value()->stage == stage);
            CHECK(blocked.Value()->filter == false);
            CHECK(blocked.Value()->metric == 0.0);
            CHECK_FALSE(probe.Poll(publication).Value().has_value());

            const Physics::PhysicsCapsuleOverlapQuery overlap{.capsule = {.radiusMeters = 0.25F, .cylindricalHalfHeightMeters = 0.5F},
                                                              .position = {0, 0, -5}};
            REQUIRE(probe.Start(physics.capability, publication, stage, overlap, filter).Value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            const auto occupied = probe.Poll(publication);
            REQUIRE(occupied.HasValue());
            REQUIRE(occupied.Value().has_value());
            CHECK(occupied.Value()->filter == false);

            filter.excludedBody = fixture.body;
            REQUIRE(probe.Start(physics.capability, publication, stage, ray, filter).Value());
            QueryPhysicsProbe competing;
            CHECK_FALSE(competing.Start(physics.capability, publication, stage, ray, filter).Value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            const auto clear = probe.Poll(publication);
            REQUIRE(clear.HasValue());
            REQUIRE(clear.Value().has_value());
            CHECK(clear.Value()->filter == true);
            CHECK(clear.Value()->metric == 1.0);
        }

        TEST_CASE("EQS exposure batches publish complete evidence against one Physics tick", "[unit][ai][sight][eqs]") {
            SightWorld physics;
            (void)physics.Box(-5);
            physics.Advance();
            const auto publication = ProbePublication(physics);
            const auto stage = QueryStageId::Create(2).Value();
            const Physics::PhysicsQueryFilter filter{.channel = VisibilityChannel()};
            const Physics::PhysicsRayQuery blocked{.maximumDistanceMeters = 10};
            const Physics::PhysicsRayQuery clear{.origin = {2, 0, 0}, .maximumDistanceMeters = 10};
            const std::array rays{blocked, clear};
            QueryPhysicsProbe exposure;
            const std::array<Physics::PhysicsRayQuery, QueryPhysicsProbe::MaximumExposureRays + 1> oversized{};
            CHECK(exposure.StartExposure(physics.capability, publication, stage, oversized, filter).HasError());
            CHECK(exposure.StartExposure(physics.capability, publication, stage, {}, filter).HasError());
            auto invalid = rays;
            invalid.back().maximumDistanceMeters = -1;
            CHECK(exposure.StartExposure(physics.capability, publication, stage, invalid, filter).HasError());
            CHECK_FALSE(exposure.Poll(publication).Value().has_value());

            REQUIRE(exposure.StartExposure(physics.capability, publication, stage, rays, filter).Value());
            QueryPhysicsProbe other;
            REQUIRE_FALSE(other.Start(physics.capability, publication, stage, blocked, filter).Value());
            CHECK_FALSE(exposure.Poll(publication).Value().has_value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            SECTION("replaced publication invalidates even a completed exposure batch") {
                physics.Advance();
                const auto stale = exposure.Poll(publication);
                REQUIRE(stale.HasError());
                CHECK(stale.ErrorValue().code.Value() == Physics::PhysicsErrors::QuerySnapshotStale.code.Value());
            }
            SECTION("one blocker and one clear ray report partial exposure") {
                const auto sample = exposure.Poll(publication);
                REQUIRE(sample.HasValue());
                REQUIRE(sample.Value().has_value());
                CHECK(sample.Value()->stage == stage);
                CHECK(sample.Value()->filter == true);
                CHECK(sample.Value()->metric == 0.5);
            }
            CHECK_FALSE(exposure.Poll(publication).Value().has_value());
            const auto next = ProbePublication(physics);
            REQUIRE(other.Start(physics.capability, next, stage, blocked, filter).Value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            const auto independent = other.Poll(next);
            REQUIRE(independent.HasValue());
            REQUIRE(independent.Value().has_value());
            CHECK(independent.Value()->filter == false);
        }

        TEST_CASE("EQS visibility preserves every candidate against one Physics publication", "[unit][ai][sight][eqs]") {
            SightWorld physics;
            (void)physics.Box(-5);
            physics.Advance();
            const auto publication = ProbePublication(physics);
            const auto stage = QueryStageId::Create(2).Value();
            const Physics::PhysicsQueryFilter filter{.channel = VisibilityChannel()};
            const Physics::PhysicsRayQuery blocked{.maximumDistanceMeters = 10};
            const Physics::PhysicsRayQuery clear{.origin = {2, 0, 0}, .maximumDistanceMeters = 10};
            const std::array rays{clear, blocked, blocked, clear, blocked, clear, clear, blocked, clear};
            const std::array expected{true, false, false, true, false, true, true, false, true};
            QueryPhysicsProbe probe;
            REQUIRE(probe.StartVisibility(physics.capability, publication, stage, rays, filter).Value());
            CHECK_FALSE(probe.PollVisibility(publication).Value().has_value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());

            SECTION("completed rays stay in submission order") {
                const auto result = probe.PollVisibility(publication);
                REQUIRE(result.HasValue());
                REQUIRE(result.Value());
                const auto &receipt = *result.Value();
                CHECK(receipt.stage == stage);
                CHECK(receipt.publication.identity.world == publication.identity.world);
                CHECK(receipt.publication.identity.capabilityGeneration == publication.identity.capabilityGeneration);
                CHECK(receipt.publication.sceneGeneration == publication.sceneGeneration);
                CHECK(receipt.publication.revision == publication.revision);
                CHECK(receipt.publication.completedTick == publication.completedTick);
                REQUIRE(receipt.count == rays.size());
                CHECK(std::ranges::equal(std::span{receipt.clear}.first(receipt.count), expected));
            }
            SECTION("the next Physics tick makes an unread batch stale") {
                physics.Advance();
                const auto result = probe.PollVisibility(publication);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == Physics::PhysicsErrors::QuerySnapshotStale.code.Value());
            }
            CHECK_FALSE(probe.PollVisibility(publication).Value().has_value());
        }

        TEST_CASE("EQS Physics probe releases backpressure and retires stale or cancelled work", "[unit][ai][sight][eqs]") {
            SightWorld physics;
            const auto publication = ProbePublication(physics);
            const auto stage = QueryStageId::Create(1).Value();
            const Physics::PhysicsRayQuery ray{.origin = {0, 0, 0}, .maximumDistanceMeters = 10};
            const Physics::PhysicsQueryFilter filter{.channel = VisibilityChannel()};
            QueryPhysicsProbe probe;
            REQUIRE(probe.Start(physics.capability, publication, stage, ray, filter).Value());
            QueryPhysicsProbe competing;
            CHECK_FALSE(competing.Start(physics.capability, publication, stage, ray, filter).Value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            REQUIRE(probe.Poll(publication).Value().has_value());
            REQUIRE(competing.Start(physics.capability, publication, stage, ray, filter).Value());
            auto replaced = publication;
            ++replaced.identity.capabilityGeneration;
            const auto stale = competing.Poll(replaced);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == Physics::PhysicsErrors::QuerySnapshotStale.code.Value());
            CHECK_FALSE(competing.Poll(publication).Value().has_value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());

            REQUIRE(probe.Start(physics.capability, publication, stage, ray, filter).Value());
            probe.Cancel();
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            CHECK_FALSE(probe.Poll(publication).Value().has_value());
        }

        TEST_CASE("EQS Physics consumes only live capability results at the captured publication", "[unit][ai][sight][eqs]") {
            SightWorld physics;
            const auto publication = ProbePublication(physics);
            const auto stage = QueryStageId::Create(1).Value();
            const Physics::PhysicsRayQuery ray{.origin = {0, 0, 0}, .maximumDistanceMeters = 10};
            const Physics::PhysicsQueryFilter filter{.channel = VisibilityChannel()};
            QueryPhysicsProbe probe;
            REQUIRE(probe.Start(physics.capability, publication, stage, ray, filter).Value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());

            auto expected = Physics::PhysicsErrors::CapabilityRevoked.code.Value();
            SECTION("explicit revocation after provider completion") {
                REQUIRE(physics.world->RevokeQueryEventCapability(physics.capability).HasValue());
            }
            SECTION("destroyed world with a retained historical completion") {
                physics.world.reset();
                expected = Physics::PhysicsErrors::CapabilityStale.code.Value();
            }
            SECTION("a newer completed tick while the caller presents the old publication") {
                physics.Advance();
                expected = Physics::PhysicsErrors::QuerySnapshotStale.code.Value();
            }
            const auto retired = probe.Poll(publication);
            REQUIRE(retired.HasError());
            CHECK(retired.ErrorValue().code.Value() == expected);
            CHECK_FALSE(probe.Poll(publication).Value().has_value());
        }

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
