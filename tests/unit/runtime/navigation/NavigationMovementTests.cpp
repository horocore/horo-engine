#include "Horo/Navigation/NavigationMovement.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#include <catch2/catch_approx.hpp>
#include <limits>

namespace Horo::Navigation {
    namespace {
        using namespace CoordinatorTestSupport;

        enum class Answer {
            Reachable,
            Partial,
            Unreachable,
            Malformed,
            MissingStart,
            WrongStart,
            ForeignWaypoint,
            Unavailable
        };

        class MovementBackend final : public INavigationQueryBackend {
        public:
            explicit MovementBackend(const Answer answer, const std::uint32_t leases) : answer_(answer), leases_(leases) {}

            NavigationProviderCapabilities Capabilities() const noexcept override {
                return MakeAvailablePathQueryCapabilities(1, Request().requirement.limits, leases_);
            }

            Result<NavigationPath> FindPath(const NavigationPathRequest &request, const CancellationToken &) const override {
                if (answer_ == Answer::Unavailable)
                    return Result<NavigationPath>::Failure(MakeError(NavigationErrors::NoNavigationData));
                NavigationPath path;
                path.sourceGeneration = request.topology;
                path.status = answer_ == Answer::Partial       ? NavigationPathStatus::Partial
                              : answer_ == Answer::Unreachable ? NavigationPathStatus::Unreachable
                                                               : NavigationPathStatus::Reachable;
                const auto waypoint = [&](const Math::Vec3 position, const NavigationPathWaypointKind kind) {
                    return NavigationPathWaypoint{position,
                                                  {.kind = kind, .polygon = {.world = request.world, .topology = request.topology}}};
                };
                path.waypoints = {waypoint(request.start, NavigationPathWaypointKind::Start),
                                  waypoint(request.destination, NavigationPathWaypointKind::Destination)};
                if (answer_ == Answer::Malformed)
                    path.waypoints.back().position.x = std::numeric_limits<float>::quiet_NaN();
                if (answer_ == Answer::MissingStart)
                    path.waypoints.front().provenance.kind = NavigationPathWaypointKind::PortalCorner;
                if (answer_ == Answer::WrongStart)
                    path.waypoints.front().position.x += 1;
                if (answer_ == Answer::ForeignWaypoint)
                    path.waypoints.front().provenance.polygon.world = World(99);
                return Result<NavigationPath>::Success(std::move(path));
            }

        private:
            Answer answer_;
            std::uint32_t leases_;
        };

        struct Fixture final {
            JobSystem jobs{{.workerCount = 0}};
            NavigationWorldLifecycle world{std::move(NavigationWorldLifecycle::Create(4)).Value()};
            NavigationCoordinator coordinator{std::move(NavigationCoordinator::Create(jobs, BatchLimits())).Value()};
            NavigationAgentRegistry registry{std::move(NavigationAgentRegistry::Create({.maximumAgents = 4})).Value()};
            NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
            std::unique_ptr<NavigationAgentSceneCandidate> candidate;
            std::unique_ptr<NavigationMovementController> movement;
            CrowdAgentHandle agent;

            static NavigationPathBatchLimits BatchLimits() {
                auto limits = Limits();
                limits.requestSlots = 16;
                limits.maximumPendingPerCaller = 15;
                return limits;
            }

            explicit Fixture(const Answer answer = Answer::Reachable, const std::uint32_t leases = 1) {
                Activate(world, std::make_unique<MovementBackend>(answer, leases));
                const NavigationAgentDescriptor descriptor{.owner = {binding.scene, 1, 1},
                                                           .profile = NavigationAgentProfileId::Create(1).Value(),
                                                           .filter = Request().filter};
                candidate = std::move(registry.PrepareScene(binding, std::array{descriptor})).Value();
                candidate->Publish();
                agent = registry.Snapshot().Value().Agents().front().handle;
                auto created = NavigationMovementController::Create(registry, world, coordinator, agent, Caller());
                REQUIRE(created.HasValue());
                movement = std::move(created).Value();
            }

            NavigationMovementObservation Now(const std::uint64_t tick = 0) const {
                return {.binding = binding,
                        .position = {0, 0, 0},
                        .stepSeconds = 0.1F,
                        .path = {.provenance = Source(), .linkRevision = 1, .goalRevision = 1, .tick = tick}};
            }

            NavigationMovementRequest Move() const {
                return {.destination = {10, 0, 0}, .query = Request().requirement, .capabilityRevision = 1};
            }

            void Commit(const std::uint64_t tick) {
                const std::array callers{Caller()};
                static_cast<void>(coordinator.Commit(Current(callers, tick)));
            }

            NavigationMovementId Start(NavigationMovementRequest request, const NavigationMovementObservation &now) {
                const auto result = movement->Start(std::move(request), now);
                REQUIRE(result.HasValue());
                return result.Value().accepted;
            }

            NavigationMovementIntent Update(const NavigationMovementObservation &now) {
                const auto result = movement->Update(now);
                REQUIRE(result.HasValue());
                return result.Value();
            }

            NavigationMovementIntent Publish() {
                REQUIRE(coordinator.Dispatch(0) == 1);
                jobs.Shutdown(ShutdownPolicy::Drain);
                Commit(1);
                return Update(Now(1));
            }
        };
    }  // namespace

    TEST_CASE("Movement repath feedback requires the latest exact operation path and tick", "[unit][navigation][movement]") {
        Fixture fixture;
        const auto id = fixture.Start(fixture.Move(), fixture.Now());
        REQUIRE(fixture.Update(fixture.Now()).state == NavigationMovementState::Querying);
        auto intent = fixture.Publish();
        auto current = fixture.Now(2);
        current.feedback = NavigationMovementFeedback{{id.agent, id.sequence + 1}, intent.path, 1, 0, {}, true};
        intent = fixture.Update(current);
        CHECK(intent.state == NavigationMovementState::Following);
        current.path.tick = 3;
        current.feedback = NavigationMovementFeedback{id, intent.path, 2, 0, {}, true};
        intent = fixture.Update(current);
        CHECK(intent.state == NavigationMovementState::Querying);
        CHECK(intent.path == PathId{});
        CHECK(intent.desiredVelocity == Math::Vec3{});
    }

    TEST_CASE("Movement motion reset validates before clearing corridor and old arrival evidence", "[unit][navigation][movement]") {
        Fixture fixture;
        const auto id = fixture.Start(fixture.Move(), fixture.Now());
        REQUIRE(fixture.Update(fixture.Now()).state == NavigationMovementState::Querying);
        const auto intent = fixture.Publish();
        auto invalid = fixture.Now(2);
        invalid.stepSeconds = 0;
        CHECK(fixture.movement->ResetMotionAtSafePoint(invalid).HasError());
        CHECK(fixture.movement->IsCurrentIntent(intent));
        auto reset = fixture.Now(2);
        reset.position = {10, 0, 0};
        REQUIRE(fixture.movement->ResetMotionAtSafePoint(reset).HasValue());
        CHECK_FALSE(fixture.movement->IsCurrentIntent(intent));
        reset.feedback = NavigationMovementFeedback{id, intent.path, 1, 0};
        const auto next = fixture.Update(reset);
        CHECK(next.state == NavigationMovementState::Querying);
        CHECK(next.desiredVelocity == Math::Vec3{});
    }

    TEST_CASE("Movement arrival requires a current path and exact achieved motion acknowledgement", "[unit][navigation][movement]") {
        Fixture fixture;
        const auto id = fixture.Start(fixture.Move(), fixture.Now());
        auto premature = fixture.Now();
        premature.position = {10, 0, 0};
        REQUIRE(fixture.Update(premature).path == PathId{});
        REQUIRE(fixture.movement->Status().state == NavigationMovementState::Querying);
        auto intent = fixture.Publish();
        REQUIRE(intent.path.IsValid());
        REQUIRE(intent.desiredVelocity.x == Catch::Approx(1.0F));
        auto atGoal = fixture.Now(2);
        atGoal.position = {10, 0, 0};
        atGoal.feedback = NavigationMovementFeedback{id, intent.path, 1, 2.0F};
        intent = fixture.Update(atGoal);
        REQUIRE(intent.state == NavigationMovementState::Following);
        REQUIRE(intent.desiredVelocity == Math::Vec3{});
        atGoal.path.tick = 3;
        atGoal.feedback = NavigationMovementFeedback{{id.agent, id.sequence + 1}, intent.path, 2, 0};
        intent = fixture.Update(atGoal);
        REQUIRE(intent.state == NavigationMovementState::Following);
        atGoal.path.tick = 4;
        atGoal.feedback = NavigationMovementFeedback{id, intent.path, 3, 0};
        REQUIRE(fixture.Update(atGoal).state == NavigationMovementState::Arrived);
        REQUIRE(fixture.movement->IsDrained());
        REQUIRE_FALSE(fixture.movement->Cancel(id));
    }

    TEST_CASE("Movement replacement reports supersession and discards old completions", "[unit][navigation][movement]") {
        for (const std::uint32_t leases : {1U, 2U}) {
            Fixture fixture{Answer::Reachable, leases};
            const auto first = fixture.Start(fixture.Move(), fixture.Now());
            fixture.Update(fixture.Now());
            REQUIRE(fixture.coordinator.Dispatch(0) == 1);  // Old work really owns a provider partition.
            auto replacement = fixture.Move();
            replacement.destination = {0, 0, 10};
            auto accepted = fixture.movement->Start(replacement, fixture.Now());
            REQUIRE(accepted.HasValue());
            REQUIRE(accepted.Value().superseded);
            REQUIRE(accepted.Value().superseded->id == first);
            REQUIRE(accepted.Value().superseded->state == NavigationMovementState::Superseded);
            REQUIRE_FALSE(fixture.movement->Cancel(first));
            fixture.Commit(1);
            const auto pending = fixture.Update(fixture.Now(1));
            REQUIRE(pending.operation == accepted.Value().accepted);
            REQUIRE_FALSE(pending.path.IsValid());
            REQUIRE(pending.desiredVelocity == Math::Vec3{});
            // Logical cancellation does not release the old partition's physical scratch lease.
            REQUIRE(fixture.coordinator.Dispatch(1) == (leases == 1 ? 0U : 1U));
            if (leases == 1)
                REQUIRE(fixture.movement->Cancel(accepted.Value().accepted));
            fixture.jobs.Shutdown(ShutdownPolicy::Drain);
            fixture.Commit(2);
            const auto intent = fixture.Update(fixture.Now(2));
            REQUIRE(intent.operation == accepted.Value().accepted);
            REQUIRE(intent.desiredVelocity.x == 0);
            if (leases == 1) {
                REQUIRE(intent.state == NavigationMovementState::Cancelled);
                REQUIRE(intent.desiredVelocity == Math::Vec3{});
            } else {
                REQUIRE(intent.state == NavigationMovementState::Following);
                REQUIRE(intent.desiredVelocity.z == Catch::Approx(1));
            }
            fixture.movement->BeginShutdown();
            REQUIRE(fixture.movement->IsDrained());
            REQUIRE(fixture.coordinator.IsDrained());
        }
    }

    TEST_CASE("Follow holds near a live target and coalesces changed targets into bounded repath", "[unit][navigation][movement]") {
        Fixture fixture;
        auto request = fixture.Move();
        request.mode = NavigationMovementMode::Follow;
        request.entity = NavigationAgentOwner{fixture.binding.scene, 4, 3};
        auto now = fixture.Now();
        now.target = NavigationMovementTarget{*request.entity, {10, 0, 0}, 1};
        const auto id = fixture.Start(request, now);
        fixture.Update(now);
        REQUIRE(fixture.coordinator.Dispatch(0) == 1);
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        fixture.Commit(1);
        now.path.tick = 1;
        auto intent = fixture.Update(now);
        now.path.tick = 2;
        now.position = {10, 0, 0};
        now.feedback = NavigationMovementFeedback{id, intent.path, 1, 0};
        intent = fixture.Update(now);
        REQUIRE(intent.state == NavigationMovementState::Holding);
        REQUIRE(intent.desiredVelocity == Math::Vec3{});
        now.path.tick = 3;
        now.target->position = {20, 0, 0};
        now.target->revision = 2;
        intent = fixture.Update(now);
        REQUIRE_FALSE(intent.path.IsValid());
        REQUIRE(intent.desiredVelocity == Math::Vec3{});
        now.path.tick = 4;
        now.target->position = {30, 0, 0};
        now.target->revision = 3;
        REQUIRE(fixture.Update(now).desiredVelocity == Math::Vec3{});
        now.path.tick = 5;
        REQUIRE(fixture.Update(now).desiredVelocity == Math::Vec3{});
        now.path.tick = 6;
        REQUIRE(fixture.Update(now).state == NavigationMovementState::Querying);
        REQUIRE_FALSE(fixture.movement->IsDrained());
        REQUIRE(fixture.movement->Cancel(id));
        fixture.Commit(7);
        now.path.tick = 7;
        REQUIRE(fixture.Update(now).state == NavigationMovementState::Cancelled);
        REQUIRE(fixture.movement->IsDrained());
    }

    TEST_CASE("Destroyed or reused target entities terminate before an old query can authorize motion", "[unit][navigation][movement]") {
        for (const bool reuse : {false, true}) {
            Fixture fixture;
            auto request = fixture.Move();
            request.entity = NavigationAgentOwner{fixture.binding.scene, 4, 3};
            auto now = fixture.Now();
            now.target = NavigationMovementTarget{*request.entity, {10, 0, 0}, 1};
            fixture.Start(request, now);
            fixture.Update(now);
            if (reuse)
                ++now.target->owner.entityGeneration;
            else
                now.target.reset();
            now.path.tick = 1;
            REQUIRE(fixture.Update(now).state == NavigationMovementState::TargetLost);
            fixture.Commit(2);
            now.path.tick = 2;
            fixture.Update(now);
            REQUIRE(fixture.movement->IsDrained());
            REQUIRE(fixture.coordinator.IsDrained());
        }
    }

    TEST_CASE("Agent removal Scene replacement and navigation unload revoke movement authority", "[unit][navigation][movement]") {
        for (int cause = 0; cause < 3; ++cause) {
            Fixture fixture;
            fixture.Start(fixture.Move(), fixture.Now());
            fixture.Update(fixture.Now());
            auto now = fixture.Now(1);
            if (cause == 0)
                REQUIRE(fixture.registry.UnregisterAtSafePoint(fixture.agent).HasValue());
            else if (cause == 1)
                now.binding.sceneGeneration = NavigationSceneGeneration::Create(now.binding.sceneGeneration.Value() + 1).Value();
            else
                fixture.world.BeginShutdown();
            const auto intent = fixture.Update(now);
            REQUIRE(intent.desiredVelocity == Math::Vec3{});
            REQUIRE(intent.state == (cause == 0 ? NavigationMovementState::AgentLost : NavigationMovementState::WorldLost));
            fixture.Commit(2);
            now.path.tick = 2;
            fixture.Update(now);
            REQUIRE(fixture.movement->IsDrained());
        }
    }

    TEST_CASE("Timeout and shutdown cancel accepted queries and drain result reservations", "[unit][navigation][movement]") {
        for (const bool shutdown : {false, true}) {
            Fixture fixture;
            auto request = fixture.Move();
            request.timeoutTicks = 2;
            const auto id = fixture.Start(request, fixture.Now());
            fixture.Update(fixture.Now());
            if (shutdown)
                fixture.movement->BeginShutdown();
            REQUIRE(fixture.Update(fixture.Now(2)).state ==
                    (shutdown ? NavigationMovementState::Cancelled : NavigationMovementState::TimedOut));
            REQUIRE_FALSE(fixture.movement->Cancel(id));
            if (shutdown)
                REQUIRE(fixture.movement->Start(request, fixture.Now(2)).HasError());
            fixture.Commit(3);
            fixture.Update(fixture.Now(3));
            REQUIRE(fixture.movement->IsDrained());
            REQUIRE(fixture.coordinator.IsDrained());
        }
    }

    TEST_CASE("Replacement pressure leaves the old operation intact until canceled query storage drains", "[unit][navigation][movement]") {
        Fixture fixture;
        NavigationMovementId previous;
        for (std::uint64_t tick = 0; tick < 8; ++tick) {
            previous = fixture.Start(fixture.Move(), fixture.Now(tick));
            fixture.Update(fixture.Now(tick));
        }
        REQUIRE(fixture.movement->Start(fixture.Move(), fixture.Now(8)).HasError());
        REQUIRE(fixture.movement->Status().id == previous);
        REQUIRE(fixture.movement->Cancel(previous));
        fixture.Commit(9);
        fixture.Update(fixture.Now(9));
        REQUIRE(fixture.movement->IsDrained());
        REQUIRE(fixture.coordinator.IsDrained());
        REQUIRE(fixture.movement->Start(fixture.Move(), fixture.Now(9)).HasValue());
    }

    TEST_CASE("Malformed movement policy is rejected without replacing current operation", "[unit][navigation][movement]") {
        Fixture fixture;
        const auto id = fixture.Start(fixture.Move(), fixture.Now());
        for (int fault = 0; fault < 6; ++fault) {
            auto request = fixture.Move();
            if (fault == 0)
                request.acceptanceRadiusMeters = -1;
            if (fault == 1)
                request.timeoutTicks = 0;
            if (fault == 2)
                request.maximumQueries = 0;
            if (fault == 3)
                request.mode = NavigationMovementMode::Follow;
            if (fault == 4)
                request.capabilityRevision = 2;
            if (fault == 5)
                request.speedMetersPerSecond = std::numeric_limits<float>::infinity();
            REQUIRE(fixture.movement->Start(request, fixture.Now()).HasError());
            REQUIRE(fixture.movement->Status().id == id);
        }
        auto now = fixture.Now(std::numeric_limits<std::uint64_t>::max());
        REQUIRE(fixture.movement->Start(fixture.Move(), now).HasError());
    }

    TEST_CASE("Partial no-path malformed and Null-like results cannot report arrival", "[unit][navigation][movement]") {
        for (const auto answer : {Answer::Partial, Answer::Unreachable, Answer::Malformed, Answer::MissingStart, Answer::WrongStart,
                                  Answer::ForeignWaypoint, Answer::Unavailable}) {
            Fixture fixture{answer};
            fixture.Start(fixture.Move(), fixture.Now());
            fixture.Update(fixture.Now());
            const auto intent = fixture.Publish();
            REQUIRE_FALSE(intent.path.IsValid());
            REQUIRE(intent.desiredVelocity == Math::Vec3{});
            REQUIRE(intent.state ==
                    (answer == Answer::Unreachable ? NavigationMovementState::Unreachable : NavigationMovementState::Failed));
            if (answer == Answer::Unavailable) {
                REQUIRE(fixture.movement->Status().error);
                REQUIRE(fixture.movement->Status().error->code.Value() == MakeError(NavigationErrors::NoNavigationData).code.Value());
            }
            REQUIRE(fixture.movement->IsDrained());
        }
    }

    TEST_CASE("Changed world revisions stop held corridors and finite query budgets end retries", "[unit][navigation][movement]") {
        Fixture fixture;
        auto request = fixture.Move();
        request.maximumQueries = 1;
        request.repath = {};
        fixture.Start(request, fixture.Now());
        fixture.Update(fixture.Now());
        REQUIRE(fixture.Publish().path.IsValid());
        auto now = fixture.Now(2);
        now.path.provenance.topology = Topology(now.path.provenance.topology.Value() + 1);
        const auto intent = fixture.Update(now);
        REQUIRE(intent.state == NavigationMovementState::Failed);
        REQUIRE(intent.desiredVelocity == Math::Vec3{});
        REQUIRE(fixture.movement->Status().error);
        REQUIRE(fixture.movement->Status().error->code.Value() == MakeError(NavigationErrors::QueryLimitExceeded).code.Value());
    }

    TEST_CASE("Rollback duplicate ticks and unrevisioned target motion cannot refresh movement authority", "[unit][navigation][movement]") {
        Fixture fixture;
        auto request = fixture.Move();
        request.entity = NavigationAgentOwner{fixture.binding.scene, 4, 3};
        auto now = fixture.Now();
        now.target = NavigationMovementTarget{*request.entity, {10, 0, 0}, 2};
        fixture.Start(request, now);
        fixture.Update(now);
        REQUIRE(fixture.movement->Update(now).HasError());
        now.path.tick = 1;
        now.target->revision = 1;
        REQUIRE(fixture.Update(now).state == NavigationMovementState::Failed);
        REQUIRE(fixture.movement->Status().error);
    }

    TEST_CASE("Movement binding and acceptance reject stale authority and invalid observations atomically",
              "[unit][navigation][movement]") {
        Fixture fixture;
        REQUIRE(NavigationMovementController::Create(fixture.registry, fixture.world, fixture.coordinator, fixture.agent, {}).HasError());
        auto stale = fixture.agent;
        ++stale.slot.generation;
        REQUIRE(NavigationMovementController::Create(fixture.registry, fixture.world, fixture.coordinator, stale, Caller()).HasError());
        const auto id = fixture.Start(fixture.Move(), fixture.Now());
        for (int fault = 0; fault < 5; ++fault) {
            auto now = fixture.Now();
            if (fault == 0)
                now.stepSeconds = 0;
            if (fault == 1)
                now.path.provenance.filterRevision = 0;
            if (fault == 2)
                now.binding.sceneGeneration = NavigationSceneGeneration::Create(99).Value();
            if (fault == 3)
                now.position.x = std::numeric_limits<float>::quiet_NaN();
            if (fault == 4)
                now.feedback = NavigationMovementFeedback{id, {}, 0, -1};
            REQUIRE(fixture.movement->Start(fixture.Move(), now).HasError());
            REQUIRE(fixture.movement->Status().id == id);
            REQUIRE(fixture.movement->IsDrained());
        }
        auto entity = fixture.Move();
        entity.entity = NavigationAgentOwner{fixture.binding.scene, 2, 1};
        REQUIRE(fixture.movement->Start(entity, fixture.Now()).HasError());
        fixture.world.BeginShutdown();
        REQUIRE(fixture.movement->Start(fixture.Move(), fixture.Now()).HasError());
        REQUIRE(fixture.movement->Status().id == id);
    }

    TEST_CASE("Old path and tick acknowledgements cannot satisfy arrival for a live corridor", "[unit][navigation][movement]") {
        Fixture fixture;
        const auto id = fixture.Start(fixture.Move(), fixture.Now());
        fixture.Update(fixture.Now());
        auto intent = fixture.Publish();
        auto now = fixture.Now(2);
        now.position = fixture.Move().destination;
        auto wrongPath = intent.path;
        ++wrongPath.slot.generation;
        now.feedback = NavigationMovementFeedback{id, wrongPath, 1, 0};
        intent = fixture.Update(now);
        REQUIRE(intent.state == NavigationMovementState::Following);
        now.path.tick = 3;
        now.feedback = NavigationMovementFeedback{id, intent.path, 1, 0};
        intent = fixture.Update(now);
        REQUIRE(intent.state == NavigationMovementState::Following);
        now.path.tick = 4;
        now.feedback = NavigationMovementFeedback{id, intent.path, 3, 0};
        REQUIRE(fixture.Update(now).state == NavigationMovementState::Arrived);
    }
}  // namespace Horo::Navigation
