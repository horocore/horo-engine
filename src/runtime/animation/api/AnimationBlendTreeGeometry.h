#pragma once
#include "Horo/Animation/AnimationGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Horo::Animation::BlendDetail {
    /** @brief Double intermediates retain finite binary32 coordinate arithmetic without float overflow. */
    struct Point final {
        double x{};
        double y{};
    };

    /** @brief Projects authored coordinates without retaining host storage. */
    inline Point Position(const GraphBlendSample &sample) noexcept {
        return {sample.x, sample.y};
    }

    /** @brief Exact signed doubled triangle area for bounded finite binary32 coordinates. */
    inline double Cross(const Point a, const Point b, const Point c) noexcept {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    /** @brief Resolves canonical stable sample identity without narrowing an absent result. */
    inline std::size_t Index(const GraphBlendTreeNode &tree, const GraphBlendSampleId id) noexcept {
        const auto found = std::ranges::find(tree.samples, id, &GraphBlendSample::id);
        return static_cast<std::size_t>(found - tree.samples.begin());
    }

    /** @brief Returns normalized signed barycentric coordinates for an admitted nondegenerate triangle. */
    inline std::array<double, 3> Coordinates(const Point p, const Point a, const Point b, const Point c) noexcept {
        const auto area = Cross(a, b, c);
        return {Cross(p, b, c) / area, Cross(a, p, c) / area, Cross(a, b, p) / area};
    }

    /** @brief Inclusive domain test; a shared boundary is resolved by canonical triangle order. */
    inline bool Inside(const std::array<double, 3> &weights) noexcept {
        return std::ranges::all_of(weights, [](double value) {
            return value >= 0.0;
        });
    }

    /** @brief Strict interior test rejects overlapping authored faces while allowing common edges. */
    inline bool StrictInside(const Point p, const std::array<Point, 3> &triangle) noexcept {
        const auto weights = Coordinates(p, triangle[0], triangle[1], triangle[2]);
        return std::ranges::all_of(weights, [](double value) {
            return value > 0.0;
        });
    }

    /** @brief Tests proper edge crossings without rejecting shared endpoints. */
    inline bool EdgesCross(const Point a, const Point b, const Point c, const Point d) noexcept {
        return Cross(a, b, c) * Cross(a, b, d) < 0.0 && Cross(c, d, a) * Cross(c, d, b) < 0.0;
    }

    /** @brief Squared Euclidean separation is finite for admitted coordinates. */
    inline double Distance(const Point a, const Point b) noexcept {
        return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
    }

    /** @brief Closest point on one nonempty authored edge, expressed in normalized edge weight. */
    inline double EdgeWeight(const Point p, const Point a, const Point b) noexcept {
        return std::clamp(((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) / Distance(a, b), 0.0, 1.0);
    }
}  // namespace Horo::Animation::BlendDetail
