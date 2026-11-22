#pragma once

/** @file EditorInspectionMath.h @brief Pure measurements over committed scene geometry and positions. */

#include "EditorInspectionCommandEndpoint.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    /** @brief Measures finite world AABBs. For an overlapping axis, both nearest points use the same shared coordinate. */
    inline InspectedDistance MeasureInspectionDistance(const InspectedBounds &reference, const InspectedBounds &target,
                                                       const double forwardAngleRadians) {
        InspectedDistance result{.object = target.object, .referenceObject = reference.object, .forwardAngleRadians = forwardAngleRadians};
        for (std::size_t axis = 0; axis < result.overlapDepth.size(); ++axis) {
            const double refMin = reference.minimum[axis];
            const double refMax = reference.maximum[axis];
            const double targetMin = target.minimum[axis];
            const double targetMax = target.maximum[axis];
            if (refMax < targetMin) {
                result.nearestReference[axis] = refMax;
                result.nearestTarget[axis] = targetMin;
            } else if (targetMax < refMin) {
                result.nearestReference[axis] = refMin;
                result.nearestTarget[axis] = targetMax;
            } else {
                const double shared = std::max(refMin, targetMin);
                result.nearestReference[axis] = shared;
                result.nearestTarget[axis] = shared;
                result.overlapDepth[axis] = std::min(refMax, targetMax) - shared;
            }
        }
        result.gap = std::hypot(result.nearestReference[0] - result.nearestTarget[0], result.nearestReference[1] - result.nearestTarget[1],
                                result.nearestReference[2] - result.nearestTarget[2]);
        return result;
    }

    /** @brief Measures a finite world position against a zero-origin grid with a positive finite step.
     * @details Midpoints round away from zero, matching the editor's translation convention.
     */
    inline InspectedGridOffset MeasureInspectionGridOffset(const std::uint64_t object, const std::array<double, 3> position,
                                                           const double gridStep) {
        InspectedGridOffset result{.object = object, .gridStep = gridStep, .position = position};
        for (std::size_t axis = 0; axis < position.size(); ++axis) {
            const double remainder = std::fmod(position[axis], gridStep);
            const double upperDistance = gridStep - std::abs(remainder);
            const double offset = std::abs(remainder) >= upperDistance ? std::copysign(upperDistance, position[axis]) : -remainder;
            result.offset[axis] = offset == 0 ? 0.0 : offset;
            result.snappedPosition[axis] = position[axis] + offset;
        }
        return result;
    }
}  // namespace Horo::Editor
