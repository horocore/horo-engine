#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationRuntimeTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;

        [[nodiscard]] NavigationAgentSceneBinding Binding() {
            return {.world = Id<NavigationWorldId>(7),
                    .scene = Id<NavigationSceneRuntimeId>(11),
                    .sceneGeneration = Id<NavigationSceneGeneration>(12)};
        }

        [[nodiscard]] NavigationAgentSnapshot Agents(const bool pair) {
            const auto binding = Binding();
            auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 4})).Value();
            const std::array
                descriptors{NavigationAgentDescriptor{.owner = {.scene = binding.scene, .entityIndex = 1, .entityGeneration = 1},
                                                      .profile = Id<NavigationAgentProfileId>(1),
                                                      .filter = Id<NavigationFilterId>(1),
                                                      .radiusOverride = 0.5F,
                                                      .enabled = true},
                            NavigationAgentDescriptor{.owner = {.scene = binding.scene, .entityIndex = 2, .entityGeneration = 1},
                                                      .profile = Id<NavigationAgentProfileId>(1),
                                                      .filter = Id<NavigationFilterId>(1),
                                                      .radiusOverride = 0.5F,
                                                      .enabled = true}};
            auto candidate = std::move(registry.PrepareScene(binding, std::span(descriptors).first(pair ? 2 : 1))).Value();
            candidate->Publish();
            return std::move(registry.Snapshot()).Value();
        }

        [[nodiscard]] NavigationDynamicRegistrySnapshot Dynamic(const bool boundary, const float boundaryX) {
            const auto binding = Binding();
            auto registry = std::move(NavigationDynamicRegistry::Create()).Value();
            if (boundary) {
                const NavigationObstacleDescriptor obstacle{.id = Id<NavigationObstacleId>(1),
                                                            .provenance = {.world = binding.world,
                                                                           .scene = binding.scene,
                                                                           .sceneGeneration = binding.sceneGeneration,
                                                                           .owner = Id<NavigationDynamicOwnerId>(1),
                                                                           .ownerGeneration = Id<NavigationDynamicOwnerGeneration>(1),
                                                                           .sourceRevision = Id<NavigationDynamicSourceRevision>(1)},
                                                            .shape = NavigationDynamicBoxShape{.center = {boundaryX, 0.0F, 0.0F},
                                                                                               .halfExtents = {0.5F, 1.0F, 0.5F}},
                                                            .layers = {1},
                                                            .updateTick = 1,
                                                            .enabled = true};
                REQUIRE(registry.StageRegisterObstacle(obstacle).HasValue());
            }
            REQUIRE(registry.CommitAtSafePoint(TestSupport::Activation(11, 12, 7), 1).HasValue());
            return std::move(registry.Snapshot()).Value();
        }

        [[nodiscard]] NavigationCrowdSnapshot Snapshot(const bool pair = false, const bool boundary = false,
                                                       const std::uint32_t maximumNeighbors = 16,
                                                       const AvoidanceExecutionMode mode = AvoidanceExecutionMode::BestEffortBounded,
                                                       const float neighborX = 2.0F, const float boundaryX = 2.0F,
                                                       const float currentVelocityX = 0.0F, const float avoidancePriority = 0.5F) {
            const auto agents = Agents(pair);
            const auto dynamic = Dynamic(boundary, boundaryX);
            std::array<NavigationCrowdMotionSample, 2> motions{};
            motions[0] = {.handle = agents.Agents()[0].handle, .position = {0.0F, 0.0F, 0.0F}, .velocity = {currentVelocityX, 0.0F, 0.0F}};
            motions[0].avoidance.priority = avoidancePriority;
            if (pair)
                motions[1] = {.handle = agents.Agents()[1].handle, .position = {neighborX, 0.0F, 0.0F}, .velocity = {0.0F, 0.0F, 0.0F}};
            const std::array profiles{NavigationCrowdProfileFacts{.profile = Id<NavigationAgentProfileId>(1),
                                                                  .radiusMeters = 0.5F,
                                                                  .neighborRadiusMeters = 4.0F,
                                                                  .maximumNeighbors = maximumNeighbors,
                                                                  .maximumBoundarySegments = 16,
                                                                  .obstacleLayers = {1}}};
            NavigationCrowdSnapshotLimits limits;
            limits.mode = mode;
            return BuildNavigationCrowdSnapshot(agents, dynamic, std::span(motions).first(pair ? 2 : 1), profiles, limits, 8).Value();
        }

        [[nodiscard]] NavigationAvoidanceRequest Request() {
            return {.agentIndex = 0,
                    .preferredVelocity = {2.0F, 0.0F, 0.0F},
                    .maximumSpeedMetersPerSecond = 3.0F,
                    .maximumAccelerationMetersPerSecondSquared = 20.0F,
                    .stepSeconds = 0.1F,
                    .horizonSeconds = 1.0F};
        }
    }  // namespace

    TEST_CASE("Detour crowd avoidance samples a finite bounded free-space velocity", "[unit][navigation][crowd][provider]") {
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        const auto snapshot = Snapshot();
        const auto result = provider.Value()->Solve(snapshot, Request());
        REQUIRE(result.HasValue());
        CHECK(result.Value().disposition == NavigationAvoidanceDisposition::Sampled);
        CHECK(result.Value().stopReason == NavigationAvoidanceStopReason::None);
        CHECK(std::isfinite(result.Value().desiredVelocity.x));
        CHECK(result.Value().desiredVelocity.y == 0.0F);
        CHECK(result.Value().desiredVelocity.x * result.Value().desiredVelocity.x +
                  result.Value().desiredVelocity.z * result.Value().desiredVelocity.z <=
              4.0001F);
        CHECK(result.Value().binding == snapshot.Binding());
        CHECK(result.Value().dynamicRevision == snapshot.DynamicRevision());
        CHECK(result.Value().captureTick == snapshot.CaptureTick());
    }

    TEST_CASE("Detour crowd avoidance never accepts a sample crossing admitted agent or boundary", "[unit][navigation][crowd][provider]") {
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        for (const auto [pair, boundary] : {std::pair{true, false}, std::pair{false, true}}) {
            const auto snapshot = Snapshot(pair, boundary);
            const auto result = provider.Value()->Solve(snapshot, Request());
            REQUIRE(result.HasValue());
            if (result.Value().disposition == NavigationAvoidanceDisposition::Sampled) {
                const auto &velocity = result.Value().desiredVelocity;
                CHECK(std::isfinite(velocity.x));
                CHECK(std::isfinite(velocity.z));
                CHECK(velocity.x * velocity.x + velocity.z * velocity.z <= 4.0001F);
            } else {
                CHECK(result.Value().desiredVelocity.x == 0.0F);
                CHECK(result.Value().stopReason == NavigationAvoidanceStopReason::NoFeasibleSample);
            }
        }
    }

    TEST_CASE("Detour crowd priority changes steering preference without bypassing admitted neighbors",
              "[unit][navigation][crowd][provider][policy]") {
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        const auto low = Snapshot(true, false, 16, AvoidanceExecutionMode::BestEffortBounded, 2.0F, 2.0F, 0.0F, 0.0F);
        const auto high = Snapshot(true, false, 16, AvoidanceExecutionMode::BestEffortBounded, 2.0F, 2.0F, 0.0F, 1.0F);
        REQUIRE(low.Agents()[0].neighborCount == 1);
        REQUIRE(high.Agents()[0].neighborCount == 1);
        const auto lowResult = provider.Value()->Solve(low, Request());
        const auto highResult = provider.Value()->Solve(high, Request());
        REQUIRE(lowResult.HasValue());
        REQUIRE(highResult.HasValue());
        CHECK(lowResult.Value().disposition == NavigationAvoidanceDisposition::Sampled);
        CHECK(highResult.Value().disposition == NavigationAvoidanceDisposition::Sampled);
        CHECK((highResult.Value().desiredVelocity.x != lowResult.Value().desiredVelocity.x ||
               highResult.Value().desiredVelocity.z != lowResult.Value().desiredVelocity.z));
        CHECK(highResult.Value().binding == lowResult.Value().binding);
        CHECK(highResult.Value().captureTick == lowResult.Value().captureTick);
    }

    TEST_CASE("Detour crowd avoidance makes overlap and acceleration infeasibility explicit", "[unit][navigation][crowd][provider]") {
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        const auto overlappingAgents = Snapshot(true, false, 16, AvoidanceExecutionMode::BestEffortBounded, 0.25F);
        const auto agentStop = provider.Value()->Solve(overlappingAgents, Request());
        REQUIRE(agentStop.HasValue());
        CHECK(agentStop.Value().disposition == NavigationAvoidanceDisposition::CollisionSafeStop);
        CHECK(agentStop.Value().stopReason == NavigationAvoidanceStopReason::NoFeasibleSample);
        CHECK(agentStop.Value().desiredVelocity.x == 0.0F);

        const auto overlappingBoundary = Snapshot(false, true, 16, AvoidanceExecutionMode::BestEffortBounded, 2.0F, 0.5F);
        const auto boundaryStop = provider.Value()->Solve(overlappingBoundary, Request());
        REQUIRE(boundaryStop.HasValue());
        CHECK(boundaryStop.Value().disposition == NavigationAvoidanceDisposition::CollisionSafeStop);
        CHECK(boundaryStop.Value().stopReason == NavigationAvoidanceStopReason::NoFeasibleSample);

        auto slow = Request();
        slow.maximumAccelerationMetersPerSecondSquared = 0.1F;
        const auto limited = provider.Value()->Solve(Snapshot(), slow);
        REQUIRE(limited.HasValue());
        if (limited.Value().disposition == NavigationAvoidanceDisposition::Sampled)
            CHECK(std::hypot(limited.Value().desiredVelocity.x, limited.Value().desiredVelocity.z) <= 0.01001F);
    }

    TEST_CASE("Detour crowd avoidance rejects malformed finite envelope and exposes incomplete snapshot fallback",
              "[unit][navigation][crowd][provider]") {
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        const auto snapshot = Snapshot();
        auto request = Request();
        request.horizonSeconds = std::numeric_limits<float>::quiet_NaN();
        CHECK(provider.Value()->Solve(snapshot, request).HasError());
        request = Request();
        request.agentIndex = 8;
        CHECK(provider.Value()->Solve(snapshot, request).HasError());
        request = Request();
        request.maximumAccelerationMetersPerSecondSquared = 0.0F;
        CHECK(provider.Value()->Solve(snapshot, request).HasError());
        const auto deterministic = Snapshot(false, false, 16, AvoidanceExecutionMode::DeterministicQualified);
        const auto stop = provider.Value()->Solve(deterministic, Request());
        REQUIRE(stop.HasValue());
        CHECK(stop.Value().disposition == NavigationAvoidanceDisposition::CollisionSafeStop);
        CHECK(stop.Value().stopReason == NavigationAvoidanceStopReason::SnapshotIncomplete);
        CHECK(stop.Value().desiredVelocity.x == 0.0F);
        CHECK(CreateRecastDetourCrowdBackend({.maximumNeighbors = 0}).HasError());
        auto narrowProvider = CreateRecastDetourCrowdBackend({.maximumNeighbors = 1, .maximumBoundarySegments = 1});
        REQUIRE(narrowProvider.HasValue());
        const auto overCapacity = narrowProvider.Value()->Solve(Snapshot(false, true), Request());
        REQUIRE(overCapacity.HasValue());
        CHECK(overCapacity.Value().disposition == NavigationAvoidanceDisposition::CollisionSafeStop);
        CHECK(overCapacity.Value().stopReason == NavigationAvoidanceStopReason::SnapshotIncomplete);
        const auto numerical =
            provider.Value()->Solve(Snapshot(false, false, 16, AvoidanceExecutionMode::BestEffortBounded, 2.0F, 2.0F, 200.0F), Request());
        REQUIRE(numerical.HasValue());
        CHECK(numerical.Value().disposition == NavigationAvoidanceDisposition::CollisionSafeStop);
        CHECK(numerical.Value().stopReason == NavigationAvoidanceStopReason::NumericalFailure);
    }

    TEST_CASE("Detour crowd avoidance shares one bounded native lease without racing snapshot ownership",
              "[unit][navigation][crowd][provider]") {
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        const auto snapshot = Snapshot(true, true);
        std::atomic<int> invalid{0};
        std::array<std::thread, 4> workers;
        for (auto &worker : workers) {
            worker = std::thread([&] {
                for (int iteration = 0; iteration < 100; ++iteration) {
                    const auto result = provider.Value()->Solve(snapshot, Request());
                    if (!result.HasValue() || !std::isfinite(result.Value().desiredVelocity.x) ||
                        !std::isfinite(result.Value().desiredVelocity.z))
                        invalid.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto &worker : workers)
            worker.join();
        CHECK(invalid.load(std::memory_order_relaxed) == 0);
    }
}  // namespace Horo::Navigation
