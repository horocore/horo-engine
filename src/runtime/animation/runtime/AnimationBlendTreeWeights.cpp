#include "AnimationBlendTreeGeometry.h"
#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        using namespace BlendDetail;

        /** @brief Reports whether a candidate is a nearer neighbor in the chosen coordinate direction. */
        bool Nearer(const GraphBlendTreeNode &tree, const std::size_t selected, const std::size_t candidate, const bool lower) noexcept {
            if (selected == tree.samples.size())
                return true;
            return lower ? tree.samples[candidate].x > tree.samples[selected].x : tree.samples[candidate].x < tree.samples[selected].x;
        }

        /** @brief Coordinate neighbors are selected independently of stable sample-ID ordering. */
        std::array<std::size_t, 2> Neighbors(const GraphBlendTreeNode &tree, const float x) noexcept {
            std::size_t low = tree.samples.size();
            std::size_t high = low;
            for (std::size_t i = 0; i < tree.samples.size(); ++i) {
                const auto value = tree.samples[i].x;
                if (value <= x && Nearer(tree, low, i, true))
                    low = i;
                if (value >= x && Nearer(tree, high, i, false))
                    high = i;
            }
            return {low, high};
        }

        /** @brief Finds coordinate neighbors in a stable-ID ordered sample table. */
        GraphInstanceOutcome One(const GraphBlendTreeNode &tree, const float x, const std::span<float> weights) {
            const auto [low, high] = Neighbors(tree, x);
            if (low == tree.samples.size() || high == tree.samples.size()) {
                if (tree.outside == GraphBlendOutside::Reject)
                    return {GraphInstanceFailure::Malformed};
                weights[low == tree.samples.size() ? high : low] = 1.0F;
                return {};
            }
            if (low == high) {
                weights[low] = 1.0F;
                return {};
            }
            const auto t = static_cast<float>((static_cast<double>(x) - tree.samples[low].x) /
                                              (static_cast<double>(tree.samples[high].x) - tree.samples[low].x));
            weights[low] = 1.0F - t;
            weights[high] = t;
            return {};
        }

        /** @brief Keeps closest authored edge projection; strict less preserves authored triangle/edge tie order. */
        void ClosestEdge(const Point p, const std::array<Point, 3> &points, std::array<double, 3> &best, double &distance) noexcept {
            for (std::size_t i = 0; i < 3; ++i) {
                const auto j = (i + 1) % 3;
                const auto t = EdgeWeight(p, points[i], points[j]);
                const Point projected{points[i].x + t * (points[j].x - points[i].x), points[i].y + t * (points[j].y - points[i].y)};
                const auto candidate = Distance(p, projected);
                if (candidate < distance) {
                    distance = candidate;
                    best = {};
                    best[i] = 1.0 - t;
                    best[j] = t;
                }
            }
        }

        /** @brief Normalizes only finite nonnegative geometry weights before publishing to binary32 members. */
        void Store(const GraphBlendTreeNode &tree, const GraphBlendTriangle &triangle, const std::array<double, 3> &values,
                   std::span<float> weights) noexcept {
            const auto sum = values[0] + values[1] + values[2];
            for (std::size_t i = 0; i < 3; ++i)
                weights[Index(tree, triangle.samples[i])] = static_cast<float>(values[i] / sum);
        }

        /** @brief Admitted triangles provide barycentric interior weights or exact nearest edge outside the domain. */
        GraphInstanceOutcome Two(const GraphBlendTreeNode &tree, const Point p, std::span<float> weights) noexcept {
            auto distance = std::numeric_limits<double>::infinity();
            std::size_t bestTriangle = 0;
            std::array<double, 3> best{};
            for (std::size_t i = 0; i < tree.triangles.size(); ++i) {
                const auto &triangle = tree.triangles[i];
                const std::array points{Position(tree.samples[Index(tree, triangle.samples[0])]),
                                        Position(tree.samples[Index(tree, triangle.samples[1])]),
                                        Position(tree.samples[Index(tree, triangle.samples[2])])};
                if (const auto values = Coordinates(p, points[0], points[1], points[2]); Inside(values)) {
                    Store(tree, triangle, values, weights);
                    return {};
                }
                const auto previous = distance;
                ClosestEdge(p, points, best, distance);
                if (distance < previous)
                    bestTriangle = i;
            }
            if (tree.outside == GraphBlendOutside::Reject)
                return {GraphInstanceFailure::Malformed};
            Store(tree, tree.triangles[bestTriangle], best, weights);
            return {};
        }
    }  // namespace

    /** @copydoc ComputeGraphBlendWeights */
    GraphInstanceOutcome ComputeGraphBlendWeights(const GraphBlendTreeNode &tree, const float x, const float y,
                                                  std::span<float> weights) noexcept {
        std::ranges::fill(weights, 0.0F);
        if (tree.dimension == GraphBlendDimension::One)
            return One(tree, x, weights);
        return Two(tree, {x, y}, weights);
    }
}  // namespace Horo::Animation::Detail
