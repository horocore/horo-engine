#pragma once

/** @file EditorInspectionDistance.h @brief Pure distance measurement over validated world bounds. */

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
}  // namespace Horo::Editor
