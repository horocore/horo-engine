#pragma once

/** @file NavigationCrowdScaleFixture.h
 * @brief Shared CrowdSmall topology, bounds and job synchronization for reference tests.
 */

#include "Horo/Navigation/NavigationCrowdBatch.h"
#include "navigation/NavigationRuntimeTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Navigation::CrowdQualification {
    using TestSupport::Activation;
    using TestSupport::Id;
    using Clock = std::chrono::steady_clock;

    // ADR-109 CrowdSmall and AvoidanceBalanced. Other bounds belong to this reference corpus.
    inline constexpr std::size_t CrowdAgents = 64;
    inline constexpr std::uint32_t FactsPerAgent = 16;
    inline constexpr std::uint64_t Steps = 32;
    inline constexpr float FixedStep = 1.0F / 60.0F;
    inline constexpr float MaximumSpeed = 2.0F;
    inline constexpr float Radius = 0.25F;

    enum class Layout {
        Crossings,
        Bottleneck,
        Formation,
        MixedPriorities
    };

    /** @brief Real logical registry and dynamic overlay retained for one isolated workload. */
    struct CrowdScene final {
        const NavigationWorldActivationDescriptor activation{Activation()};
        const NavigationAgentSceneBinding binding{activation.world, activation.scene, activation.sceneGeneration};
        NavigationAgentRegistry agents{NavigationAgentRegistry::Create({.maximumAgents = CrowdAgents}).Value()};
        // This authored layout has up to two obstacles and uses no modifier.
        static constexpr std::size_t OverlayBoxes = 2;
        NavigationDynamicRegistry dynamic{NavigationDynamicRegistry::Create({.maximumObstacles = OverlayBoxes,
                                                                             .maximumModifiers = 1,
                                                                             .maximumPendingCommands = OverlayBoxes,
                                                                             .maximumMutationsPerCommit = OverlayBoxes})
                                              .Value()};
        std::unique_ptr<NavigationAgentSceneCandidate> candidate;
        const NavigationAgentProfileId profile{Id<NavigationAgentProfileId>(1)};
        std::optional<CrowdAgentHandle> teleported;

        CrowdScene() {
            std::array<NavigationAgentDescriptor, CrowdAgents> descriptors;
            for (std::size_t slot = 0; slot < descriptors.size(); ++slot) {
                descriptors[slot] = {.owner = {activation.scene, static_cast<std::uint32_t>(slot + 1), 1},
                                     .profile = profile,
                                     .filter = Id<NavigationFilterId>(1),
                                     .radiusOverride = Radius};
            }
            candidate = agents.PrepareScene(binding, descriptors).Value();
            candidate->Publish();
        }

        /** @brief Two physical-width obstacle faces bound the entrance to the central lane. */
        void CommitOverlay(const Layout layout) {
            if (layout == Layout::Bottleneck) {
                for (std::size_t id = 1; id <= OverlayBoxes; ++id) {
                    const float side = id == 1 ? -1.0F : 1.0F;
                    REQUIRE(dynamic
                                .StageRegisterObstacle({
                                    .id = Id<NavigationObstacleId>(id),
                                    .provenance = {activation.world, activation.scene, activation.sceneGeneration,
                                                   Id<NavigationDynamicOwnerId>(id), Id<NavigationDynamicOwnerGeneration>(1),
                                                   Id<NavigationDynamicSourceRevision>(1)},
                                    .shape = NavigationDynamicBoxShape{{0, 0, side * 3.4F}, {0.25F, 1, 1.15F}},
                                    .layers = {1},
                                    .updateTick = 1,
                                })
                                .HasValue());
                }
            }
            REQUIRE(dynamic.CommitAtSafePoint(activation, 1).HasValue());
        }

        /** @brief Invalidates every registry identity after the solver jobs have drained. */
        void Close() {
            candidate->Shutdown();
            dynamic.BeginShutdown();
            CHECK(agents.ActiveBinding().HasError());
            CHECK(dynamic.IsShutdown());
        }
    };

    /** @brief Samples an authored fixed-tick arrangement without assuming a Character displacement result. */
    inline NavigationCrowdMotionSample Sample(const NavigationAgentRecord &agent, const Layout layout, const std::uint64_t tick) {
        using enum Layout;
        const auto slot = agent.owner.entityIndex - 1;
        const auto row = static_cast<float>(slot / 8) - 3.5F;
        const auto column = slot % 8;
        const auto travel = static_cast<float>(tick - 1) * FixedStep;
        if (layout == Formation)
            return {.handle = agent.handle, .position = {static_cast<float>(column) * 1.2F + travel, 0, row * 1.2F}};

        const float side = column < 4 ? -1.0F : 1.0F;
        const float spacing = layout == Bottleneck ? 0.8F : 2.4F;
        const float rightOfWay = side < 0 ? 0.9F : 0.1F;
        return {.handle = agent.handle,
                .position = {side * (1.05F + static_cast<float>(column % 4) * 1.2F - travel), 0, row * spacing},
                .avoidance = {.priority = layout == MixedPriorities ? rightOfWay : 0.5F}};
    }

    /** @brief Admits exact motion samples under the workload's bounded CrowdSmall profile. */
    inline NavigationCrowdSnapshot BuildCapture(const CrowdScene &scene, const NavigationAgentSnapshot &agents,
                                                std::span<const NavigationCrowdMotionSample> motions, const std::uint64_t tick,
                                                const NavigationCrowdQualification *qualified = nullptr) {
        const std::array profiles{NavigationCrowdProfileFacts{.profile = scene.profile,
                                                              .radiusMeters = Radius,
                                                              .neighborRadiusMeters = 1.8F,
                                                              .maximumNeighbors = FactsPerAgent,
                                                              .maximumBoundarySegments = FactsPerAgent}};
        NavigationCrowdSnapshotLimits limits{.maximumAgents = CrowdAgents,
                                             .maximumBoundarySegments = FactsPerAgent,
                                             .maximumCellEntries = CrowdAgents * 9,
                                             .maximumNeighborFacts = CrowdAgents * FactsPerAgent,
                                             .maximumBoundaryFacts = CrowdAgents * FactsPerAgent,
                                             .maximumPairChecks = CrowdAgents * (CrowdAgents - 1),
                                             .maximumBoundaryChecks = CrowdAgents * FactsPerAgent};
        if (qualified) {
            limits.mode = AvoidanceExecutionMode::DeterministicQualified;
            limits.deterministicStamp = {scene.activation.topology, qualified->topologyContent, qualified->profile};
        }
        auto result = BuildNavigationCrowdSnapshot(agents, scene.dynamic.Snapshot().Value(), motions, profiles, limits, tick);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Requests one 60 Hz bounded solve per captured agent. */
    inline std::vector<NavigationAvoidanceRequest> Requests(const NavigationCrowdSnapshot &capture, const Layout layout) {
        std::vector<NavigationAvoidanceRequest> requests;
        requests.reserve(capture.Agents().size());
        for (const auto &agent : capture.Agents()) {
            const float direction = layout == Layout::Formation || agent.position.x < 0 ? 1.0F : -1.0F;
            requests.push_back({.agentIndex = requests.size(),
                                .preferredVelocity = {direction * MaximumSpeed, 0, 0},
                                .maximumSpeedMetersPerSecond = MaximumSpeed,
                                .maximumAccelerationMetersPerSecondSquared = 20,
                                .stepSeconds = FixedStep});
        }
        return requests;
    }

    /** @brief Joins one test-owned batch at a bounded fence while leaving the production collection path nonblocking. */
    inline std::vector<NavigationCrowdBatchResult> Collect(NavigationCrowdBatch &batch, const NavigationCrowdSnapshot &capture,
                                                           std::uint64_t &elapsed) {
        const auto deadline = Clock::now() + std::chrono::seconds{10};
        do {
            auto result = batch.TryTake(capture, capture.CaptureTick(), &elapsed);
            REQUIRE(result.HasValue());
            if (result.Value())
                return *std::move(result).Value();
            std::this_thread::yield();
        } while (Clock::now() < deadline);
        FAIL("CrowdSmall batch failed to reach the test retirement fence");
        return {};
    }
}  // namespace Horo::Navigation::CrowdQualification
