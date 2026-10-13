#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationCoordinator.h"
#include "Horo/Navigation/NavigationMovement.h"
#include "navigation/NavigationGroundedProviderFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    namespace {
        using namespace TestSupport;

        std::unique_ptr<INavigationQueryBackend> MakeProvider() {
            return MakeGroundedProvider();
        }
    }  // namespace

    TEST_CASE("Compatible path partitions retain real Detour corridors through owner publication",
              "[unit][navigation][coordinator][provider]") {
        using namespace TestSupport;
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        REQUIRE(world.Stage(Activation(), MakeProvider()).HasValue());
        REQUIRE(world.CommitAtSafePoint(Activation().scene, Activation().sceneGeneration).HasValue());
        JobSystem jobs{{.workerCount = 0}};
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, {})).Value();
        const NavigationPathCaller callers[]{{NavigationDynamicOwnerId::Create(1).Value(),
                                              NavigationDynamicOwnerGeneration::Create(1).Value()},
                                             {NavigationDynamicOwnerId::Create(2).Value(),
                                              NavigationDynamicOwnerGeneration::Create(1).Value()}};
        const NavigationOutcomeProvenance source{NavigationSnapshotToken::Create(1).Value(), World(), Topology(), 1, 1, 1, 1, 0};
        std::array<NavRequestHandle, 2> handles;
        for (std::size_t index = 0; index < handles.size(); ++index) {
            auto request = Request();
            request.start = {1, 0, 1};
            request.destination = {9, 0, 9};
            auto accepted = coordinator.Submit(world,
                                               {.caller = callers[index],
                                                .request = request,
                                                .source = source,
                                                .capabilityRevision = 1,
                                                .targetTick = 1,
                                                .deadlineTick = 10},
                                               0);
            REQUIRE(accepted.HasValue());
            handles[index] = accepted.Value();
        }
        REQUIRE(coordinator.Dispatch(1) == 2);
        REQUIRE(coordinator.Commit({Activation(), source, callers, 1}) == 0);
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(coordinator.Commit({Activation(), source, callers, 1}) == 2);
        JobId partition{};
        for (const auto handle : handles) {
            auto result = coordinator.Take(handle);
            REQUIRE(result);
            REQUIRE(result->result.HasValue());
            REQUIRE(result->result.Value().status == NavigationPathStatus::Reachable);
            REQUIRE_FALSE(result->result.Value().corridor.empty());
            REQUIRE_FALSE(result->result.Value().waypoints.empty());
            REQUIRE(result->result.Value().sourceGeneration == Topology());
            if (partition)
                REQUIRE(result->job == partition);
            partition = result->job;
            REQUIRE(partition != 0);
        }
        REQUIRE(coordinator.IsDrained());
    }

    TEST_CASE("Move-to consumes real Detour waypoints and requires Character acknowledgement for arrival",
              "[unit][navigation][movement][provider]") {
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        REQUIRE(world.Stage(Activation(), MakeProvider()).HasValue());
        REQUIRE(world.CommitAtSafePoint(Activation().scene, Activation().sceneGeneration).HasValue());
        JobSystem jobs{{.workerCount = 0}};
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, {})).Value();
        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 1})).Value();
        const NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
        const NavigationAgentDescriptor descriptor{.owner = {binding.scene, 1, 1},
                                                   .profile = NavigationAgentProfileId::Create(1).Value(),
                                                   .filter = Request().filter};
        auto candidate = std::move(registry.PrepareScene(binding, std::array{descriptor})).Value();
        candidate->Publish();
        const auto agent = registry.Snapshot().Value().Agents().front().handle;
        const NavigationPathCaller callers[]{
            {NavigationDynamicOwnerId::Create(1).Value(), NavigationDynamicOwnerGeneration::Create(1).Value()}};
        auto created = NavigationMovementController::Create(registry, world, coordinator, agent, callers[0]);
        REQUIRE(created.HasValue());
        auto movement = std::move(created).Value();
        const NavigationOutcomeProvenance source{NavigationSnapshotToken::Create(1).Value(), World(), Topology(), 1, 1, 1, 1, 0};
        NavigationMovementObservation now{.binding = binding,
                                          .position = {1, 0, 1},
                                          .stepSeconds = 0.1F,
                                          .path = {.provenance = source, .linkRevision = 1, .goalRevision = 1, .tick = 0}};
        const NavigationMovementRequest request{.destination = {9, 0, 9}, .query = Request().requirement, .capabilityRevision = 1};
        const auto accepted = movement->Start(request, now);
        REQUIRE(accepted.HasValue());
        REQUIRE(movement->Update(now).HasValue());
        REQUIRE(coordinator.Dispatch(0) == 1);
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(coordinator.Commit({Activation(), source, callers, 1}) == 1);
        now.path.tick = 1;
        const auto intent = movement->Update(now);
        REQUIRE(intent.HasValue());
        REQUIRE(intent.Value().state == NavigationMovementState::Following);
        REQUIRE(intent.Value().path.IsValid());
        REQUIRE(intent.Value().desiredVelocity.x > 0);
        REQUIRE(intent.Value().desiredVelocity.z > 0);
        now.position = request.destination;
        now.path.tick = 2;
        now.feedback = NavigationMovementFeedback{accepted.Value().accepted, intent.Value().path, 1, 0};
        const auto arrived = movement->Update(now);
        REQUIRE(arrived.HasValue());
        REQUIRE(arrived.Value().state == NavigationMovementState::Arrived);
        REQUIRE(arrived.Value().desiredVelocity == Math::Vec3{});
        REQUIRE(movement->IsDrained());
        REQUIRE(coordinator.IsDrained());
    }
}  // namespace Horo::Navigation
