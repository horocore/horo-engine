#pragma once

#include "Horo/Navigation/NavigationCrowdAvoidance.h"

namespace Horo::Navigation::Detail {
    /** @brief Validates speed, acceleration, horizon and equality to the owner's fixed step. */
    [[nodiscard]] bool ValidCrowdEnvelope(const NavigationAvoidanceRequest &request, float step) noexcept;
    /** @brief Validates a worker sample against its captured finite planar speed and acceleration envelope. */
    [[nodiscard]] bool ValidCrowdOutcome(const NavigationAvoidanceOutcome &outcome, const NavigationAvoidanceRequest &request,
                                         Math::Vec3 capturedVelocity) noexcept;
}  // namespace Horo::Navigation::Detail
