#pragma once

/** @file GuardCover.h
 * @brief Scene-owner composition for the guard example's cover selection.
 */
#include "Horo/Gameplay/TacticalCover.h"

namespace Horo::Examples {
    /** @brief Detached reachability and threat visibility measured for one captured cover point. */
    struct GuardCoverObservation final {
        Gameplay::TacticalCoverHandle cover;
        AI::QueryTestSample reachability;
        AI::QueryTestSample visibility;
    };

    /** @brief Accepted cover receipt and its canonical world destination. */
    struct GuardCoverSelection final {
        Gameplay::TacticalCoverReservation reservation;
        Math::WorldCoordinate64 destination;
    };

    /** @brief Selects the lowest-cost reachable, occluded and unoccupied candidate and reserves it.
     * @param cover Scene-owned cover inventory.
     * @param scene Current Scene publication on the owner lane.
     * @param guard Exact live guard that will own the reservation.
     * @param observations Current provider evidence for the captured cover points.
     * @param contentionStage Distinct scoring stage reserved for cover contention.
     * @return The reserved destination, empty when no candidate qualifies, or a typed stale/invalid result.
     * @details Equal Navigation costs prefer the lower authored cover ID. The caller releases the exact receipt
     * after movement failure, cancellation or departure. No layout or provider is discovered here.
     */
    [[nodiscard]] Result<std::optional<GuardCoverSelection>> TrySelectGuardCover(Gameplay::TacticalCoverRegistry &cover,
                                                                                 const Runtime::RuntimeSceneView &scene,
                                                                                 Runtime::EntityRef guard,
                                                                                 std::span<const GuardCoverObservation> observations,
                                                                                 AI::QueryStageId contentionStage);
}  // namespace Horo::Examples
