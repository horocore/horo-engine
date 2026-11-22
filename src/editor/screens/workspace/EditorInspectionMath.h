#pragma once

/** @file EditorInspectionMath.h @brief Pure measurements over committed scene geometry and positions. */

#include "EditorInspectionCommandEndpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

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

    /** @brief Intersects a normalized segment with a world-space triangle in either winding. */
    inline std::optional<InspectionSurfaceSample> MeasureInspectionTriangle(const InspectionSurfaceRay &ray, const std::array<double, 3> &a,
                                                                            const std::array<double, 3> &b,
                                                                            const std::array<double, 3> &c) {
        using Vec = std::array<double, 3>;
        const auto subtract = [](const Vec &lhs, const Vec &rhs) -> Vec {
            return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
        };
        const auto dot = [](const Vec &lhs, const Vec &rhs) {
            return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2];
        };
        const auto cross = [](const Vec &lhs, const Vec &rhs) -> Vec {
            return {lhs[1] * rhs[2] - lhs[2] * rhs[1], lhs[2] * rhs[0] - lhs[0] * rhs[2], lhs[0] * rhs[1] - lhs[1] * rhs[0]};
        };
        const Vec edge1 = subtract(b, a);
        const Vec edge2 = subtract(c, a);
        const Vec normal = cross(edge1, edge2);
        const double area = std::hypot(normal[0], normal[1], normal[2]);
        const Vec transverse = cross(ray.direction, edge2);
        const double determinant = dot(edge1, transverse);
        // Relative tolerance keeps the same grazing-angle rule at every mesh scale.
        if (std::abs(determinant) <= std::numeric_limits<double>::epsilon() * area)
            return std::nullopt;
        const Vec fromVertex = subtract(ray.origin, a);
        const double u = dot(fromVertex, transverse) / determinant;
        const Vec barycentric = cross(fromVertex, edge1);
        const double v = dot(ray.direction, barycentric) / determinant;
        if (u < 0.0 || v < 0.0 || u + v > 1.0)
            return std::nullopt;
        const double distance = dot(edge2, barycentric) / determinant;
        if (distance < 0.0 || distance > ray.maximumDistance)
            return std::nullopt;
        const Vec position{ray.origin[0] + ray.direction[0] * distance, ray.origin[1] + ray.direction[1] * distance,
                           ray.origin[2] + ray.direction[2] * distance};
        const Vec unitNormal{normal[0] / area, normal[1] / area, normal[2] / area};
        return InspectionSurfaceSample{distance, position, unitNormal, std::acos(std::clamp(unitNormal[1], -1.0, 1.0))};
    }

    enum class InspectionSurfaceScan {
        Complete,
        GeometryUnavailable,
        BudgetExceeded
    };

    /** @brief Tests an indexed mesh and keeps the sequence's nearest hit within the caller's triangle budget. */
    template <typename Project>
    InspectionSurfaceScan SampleInspectionTriangles(const std::span<const std::uint32_t> indices, const InspectionSurfaceRay &ray,
                                                    const Project &project, std::optional<InspectionSurfaceSample> &nearest,
                                                    std::size_t &remainingTriangles) {
        if (indices.empty() || indices.size() % 3 != 0)
            return InspectionSurfaceScan::GeometryUnavailable;
        const auto count = indices.size() / 3;
        if (count > remainingTriangles)
            return InspectionSurfaceScan::BudgetExceeded;
        remainingTriangles -= count;
        for (std::size_t index = 0; index < indices.size(); index += 3) {
            const std::array points{project(indices[index]), project(indices[index + 1]), project(indices[index + 2])};
            if (!std::ranges::all_of(points, [](const auto &point) {
                return point.has_value();
            }))
                return InspectionSurfaceScan::GeometryUnavailable;
            const auto hit = MeasureInspectionTriangle(ray, *points[0], *points[1], *points[2]);
            if (hit && (!nearest || hit->distance < nearest->distance))
                nearest = hit;
        }
        return InspectionSurfaceScan::Complete;
    }
}  // namespace Horo::Editor
