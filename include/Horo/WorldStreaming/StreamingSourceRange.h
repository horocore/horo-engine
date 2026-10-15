#pragma once

/**
 * @file StreamingSourceRange.h
 * @brief Owned bounded source shapes and pure streaming-cell range evaluation.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/WorldStreaming/StreamingSourceDescriptor.h"
#include "Horo/WorldStreaming/WorldCellQuantization.h"
#include "Horo/WorldStreaming/WorldPartitionDescriptor.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace Horo::WorldStreaming {
    /** @brief Contract limits that keep range evaluation bounded and numerically stable. */
    struct StreamingSourceRangeLimits final {
        static constexpr std::int64_t MaximumExtentMillimeters = 1'000'000'000;
        static constexpr std::size_t MaximumPathPointCount = 16;
    };

    /** @brief Exact global sphere with a positive bounded millimeter radius. */
    struct StreamingSourceSphere final {
        Math::WorldCoordinate64 center;   /**< Exact global center. */
        std::int64_t radiusMillimeters{}; /**< Positive radius within MaximumExtentMillimeters. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingSourceSphere &) const noexcept = default;
    };

    /** @brief Exact global axis-aligned box with inclusive ordered millimeter endpoints. */
    struct StreamingSourceBox final {
        Math::WorldCoordinate64 minimum; /**< Inclusive global minimum. */
        Math::WorldCoordinate64 maximum; /**< Inclusive global maximum. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingSourceBox &) const noexcept = default;
    };

    /** @brief One inward-facing half-space in millimeters relative to a frustum anchor. */
    struct StreamingSourceFrustumPlane final {
        Math::Vec3 inwardNormal;     /**< Finite unit-length normal pointing into the frustum. */
        float distanceMillimeters{}; /**< Non-negative anchor-to-plane distance. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingSourceFrustumPlane &) const noexcept = default;
    };

    /**
     * @brief Bounded frustum expressed as six inward half-spaces around an exact global anchor.
     * @details A point is inside when dot(inwardNormal, point - anchor) + distanceMillimeters is non-negative for every plane.
     * maximumExtentMillimeters bounds the represented frustum on every axis and prevents far-world floating-point evaluation.
     */
    struct StreamingSourceFrustum final {
        Math::WorldCoordinate64 anchor;                      /**< Exact global point inside every half-space. */
        std::array<StreamingSourceFrustumPlane, 6> planes{}; /**< Six inward half-spaces in anchor-local millimeters. */
        std::int64_t maximumExtentMillimeters{};             /**< Positive axis bound for all represented geometry. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingSourceFrustum &) const noexcept = default;
    };

    /**
     * @brief Owned bounded polyline swept by an axis-aligned millimeter half-extent.
     * @details The volume is the union of line segments Minkowski-summed with an axis-aligned box. Only points before pointCount
     * participate.
     */
    struct StreamingSourcePathVolume final {
        std::array<Math::WorldCoordinate64, StreamingSourceRangeLimits::MaximumPathPointCount> points{}; /**< Owned points. */
        std::size_t pointCount{};             /**< Active prefix length in [2, MaximumPathPointCount]. */
        std::int64_t halfExtentMillimeters{}; /**< Positive swept axis-aligned half-extent. */

        /**
         * @brief Compares only the active point prefixes and remaining logical fields.
         * @param other Path volume to compare.
         * @return Strong ordering independent of inactive array storage.
         */
        [[nodiscard]] constexpr std::strong_ordering operator<=>(const StreamingSourcePathVolume &other) const noexcept {
            const std::size_t commonPointCount = std::min({pointCount, other.pointCount, points.size()});
            for (std::size_t index = 0; index < commonPointCount; ++index) {
                if (const auto order = points[index] <=> other.points[index]; order != 0)
                    return order;
            }
            if (const auto order = pointCount <=> other.pointCount; order != 0)
                return order;
            return halfExtentMillimeters <=> other.halfExtentMillimeters;
        }

        /** @brief Tests logical equality without observing inactive array storage. @param other Path volume to compare. @return Equality.
         */
        [[nodiscard]] constexpr bool operator==(const StreamingSourcePathVolume &other) const noexcept {
            return (*this <=> other) == 0;
        }
    };

    /** @brief One value-owned source influence shape. */
    using StreamingSourceShape = std::variant<StreamingSourceSphere, StreamingSourceBox, StreamingSourceFrustum, StreamingSourcePathVolume>;

    /** @brief Shape capabilities supplied explicitly by the evaluating host. */
    struct StreamingSourceShapeSupport final {
        bool sphere{true};     /**< Whether sphere evaluation is admitted. */
        bool box{true};        /**< Whether box evaluation is admitted. */
        bool frustum{true};    /**< Whether frustum evaluation is admitted. */
        bool pathVolume{true}; /**< Whether path-volume evaluation is admitted. */
    };

    /**
     * @brief Validates that a shape is structurally bounded without consulting a backend or registry.
     * @param shape Value-owned shape to inspect.
     * @return Success or a typed invalid/range error.
     */
    [[nodiscard]] Result<void> ValidateStreamingSourceShape(const StreamingSourceShape &shape);

    /**
     * @brief Purely evaluates whether an admissible source shape intersects one partition cell.
     * @param descriptor Inert source identity and owner metadata.
     * @param admission Immutable owner lifetime, revision, capacity, and lifecycle snapshot.
     * @param shape Structurally bounded value-owned shape.
     * @param support Explicit host-supported shape categories.
     * @param policy Validated partition quantization policy associated with descriptor.owner.partition.
     * @param cell Candidate cell to evaluate.
     * @return Intersection result, or a typed invalid, stale, capacity, lifecycle, unsupported, bounds, or arithmetic error. No state is
     * mutated.
     */
    [[nodiscard]] Result<bool> EvaluateStreamingSourceCellRange(const StreamingSourceDescriptor &descriptor,
                                                                const StreamingSourceAdmissionContext &admission,
                                                                const StreamingSourceShape &shape, StreamingSourceShapeSupport support,
                                                                const WorldCellQuantizationPolicy &policy, StreamingCellId cell);
    /**
     * @brief Measures exact Chebyshev millimeter distance from the source anchor to a cell AABB.
     * @param shape Valid source geometry; box minimum, sphere center, frustum anchor or first path point is its relevance origin.
     * @param policy Exact immutable grid.
     * @param cell Manifest cell identity.
     * @return Non-negative integer distance or typed geometry/grid failure; no floating-point position conversion occurs.
     */
    [[nodiscard]] Result<std::uint64_t> StreamingSourceCellDistance(const StreamingSourceShape &shape,
                                                                    const WorldCellQuantizationPolicy &policy, StreamingCellId cell);

    /**
     * @brief Computes conservative exact query bounds for a validated shape, with outward hysteresis padding.
     * @param shape Owned source geometry.
     * @param expansionMillimeters Non-negative bounded exit margin; endpoints saturate at canonical coordinate limits.
     * @return Inclusive spatial-index query bounds or typed invalid geometry failure.
     */
    [[nodiscard]] Result<WorldPartitionBounds> StreamingSourceQueryBounds(const StreamingSourceShape &shape,
                                                                          std::int64_t expansionMillimeters);
    /**
     * @brief Produces threshold evidence for existing cell stability without estimating an unmeasured geometric distance.
     * @param shape Validated source geometry.
     * @param policy Exact immutable grid.
     * @param cell Manifest cell identity.
     * @param enterMarginMillimeters Non-negative inward admission margin.
     * @param exitMarginMillimeters Non-negative outward retention margin.
     * @return Signed evidence meeting precisely the enter/exit thresholds, or typed invalid/range failure.
     * @details Zero means original overlap without sufficient entry depth. This value is threshold evidence, not a metric distance.
     */
    [[nodiscard]] Result<std::int64_t> EvaluateStreamingSourceCellBoundary(const StreamingSourceShape &shape,
                                                                           const WorldCellQuantizationPolicy &policy, StreamingCellId cell,
                                                                           std::int64_t enterMarginMillimeters,
                                                                           std::int64_t exitMarginMillimeters);
}  // namespace Horo::WorldStreaming
