#pragma once

#include "Horo/Assets/PreFracturedSource.h"
#include "Horo/Foundation/CancellationToken.h"

#include <cstdint>

namespace Horo::Destruction::Detail {
    enum class IntersectionCheck : std::uint8_t {
        Clear,
        Intersecting,
        TooMuchWork,
        Cancelled
    };

    [[nodiscard]] IntersectionCheck CheckSelfIntersection(const Assets::PreFracturedSourceNode &node, std::uint64_t &remainingWork,
                                                          const CancellationToken &cancellation);
}  // namespace Horo::Destruction::Detail
