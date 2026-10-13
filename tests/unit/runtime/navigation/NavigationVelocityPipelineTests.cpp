#include "AllocationProbe.h"
#include "Horo/Navigation/NavigationVelocityPipeline.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#include <catch2/catch_approx.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Navigation {
    namespace {
        using namespace CoordinatorTestSupport;

        class RouteBackend final : public INavigationQueryBackend {
        public:
            NavigationProviderCapabilities Capabilities() const noexcept override {
                return MakeAvailablePathQueryCapabilities(1, Request().requirement.limits, 1);
            }

            Result<NavigationPath> FindPath(const NavigationPathRequest &request, const CancellationToken &) const override {
                NavigationPath path;
                path.sourceGeneration = request.topology;
                path.status = NavigationPathStatus::Reachable;
                const auto point = [&](const Math::Vec3 position, const NavigationPathWaypointKind kind) {
                    return NavigationPathWaypoint{position,
                                                  {.kind = kind, .polygon = {.world = request.world, .topology = request.topology}}};
                };
                path.waypoints = {point(request.start, NavigationPathWaypointKind::Start),
                                  point(request.destination, NavigationPathWaypointKind::Destination)};
                return Result<NavigationPath>::Success(std::move(path));
            }
        };

        class SampleBackend final : public INavigationCrowdBackend {
        public:
            Result<NavigationAvoidanceOutcome> Solve(const NavigationCrowdSnapshot &snapshot,
                                                     const NavigationAvoidanceRequest &request) override {
                return Result<NavigationAvoidanceOutcome>::Success({request.preferredVelocity, NavigationAvoidanceDisposition::Sampled,
                                                                    NavigationAvoidanceStopReason::None, snapshot.Binding(),
                                                                    snapshot.DynamicRevision(), snapshot.CaptureTick()});
            }
        };

        struct VelocityFixture final {
            JobSystem jobs{{.workerCount = 1}};
            NavigationWorldLifecycle world{std::move(NavigationWorldLifecycle::Create(4)).Value()};
            NavigationCoordinator coordinator{std::move(NavigationCoordinator::Create(jobs, Limits())).Value()};
            NavigationAgentRegistry registry{std::move(NavigationAgentRegistry::Create({.maximumAgents = 4})).Value()};
            NavigationDynamicRegistry dynamic{std::move(NavigationDynamicRegistry::Create()).Value()};
            NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
            CrowdAgentHandle agent;
            std::unique_ptr<NavigationAgentSceneCandidate> candidate;
            std::unique_ptr<NavigationMovementController> movement;
            std::unique_ptr<NavigationVelocityPipeline> pipeline;
            SampleBackend solver;
            Math::Vec3 position{};

            explicit VelocityFixture(const NavigationVelocityPolicy policy = {}, const std::uint32_t maximumQueries = 16) {
                Activate(world, std::make_unique<RouteBackend>());
                const std::array descriptors{NavigationAgentDescriptor{.owner = {binding.scene, 1, 1},
                                                                       .profile = NavigationAgentProfileId::Create(1).Value(),
                                                                       .filter = Request().filter}};
                candidate = std::move(registry.PrepareScene(binding, descriptors)).Value();
                candidate->Publish();
                agent = registry.Snapshot().Value().Agents().front().handle;
                movement = std::move(NavigationMovementController::Create(registry, world, coordinator, agent, Caller())).Value();
                pipeline = std::move(NavigationVelocityPipeline::Create(*movement, binding, agent, 1, policy)).Value();
                REQUIRE(dynamic.CommitAtSafePoint(Activation(), 1).HasValue());
                REQUIRE(movement
                            ->Start({.destination = {10, 0, 0},
                                     .maximumQueries = maximumQueries,
                                     .query = Request().requirement,
                                     .capabilityRevision = 1},
                                    Now(0))
                            .HasValue());
            }

            NavigationMovementObservation Now(const std::uint64_t tick) const {
                return {.binding = binding,
                        .position = position,
                        .stepSeconds = 0.1F,
                        .path = {.provenance = Source(), .linkRevision = 1, .goalRevision = 1, .tick = tick}};
            }

            NavigationCrowdSnapshot Capture(const std::uint64_t tick, const Math::Vec3 velocity = {}) const {
                const std::array motions{NavigationCrowdMotionSample{.handle = agent, .position = position, .velocity = velocity}};
                const std::array profiles{NavigationCrowdProfileFacts{.profile = NavigationAgentProfileId::Create(1).Value()}};
                auto snapshot =
                    BuildNavigationCrowdSnapshot(registry.Snapshot().Value(), dynamic.Snapshot().Value(), motions, profiles, {}, tick);
                REQUIRE(snapshot.HasValue());
                return std::move(snapshot).Value();
            }

            void PublishRoute() {
                const auto intent = movement->Update(Now(0));
                REQUIRE(intent.HasValue());
                REQUIRE(intent.Value().state == NavigationMovementState::Querying);
                REQUIRE_FALSE(movement->Status().error.has_value());
                CommitRoute(1);
            }

            void CommitRoute(const std::uint64_t tick) {
                REQUIRE(coordinator.Dispatch(tick - 1) == 1);
                const std::array callers{Caller()};
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                std::uint32_t committed = 0;
                while (!committed && std::chrono::steady_clock::now() < deadline) {
                    committed = coordinator.Commit(Current(callers, tick));
                    std::this_thread::yield();
                }
                REQUIRE(committed == 1);
            }

            ~VelocityFixture() {
                pipeline->Retire();
                movement->BeginShutdown();
                coordinator.BeginShutdown();
                jobs.Shutdown(ShutdownPolicy::Cancel);
            }

            NavigationVelocityWork Prepare(const NavigationCrowdSnapshot &snapshot) {
                auto work = pipeline->Prepare(Now(snapshot.CaptureTick()), snapshot,
                                              {.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = 0.1F});
                REQUIRE(work.HasValue());
                return work.Value();
            }

            NavigationVelocityPublication Publish(const NavigationCrowdSnapshot &snapshot) {
                const auto work = Prepare(snapshot);
                const auto sampled = solver.Solve(snapshot, work.request);
                REQUIRE(sampled.HasValue());
                auto result = pipeline->Commit(work, sampled.Value(), snapshot, Now(snapshot.CaptureTick()));
                REQUIRE(result.HasValue());
                return result.Value();
            }

            void Feedback(const NavigationVelocityPublication &publication, const bool blocked, const Math::Vec3 displacement = {}) {
                REQUIRE(pipeline
                            ->ObserveFeedback({publication.intent.operation, publication.intent.path, publication.intent.tick,
                                               publication.characterRevision, displacement, blocked})
                            .HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Movement tick zero remains current and velocity rejects repeated admitted publication ticks",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        const auto zero = fixture.movement->Update(fixture.Now(0));
        REQUIRE(zero.HasValue());
        CHECK(zero.Value().tick == 0);
        CHECK(fixture.movement->IsCurrentIntent(zero.Value()));
        fixture.CommitRoute(1);
        const auto snapshot = fixture.Capture(1);
        const auto first = fixture.Publish(snapshot);
        CHECK(first.intent.tick == 1);
        CHECK(fixture.movement->IsCurrentIntent(first.intent));
        const auto repeated = fixture.pipeline->Prepare(fixture.Now(1), snapshot, {.stepSeconds = 0.1F});
        REQUIRE(repeated.HasError());
        CHECK(repeated.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
        fixture.Feedback(first, false);
        const auto next = fixture.Publish(fixture.Capture(2));
        CHECK(next.intent.tick == 2);
        CHECK(next.intent.state == NavigationMovementState::Following);
        CHECK(next.intent.desiredVelocity.x == Catch::Approx(1));
    }

    TEST_CASE("Velocity pipeline routes preferred motion through solver publication and exact Character feedback",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto first = fixture.Publish(fixture.Capture(1));
        CHECK(first.intent.desiredVelocity.x == Catch::Approx(1));
        CHECK(first.intent.path.IsValid());
        fixture.Feedback(first, false, {0.1F, 0, 0});
        const auto second = fixture.Publish(fixture.Capture(2, {1, 0, 0}));
        CHECK(second.intent.operation == first.intent.operation);
        CHECK(second.intent.tick == 2);
        CHECK(second.binding == fixture.binding);
    }

    TEST_CASE("Velocity publication rejects stale work capture and malformed solver samples without consuming pending work",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto snapshot = fixture.Capture(1);
        const auto work = fixture.Prepare(snapshot);
        auto outcome = fixture.solver.Solve(snapshot, work.request).Value();
        auto stale = work;
        ++stale.sequence;
        CHECK(fixture.pipeline->Commit(stale, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        CHECK(fixture.pipeline->Commit(work, outcome, fixture.Capture(2), fixture.Now(2)).HasError());
        CHECK(fixture.pipeline->Commit(work, outcome, fixture.Capture(1), fixture.Now(1)).HasError());
        outcome.captureTick = 2;
        CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        outcome.captureTick = 1;
        outcome.desiredVelocity.x = std::numeric_limits<float>::quiet_NaN();
        CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        outcome.desiredVelocity = {4, 0, 0};
        CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        outcome.desiredVelocity = {1, 0, 0};
        REQUIRE(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasValue());
        CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
    }

    TEST_CASE("Velocity immutable work crosses JobSystem while only the owner publishes or rejects a cancelled result",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto snapshot = fixture.Capture(1);
        const auto work = fixture.Prepare(snapshot);
        JobSystem workers{{.workerCount = 1}};
        std::optional<Result<NavigationAvoidanceOutcome>> result;
        const auto job = workers.Submit({}, [work, snapshot, &result](const CancellationToken &) {
            SampleBackend solver;
            result = solver.Solve(snapshot, work.request);
        });
        REQUIRE(job.HasValue());
        workers.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(result.has_value());
        REQUIRE(result->HasValue());
        REQUIRE(fixture.pipeline->Commit(work, result->Value(), snapshot, fixture.Now(1)).HasValue());

        const auto nextSnapshot = fixture.Capture(2);
        const auto next = fixture.Prepare(nextSnapshot);
        JobSystem cancelled{{.workerCount = 0}};
        bool ran = false;
        const auto queued = cancelled.Submit({}, [&ran](const CancellationToken &) {
            ran = true;
        });
        REQUIRE(queued.HasValue());
        REQUIRE(cancelled.RequestCancel(queued.Value().Id()).HasValue());
        cancelled.Shutdown(ShutdownPolicy::Cancel);
        CHECK_FALSE(ran);
        REQUIRE(fixture.pipeline->CancelPending(next));
        CHECK(fixture.pipeline->Commit(next, result->Value(), nextSnapshot, fixture.Now(2)).HasError());
    }

    TEST_CASE("Velocity feedback rejects old operation path tick Character incarnation and duplicate acknowledgements",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto publication = fixture.Publish(fixture.Capture(1));
        const NavigationVelocityFeedback correct{publication.intent.operation, publication.intent.path, 1, 1, {0.1F, 0, 0}, false};
        for (int field = 0; field != 5; ++field) {
            auto wrong = correct;
            if (field == 0)
                ++wrong.operation.sequence;
            if (field == 1)
                ++wrong.path.slot.generation;
            if (field == 2)
                ++wrong.tick;
            if (field == 3)
                ++wrong.characterRevision;
            if (field == 4)
                wrong.achievedDisplacement.x = std::numeric_limits<float>::infinity();
            CHECK(fixture.pipeline->ObserveFeedback(wrong).HasError());
        }
        REQUIRE(fixture.pipeline->ObserveFeedback(correct).HasValue());
        CHECK(fixture.pipeline->ObserveFeedback(correct).HasError());
        CHECK(fixture.Publish(fixture.Capture(2)).intent.tick == 2);
    }

    TEST_CASE("Velocity blocked feedback has consecutive tick hysteresis and a bounded repath request", "[unit][navigation][velocity]") {
        VelocityFixture fixture{{.blockedTicksBeforeRepath = 2, .repathCooldownTicks = 10}};
        fixture.PublishRoute();
        auto publication = fixture.Publish(fixture.Capture(1));
        fixture.Feedback(publication, true);
        publication = fixture.Publish(fixture.Capture(2));
        CHECK(publication.intent.state == NavigationMovementState::Following);
        fixture.Feedback(publication, false, {0.1F, 0, 0});
        publication = fixture.Publish(fixture.Capture(3));
        fixture.Feedback(publication, true);
        publication = fixture.Publish(fixture.Capture(4));
        CHECK(publication.intent.state == NavigationMovementState::Following);
        fixture.Feedback(publication, true);
        publication = fixture.Publish(fixture.Capture(5));
        CHECK(publication.intent.state == NavigationMovementState::Querying);
        CHECK(publication.intent.desiredVelocity == Math::Vec3{});
    }

    TEST_CASE("Velocity reset retires old work feedback and corridor without replacing the movement owner",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto snapshot = fixture.Capture(1);
        const auto work = fixture.Prepare(snapshot);
        const auto outcome = fixture.solver.Solve(snapshot, work.request).Value();
        fixture.position = {20, 0, 0};
        const auto reset = fixture.Now(2);
        REQUIRE(fixture.pipeline->Reset(reset, 2).HasValue());
        CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        CHECK(fixture.pipeline->ObserveFeedback({work.preferred.operation, work.preferred.path, 1, 1, {}, true}).HasError());
        CHECK(fixture.pipeline->Reset(reset, 2).HasError());
        const auto next = fixture.Publish(fixture.Capture(3));
        CHECK(next.characterRevision == 2);
        CHECK(next.intent.desiredVelocity == Math::Vec3{});
        CHECK(fixture.movement->Status().id == work.preferred.operation);
    }

    TEST_CASE("Velocity repath cooldown and total query ceiling bound persistent blockage across replacement paths",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture{{.blockedTicksBeforeRepath = 1, .repathCooldownTicks = 4}, 2};
        fixture.PublishRoute();
        auto publication = fixture.Publish(fixture.Capture(1));
        fixture.Feedback(publication, true);
        publication = fixture.Publish(fixture.Capture(2));
        REQUIRE(publication.intent.state == NavigationMovementState::Querying);
        fixture.CommitRoute(3);
        publication = fixture.Publish(fixture.Capture(3));
        REQUIRE(publication.intent.state == NavigationMovementState::Following);
        for (std::uint64_t tick = 4; tick <= 5; ++tick) {
            fixture.Feedback(publication, true);
            publication = fixture.Publish(fixture.Capture(tick));
            CHECK(publication.intent.state == NavigationMovementState::Following);
        }
        fixture.Feedback(publication, true);
        publication = fixture.Publish(fixture.Capture(6));
        CHECK(publication.intent.state == NavigationMovementState::Failed);
        CHECK(publication.intent.desiredVelocity == Math::Vec3{});
        REQUIRE(fixture.movement->Status().error.has_value());
        CHECK(fixture.movement->Status().error->domain.Value() == NavigationErrors::QueryLimitExceeded.domain.Value());
        CHECK(fixture.movement->Status().error->code.Value() == NavigationErrors::QueryLimitExceeded.code.Value());
    }

    TEST_CASE("Velocity cancellation shutdown and wrong-thread calls cannot publish retained worker results",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto snapshot = fixture.Capture(1);
        const auto work = fixture.Prepare(snapshot);
        const auto outcome = fixture.solver.Solve(snapshot, work.request).Value();
        bool rejected = false;
        std::jthread other([&fixture, &work, &outcome, &snapshot, &rejected] {
            rejected = fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError();
        });
        other.join();
        CHECK(rejected);
        REQUIRE(fixture.pipeline->CancelPending(work));
        CHECK_FALSE(fixture.pipeline->CancelPending(work));
        CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        const auto next = fixture.Prepare(fixture.Capture(2));
        fixture.pipeline->Retire();
        CHECK(fixture.pipeline->Commit(next, outcome, snapshot, fixture.Now(snapshot.CaptureTick())).HasError());
        CHECK(fixture.pipeline->Prepare(fixture.Now(3), fixture.Capture(3), {}).HasError());
    }

    TEST_CASE("Velocity publication revalidates live movement agent and world authority after worker completion",
              "[unit][navigation][velocity]") {
        for (int revoke = 0; revoke != 4; ++revoke) {
            VelocityFixture fixture;
            fixture.PublishRoute();
            const auto snapshot = fixture.Capture(1);
            const auto work = fixture.Prepare(snapshot);
            const auto outcome = fixture.solver.Solve(snapshot, work.request).Value();
            if (revoke == 0)
                REQUIRE(fixture.movement->Cancel(work.preferred.operation));
            else if (revoke == 1)
                fixture.movement->BeginShutdown();
            else if (revoke == 2)
                REQUIRE(fixture.registry.UnregisterAtSafePoint(fixture.agent).HasValue());
            else
                fixture.world.BeginShutdown();
            CHECK(fixture.pipeline->Commit(work, outcome, snapshot, fixture.Now(1)).HasError());
        }
    }

    TEST_CASE("Velocity solver envelope rejects excessive acceleration and admits explicit collision safe stops",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto snapshot = fixture.Capture(1);
        auto envelope = NavigationAvoidanceRequest{.maximumAccelerationMetersPerSecondSquared = 1, .stepSeconds = 0.1F};
        const auto prepared = fixture.pipeline->Prepare(fixture.Now(1), snapshot, envelope);
        REQUIRE(prepared.HasValue());
        auto outcome = fixture.solver.Solve(snapshot, prepared.Value().request).Value();
        CHECK(fixture.pipeline->Commit(prepared.Value(), outcome, snapshot, fixture.Now(1)).HasError());
        outcome.desiredVelocity = {};
        outcome.disposition = static_cast<NavigationAvoidanceDisposition>(255);
        CHECK(fixture.pipeline->Commit(prepared.Value(), outcome, snapshot, fixture.Now(1)).HasError());
        outcome.disposition = NavigationAvoidanceDisposition::CollisionSafeStop;
        outcome.stopReason = static_cast<NavigationAvoidanceStopReason>(255);
        CHECK(fixture.pipeline->Commit(prepared.Value(), outcome, snapshot, fixture.Now(1)).HasError());
        outcome.stopReason = NavigationAvoidanceStopReason::None;
        CHECK(fixture.pipeline->Commit(prepared.Value(), outcome, snapshot, fixture.Now(1)).HasError());
        outcome.stopReason = NavigationAvoidanceStopReason::NoFeasibleSample;
        const auto stopped = fixture.pipeline->Commit(prepared.Value(), outcome, snapshot, fixture.Now(1));
        REQUIRE(stopped.HasValue());
        CHECK(stopped.Value().intent.desiredVelocity == Math::Vec3{});
        CHECK(stopped.Value().preferredVelocity.x == Catch::Approx(1));
        fixture.Feedback(stopped.Value(), true);
        CHECK(fixture.Publish(fixture.Capture(2)).intent.state == NavigationMovementState::Following);
    }

    TEST_CASE("Velocity operation replacement retires old feedback and pending work without changing Character incarnation",
              "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        fixture.PublishRoute();
        const auto first = fixture.Publish(fixture.Capture(1));
        fixture.Feedback(first, true);
        REQUIRE(fixture.movement
                    ->Start({.destination = {20, 0, 0}, .maximumQueries = 16, .query = Request().requirement, .capabilityRevision = 1},
                            fixture.Now(2))
                    .HasValue());
        CHECK(fixture.pipeline->ObserveFeedback({first.intent.operation, first.intent.path, 1, 1, {}, true}).HasError());
        const auto nextSnapshot = fixture.Capture(2);
        const auto pending = fixture.Prepare(nextSnapshot);
        CHECK(pending.preferred.operation != first.intent.operation);
        CHECK(pending.characterRevision == 1);
        REQUIRE(fixture.movement
                    ->Start({.destination = {30, 0, 0}, .maximumQueries = 16, .query = Request().requirement, .capabilityRevision = 1},
                            fixture.Now(3))
                    .HasValue());
        const auto replacementSnapshot = fixture.Capture(3);
        const auto replacement = fixture.Prepare(replacementSnapshot);
        CHECK(replacement.preferred.operation != pending.preferred.operation);
        CHECK(replacement.characterRevision == 1);
        CHECK(fixture.pipeline->Commit(pending, fixture.solver.Solve(nextSnapshot, pending.request).Value(), nextSnapshot, fixture.Now(2))
                  .HasError());
        const auto result = fixture.pipeline->Commit(replacement, fixture.solver.Solve(replacementSnapshot, replacement.request).Value(),
                                                     replacementSnapshot, fixture.Now(3));
        REQUIRE(result.HasValue());
        CHECK(result.Value().intent.desiredVelocity == Math::Vec3{});
    }

    TEST_CASE("Velocity admission and steady publication enforce finite bounds without tick allocations", "[unit][navigation][velocity]") {
        VelocityFixture fixture;
        CHECK(NavigationVelocityPipeline::Create(*fixture.movement, fixture.binding, fixture.agent, 0).HasError());
        CHECK(NavigationVelocityPipeline::Create(*fixture.movement, fixture.binding, fixture.agent, 1, {.blockedTicksBeforeRepath = 0})
                  .HasError());
        fixture.PublishRoute();
        // First corridor adoption consumes the prepared query result. Measure the
        // subsequent steady owner loop, not query completion/admission storage.
        const auto initial = fixture.Publish(fixture.Capture(1));
        fixture.Feedback(initial, false, {0.1F, 0, 0});
        const auto snapshot = fixture.Capture(2, {1, 0, 0});
        CHECK(fixture.pipeline->Prepare(fixture.Now(2), snapshot, {.agentIndex = 1, .stepSeconds = 0.1F}).HasError());
        CHECK(fixture.pipeline->Prepare(fixture.Now(2), snapshot, {.stepSeconds = 0}).HasError());
        std::optional<Result<NavigationVelocityWork>> prepared;
        std::optional<Result<NavigationVelocityPublication>> published;
        std::optional<Result<void>> feedback;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement window;
            prepared =
                fixture.pipeline->Prepare(fixture.Now(2), snapshot, {.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = 0.1F});
            if (prepared->HasValue()) {
                const auto sampled = fixture.solver.Solve(snapshot, prepared->Value().request);
                if (sampled.HasValue()) {
                    published = fixture.pipeline->Commit(prepared->Value(), sampled.Value(), snapshot, fixture.Now(2));
                    if (published->HasValue())
                        feedback = fixture.pipeline->ObserveFeedback(
                            {published->Value().intent.operation, published->Value().intent.path, 2, 1, {0.1F, 0, 0}, false});
                }
            }
            measured = window.Snapshot();
        }
        REQUIRE(prepared.has_value());
        REQUIRE(prepared->HasValue());
        REQUIRE(published.has_value());
        REQUIRE(published->HasValue());
        REQUIRE(feedback.has_value());
        REQUIRE(feedback->HasValue());
        CHECK(measured.requests == 0);
        CHECK(measured.requestedBytes == 0);
    }
}  // namespace Horo::Navigation
