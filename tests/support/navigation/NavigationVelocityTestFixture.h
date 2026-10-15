#pragma once

#include "Horo/Navigation/NavigationVelocityPipeline.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

namespace Horo::Navigation::VelocityTestSupport {
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
                return NavigationPathWaypoint{position, {.kind = kind, .polygon = {.world = request.world, .topology = request.topology}}};
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
}  // namespace Horo::Navigation::VelocityTestSupport
