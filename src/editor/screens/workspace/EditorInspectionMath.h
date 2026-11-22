#pragma once

/** @file EditorInspectionMath.h @brief Pure measurements over committed scene geometry and positions. */

#include "EditorInspectionCommandEndpoint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace Horo::Editor {
    using InspectionVector = std::array<double, 3>;

    /** @brief Scalar projection in inspection's double-precision coordinate space. */
    inline double InspectionDot(const InspectionVector &lhs, const InspectionVector &rhs) {
        return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2];
    }

    /** @brief Right-handed cross product in inspection's double-precision coordinate space. */
    inline InspectionVector InspectionCross(const InspectionVector &lhs, const InspectionVector &rhs) {
        return {lhs[1] * rhs[2] - lhs[2] * rhs[1], lhs[2] * rhs[0] - lhs[0] * rhs[2], lhs[0] * rhs[1] - lhs[1] * rhs[0]};
    }

    /** @brief Encloses a local AABB in a right-handed frame built from the first two affine columns.
     * @details Projections include every local column, retaining containment under shear and reflection.
     */
    inline std::optional<InspectedOrientedBounds> MeasureInspectionOrientedBounds(const InspectionVector &minimum,
                                                                                  const InspectionVector &maximum,
                                                                                  const std::array<InspectionVector, 3> &columns,
                                                                                  const InspectionVector &translation) {
        const auto normal = InspectionCross(columns[0], columns[1]);
        const double width = std::hypot(columns[0][0], columns[0][1], columns[0][2]);
        const double area = std::hypot(normal[0], normal[1], normal[2]);
        if (width == 0.0 || area == 0.0 || !std::isfinite(width) || !std::isfinite(area))
            return std::nullopt;
        InspectedOrientedBounds result{.center = translation};
        for (std::size_t axis = 0; axis < translation.size(); ++axis) {
            result.axes[0][axis] = columns[0][axis] / width;
            result.axes[2][axis] = normal[axis] / area;
        }
        result.axes[1] = InspectionCross(result.axes[2], result.axes[0]);
        for (std::size_t column = 0; column < columns.size(); ++column) {
            const double half = (maximum[column] - minimum[column]) * 0.5;
            const double midpoint = minimum[column] + half;
            for (std::size_t axis = 0; axis < translation.size(); ++axis) {
                result.center[axis] += columns[column][axis] * midpoint;
                result.halfExtents[axis] += std::abs(InspectionDot(result.axes[axis], columns[column])) * half;
            }
        }
        result.volume = 8.0 * result.halfExtents[0] * result.halfExtents[1] * result.halfExtents[2];
        return result;
    }

    using InspectionClipPoint = std::array<double, 4>;

    /** @brief Intersects a homogeneous box edge with the canonical zero-to-one clip-depth interval. */
    inline std::optional<std::array<double, 2>> ClipInspectionEdge(const InspectionClipPoint &from, const InspectionClipPoint &to) {
        std::array<double, 2> interval{0.0, 1.0};
        const std::array fromDepth{from[2], from[3] - from[2]};
        const std::array toDepth{to[2], to[3] - to[2]};
        for (std::size_t plane = 0; plane < fromDepth.size(); ++plane) {
            const double start = fromDepth[plane];
            const double end = toDepth[plane];
            if (start < 0.0 && end < 0.0)
                return std::nullopt;
            if (start < 0.0)
                interval[0] = std::max(interval[0], start / (start - end));
            else if (end < 0.0)
                interval[1] = std::min(interval[1], start / (start - end));
        }
        return interval[0] <= interval[1] ? std::optional{interval} : std::nullopt;
    }

    /** @brief Intersects two normalized viewport rectangles. */
    inline InspectionViewportRect IntersectInspectionRect(const InspectionViewportRect &lhs, const InspectionViewportRect &rhs) {
        return {{std::max(lhs.minimum[0], rhs.minimum[0]), std::max(lhs.minimum[1], rhs.minimum[1])},
                {std::min(lhs.maximum[0], rhs.maximum[0]), std::min(lhs.maximum[1], rhs.maximum[1])}};
    }

    /** @brief Returns positive area for a nonempty rectangle and zero for touching or disjoint regions. */
    inline double InspectionRectArea(const InspectionViewportRect &rect) {
        return std::max(rect.maximum[0] - rect.minimum[0], 0.0) * std::max(rect.maximum[1] - rect.minimum[1], 0.0);
    }

    /** @brief Accumulates one projected edge endpoint before the explicit screen-region clip. */
    inline void IncludeInspectionPoint(InspectionViewportRect &bounds, const InspectionClipPoint &from, const InspectionClipPoint &to,
                                       const double fraction) {
        const double w = std::lerp(from[3], to[3], fraction);
        if (w <= 0.0)
            return;
        const std::array point{0.5 + 0.5 * std::lerp(from[0], to[0], fraction) / w, 0.5 - 0.5 * std::lerp(from[1], to[1], fraction) / w};
        for (std::size_t axis = 0; axis < point.size(); ++axis) {
            bounds.minimum[axis] = std::min(bounds.minimum[axis], point[axis]);
            bounds.maximum[axis] = std::max(bounds.maximum[axis], point[axis]);
        }
    }

    /** @brief Projects eight ordered box corners and clips their twelve edges before perspective division. */
    inline InspectedVisualIssues MeasureInspectionViewport(const std::uint64_t object, const std::array<InspectionClipPoint, 8> &corners,
                                                           const InspectionViewportRect &region) {
        constexpr double inf = std::numeric_limits<double>::infinity();
        InspectionViewportRect bounds{{inf, inf}, {-inf, -inf}};
        bool depthClipped{};
        for (std::size_t vertex = 0; vertex < corners.size(); ++vertex) {
            for (const std::size_t axis : {1U, 2U, 4U}) {
                if ((vertex & axis) != 0)
                    continue;
                const auto &from = corners[vertex];
                const auto &to = corners[vertex | axis];
                const auto clip = ClipInspectionEdge(from, to);
                if (!clip) {
                    depthClipped = true;
                    continue;
                }
                depthClipped |= (*clip)[0] != 0.0 || (*clip)[1] != 1.0;
                IncludeInspectionPoint(bounds, from, to, (*clip)[0]);
                IncludeInspectionPoint(bounds, from, to, (*clip)[1]);
            }
        }
        const auto cropped = IntersectInspectionRect(bounds, region);
        if (InspectionRectArea(cropped) == 0.0)
            return {.object = object};
        return {.object = object,
                .regionBounds = cropped,
                .clipped = depthClipped || bounds.minimum != cropped.minimum || bounds.maximum != cropped.maximum};
    }

    /** @brief Compares positive projected area against each other explicitly requested visible target. */
    inline void MeasureInspectionViewportOverlaps(std::span<InspectedVisualIssues> rows) {
        for (auto &source : rows) {
            if (!source.regionBounds)
                continue;
            double strongest{};
            for (const auto &peer : rows) {
                if (peer.object == source.object || !peer.regionBounds)
                    continue;
                const auto area = InspectionRectArea(IntersectInspectionRect(*source.regionBounds, *peer.regionBounds));
                if (area > strongest) {
                    strongest = area;
                    source.largestOverlap = InspectionViewportOverlap{peer.object, area};
                }
            }
        }
    }

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
        const Vec edge1 = subtract(b, a);
        const Vec edge2 = subtract(c, a);
        const Vec normal = InspectionCross(edge1, edge2);
        const double area = std::hypot(normal[0], normal[1], normal[2]);
        const Vec transverse = InspectionCross(ray.direction, edge2);
        const double determinant = InspectionDot(edge1, transverse);
        // Relative tolerance keeps the same grazing-angle rule at every mesh scale.
        if (std::abs(determinant) <= std::numeric_limits<double>::epsilon() * area)
            return std::nullopt;
        const Vec fromVertex = subtract(ray.origin, a);
        const double u = InspectionDot(fromVertex, transverse) / determinant;
        const Vec barycentric = InspectionCross(fromVertex, edge1);
        const double v = InspectionDot(ray.direction, barycentric) / determinant;
        if (u < 0.0 || v < 0.0 || u + v > 1.0)
            return std::nullopt;
        const double distance = InspectionDot(edge2, barycentric) / determinant;
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
