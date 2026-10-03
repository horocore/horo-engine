#pragma once

/**
 * @file NavigationCrowdAvoidance.h
 * @brief Provider-neutral, bounded best-effort local-avoidance request and outcome.
 */

#include "Horo/Navigation/NavigationCrowdSnapshot.h"

#include <cstddef>
#include <cstdint>

namespace Horo::Navigation {
    /** @brief Solver disposition; a stop is a desired command, not a collision-free movement guarantee. */
    enum class NavigationAvoidanceDisposition : std::uint8_t {
        Sampled,
        CollisionSafeStop,
    };

    /** @brief Typed reason for an explicit stop rather than an accepted sampled velocity. */
    enum class NavigationAvoidanceStopReason : std::uint8_t {
        None,
        SnapshotIncomplete,
        CapacityBusy,
        NumericalFailure,
        NoFeasibleSample,
    };

    /** @brief One agent's Horo-owned preferred-velocity request over an immutable snapshot. */
    struct NavigationAvoidanceRequest final {
        std::size_t agentIndex{};                              /**< Index in snapshot.Agents(). */
        Math::Vec3 preferredVelocity{};                        /**< Finite planar desired velocity in metres per second. */
        float maximumSpeedMetersPerSecond{3.5F};               /**< Finite positive horizontal speed ceiling. */
        float maximumAccelerationMetersPerSecondSquared{8.0F}; /**< Finite positive change-of-velocity ceiling. */
        float stepSeconds{0.05F};                              /**< Finite positive owner tick duration. */
        float horizonSeconds{2.0F};                            /**< Finite positive predicted avoidance horizon. */
    };

    /** @brief One finite planar desired velocity with exact source-publication evidence. */
    struct NavigationAvoidanceOutcome final {
        Math::Vec3 desiredVelocity{}; /**< Finite speed-bounded command; zero on fallback. */
        NavigationAvoidanceDisposition disposition{NavigationAvoidanceDisposition::CollisionSafeStop};
        NavigationAvoidanceStopReason stopReason{NavigationAvoidanceStopReason::NoFeasibleSample};
        NavigationAgentSceneBinding binding;
        NavigationDynamicRegistryRevision dynamicRevision;
        std::uint64_t captureTick{};
    };

    /** @brief Optional provider selected by host composition before scene activation; it cannot mutate the snapshot. */
    class INavigationCrowdBackend {
    public:
        virtual ~INavigationCrowdBackend() = default;

        /**
         * @brief Samples one best-effort planar velocity from snapshot-owned neighbors and boundaries.
         * @param snapshot Complete immutable capture pinned by the caller throughout the call.
         * @param request Finite speed, acceleration, step, horizon, and preferred velocity envelope.
         * @return Sample or explicit safe-stop; malformed requests fail without a partial outcome.
         * @note The caller must reject stale binding/revision/tick before publication; Character/Physics owns achieved movement.
         */
        [[nodiscard]] virtual Result<NavigationAvoidanceOutcome> Solve(const NavigationCrowdSnapshot &snapshot,
                                                                       const NavigationAvoidanceRequest &request) = 0;
    };
}  // namespace Horo::Navigation
