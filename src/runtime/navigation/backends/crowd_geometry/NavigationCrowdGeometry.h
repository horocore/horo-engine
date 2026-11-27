#pragma once

#include "Horo/Navigation/NavigationCrowdAvoidance.h"

namespace Horo::Navigation::Detail {
    /** @brief Checks the conservative planar swept envelope of the sampled velocity against all admitted facts. */
    [[nodiscard]] bool IsAvoidanceCandidateClear(const NavigationCrowdSnapshot &snapshot, const NavigationCrowdAgentFact &agent,
                                                 const Math::Vec3 &candidate, float horizonSeconds) noexcept;
}  // namespace Horo::Navigation::Detail
