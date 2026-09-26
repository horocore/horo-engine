#include "PreFracturedIntersection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace Horo::Destruction::Detail {
    namespace {
        using Point = std::array<double, 3>;
        using Point2 = std::array<double, 2>;

        struct Triangle {
            std::array<std::uint32_t, 3> indices{};
            Point minimum{};
            Point maximum{};
        };

        [[nodiscard]] Point Position(const Assets::PreFracturedSourceNode &node, const std::uint32_t index) {
            const auto &position = node.positions[index];
            return {position[0], position[1], position[2]};
        }

        [[nodiscard]] Point Subtract(const Point &a, const Point &b) {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }

        [[nodiscard]] Point Cross(const Point &a, const Point &b) {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        [[nodiscard]] double Dot(const Point &a, const Point &b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        [[nodiscard]] std::optional<Point> SegmentPiercesTriangle(const Point &start, const Point &end,
                                                                  const std::array<Point, 3> &triangle) {
            constexpr double epsilon = 1.0e-10;
            const Point direction = Subtract(end, start);
            const Point edgeA = Subtract(triangle[1], triangle[0]);
            const Point edgeB = Subtract(triangle[2], triangle[0]);
            const Point p = Cross(direction, edgeB);
            const double determinant = Dot(edgeA, p);
            if (std::abs(determinant) <= epsilon)
                return std::nullopt;
            const double inverse = 1.0 / determinant;
            const Point offset = Subtract(start, triangle[0]);
            const double u = Dot(offset, p) * inverse;
            if (u < -epsilon || u > 1.0 + epsilon)
                return std::nullopt;
            const Point q = Cross(offset, edgeA);
            if (const double v = Dot(direction, q) * inverse; v < -epsilon || u + v > 1.0 + epsilon)
                return std::nullopt;
            const double t = Dot(edgeB, q) * inverse;
            if (t < -epsilon || t > 1.0 + epsilon)
                return std::nullopt;
            return Point{start[0] + t * direction[0], start[1] + t * direction[1], start[2] + t * direction[2]};
        }

        [[nodiscard]] Point2 Project(const Point &point, const std::size_t axis) {
            return {point[(axis + 1U) % 3U], point[(axis + 2U) % 3U]};
        }

        [[nodiscard]] double Orient(const Point2 &a, const Point2 &b, const Point2 &c) {
            return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        }

        [[nodiscard]] bool SegmentsMeet(const Point2 &a, const Point2 &b, const Point2 &c, const Point2 &d) {
            constexpr double epsilon = 1.0e-10;
            const double ac = Orient(a, b, c);
            const double ad = Orient(a, b, d);
            const double ca = Orient(c, d, a);
            if (const double cb = Orient(c, d, b); (ac > epsilon && ad > epsilon) || (ac < -epsilon && ad < -epsilon) ||
                                                   (ca > epsilon && cb > epsilon) || (ca < -epsilon && cb < -epsilon))
                return false;
            const auto overlap = [](const double a0, const double a1, const double b0, const double b1) {
                return std::max(std::min(a0, a1), std::min(b0, b1)) <= std::min(std::max(a0, a1), std::max(b0, b1)) + 1.0e-10;
            };
            return overlap(a[0], b[0], c[0], d[0]) && overlap(a[1], b[1], c[1], d[1]);
        }

        [[nodiscard]] bool CoplanarOverlap(const std::array<Point, 3> &a, const std::array<Point, 3> &b, const Point &normal) {
            std::size_t axis = 0;
            for (std::size_t candidate = 1; candidate < 3; ++candidate) {
                if (std::abs(normal[candidate]) > std::abs(normal[axis]))
                    axis = candidate;
            }
            std::array<Point2, 3> projectedA{};
            std::array<Point2, 3> projectedB{};
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                projectedA[vertex] = Project(a[vertex], axis);
                projectedB[vertex] = Project(b[vertex], axis);
            }
            for (std::size_t edgeA = 0; edgeA < 3; ++edgeA) {
                for (std::size_t edgeB = 0; edgeB < 3; ++edgeB) {
                    if (SegmentsMeet(projectedA[edgeA], projectedA[(edgeA + 1U) % 3U], projectedB[edgeB], projectedB[(edgeB + 1U) % 3U]))
                        return true;
                }
            }
            const auto inside = [](const Point2 &point, const std::array<Point2, 3> &triangle) {
                const double a = Orient(triangle[0], triangle[1], point);
                const double b = Orient(triangle[1], triangle[2], point);
                const double c = Orient(triangle[2], triangle[0], point);
                return (a >= -1.0e-10 && b >= -1.0e-10 && c >= -1.0e-10) || (a <= 1.0e-10 && b <= 1.0e-10 && c <= 1.0e-10);
            };
            return inside(projectedA[0], projectedB) || inside(projectedB[0], projectedA);
        }

        [[nodiscard]] bool CoplanarAreaOverlap(const std::array<Point, 3> &a, const std::array<Point, 3> &b, const Point &normal) {
            std::size_t axis = 0;
            for (std::size_t candidate = 1; candidate < 3; ++candidate) {
                if (std::abs(normal[candidate]) > std::abs(normal[axis]))
                    axis = candidate;
            }
            std::array<Point2, 3> clip{};
            std::array<Point2, 8> polygon{};
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                polygon[vertex] = Project(a[vertex], axis);
                clip[vertex] = Project(b[vertex], axis);
            }
            const double sign = Orient(clip[0], clip[1], clip[2]) >= 0.0 ? 1.0 : -1.0;
            std::size_t count = 3;
            for (std::size_t edge = 0; edge < 3; ++edge) {
                std::array<Point2, 8> next{};
                std::size_t nextCount{};
                const Point2 start = clip[edge];
                const Point2 end = clip[(edge + 1U) % 3U];
                for (std::size_t vertex = 0; vertex < count; ++vertex) {
                    const Point2 previous = polygon[(vertex + count - 1U) % count];
                    const Point2 current = polygon[vertex];
                    const double previousSide = sign * Orient(start, end, previous);
                    const double currentSide = sign * Orient(start, end, current);
                    const bool previousInside = previousSide >= -1.0e-10;
                    const bool currentInside = currentSide >= -1.0e-10;
                    if (previousInside != currentInside) {
                        const double fraction = previousSide / (previousSide - currentSide);
                        next[nextCount++] = {previous[0] + fraction * (current[0] - previous[0]),
                                             previous[1] + fraction * (current[1] - previous[1])};
                    }
                    if (currentInside)
                        next[nextCount++] = current;
                }
                if (nextCount == 0)
                    return false;
                polygon = next;
                count = nextCount;
            }
            double doubledArea{};
            for (std::size_t vertex = 0; vertex < count; ++vertex) {
                const auto &current = polygon[vertex];
                const auto &next = polygon[(vertex + 1U) % count];
                doubledArea += current[0] * next[1] - current[1] * next[0];
            }
            double extent{};
            for (const auto &point : clip)
                extent = std::max({extent, std::abs(point[0] - clip[0][0]), std::abs(point[1] - clip[0][1])});
            return std::abs(doubledArea) > extent * extent * 1.0e-12;
        }

        [[nodiscard]] bool SamePoint(const Point &a, const Point &b) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (std::abs(a[axis] - b[axis]) > 1.0e-7 * std::max({1.0, std::abs(a[axis]), std::abs(b[axis])}))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool EdgesPierceBeyondShared(const std::array<Point, 3> &edges, const std::array<Point, 3> &triangle,
                                                   const std::optional<Point> &sharedPoint) {
            for (std::size_t edge = 0; edge < 3; ++edge) {
                const auto hit = SegmentPiercesTriangle(edges[edge], edges[(edge + 1U) % 3U], triangle);
                if (hit.has_value() && (!sharedPoint.has_value() || !SamePoint(*hit, *sharedPoint)))
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool TrianglesIntersect(const Assets::PreFracturedSourceNode &node, const Triangle &a, const Triangle &b) {
            std::array<Point, 3> pointsA{};
            std::array<Point, 3> pointsB{};
            std::array<std::uint32_t, 3> shared{};
            std::size_t sharedCount{};
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                pointsA[vertex] = Position(node, a.indices[vertex]);
                pointsB[vertex] = Position(node, b.indices[vertex]);
                for (const auto other : b.indices) {
                    if (a.indices[vertex] == other)
                        shared[sharedCount++] = other;
                }
            }
            const Point normal = Cross(Subtract(pointsA[1], pointsA[0]), Subtract(pointsA[2], pointsA[0]));
            bool coplanar = true;
            for (const Point &point : pointsB)
                coplanar &= std::abs(Dot(normal, Subtract(point, pointsA[0]))) <= 1.0e-10;
            if (sharedCount == 3)
                return true;
            if (sharedCount == 2)
                return coplanar && CoplanarAreaOverlap(pointsA, pointsB, normal);
            const std::optional<Point> sharedPoint = sharedCount == 0 ? std::nullopt : std::optional{Position(node, shared[0])};
            if (EdgesPierceBeyondShared(pointsA, pointsB, sharedPoint) || EdgesPierceBeyondShared(pointsB, pointsA, sharedPoint))
                return true;
            if (!coplanar)
                return false;
            return sharedCount == 0 ? CoplanarOverlap(pointsA, pointsB, normal) : CoplanarAreaOverlap(pointsA, pointsB, normal);
        }
    }  // namespace

    IntersectionCheck CheckSelfIntersection(const Assets::PreFracturedSourceNode &node, std::uint64_t &remainingWork,
                                            const CancellationToken &cancellation) {
        std::vector<Triangle> triangles;
        triangles.reserve(node.triangleIndices.size() / 3U);
        for (std::size_t offset = 0; offset < node.triangleIndices.size(); offset += 3U) {
            Triangle triangle;
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                triangle.indices[vertex] = node.triangleIndices[offset + vertex];
                const Point point = Position(node, triangle.indices[vertex]);
                if (vertex == 0) {
                    triangle.minimum = point;
                    triangle.maximum = point;
                } else {
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        triangle.minimum[axis] = std::min(triangle.minimum[axis], point[axis]);
                        triangle.maximum[axis] = std::max(triangle.maximum[axis], point[axis]);
                    }
                }
            }
            triangles.push_back(triangle);
        }
        std::ranges::sort(triangles, [](const Triangle &a, const Triangle &b) {
            return a.minimum[0] < b.minimum[0];
        });
        for (std::size_t left = 0; left < triangles.size(); ++left) {
            if (cancellation.IsCancellationRequested())
                return IntersectionCheck::Cancelled;
            const auto &a = triangles[left];
            for (std::size_t right = left + 1U; right < triangles.size() && triangles[right].minimum[0] <= a.maximum[0]; ++right) {
                if (remainingWork == 0)
                    return IntersectionCheck::TooMuchWork;
                --remainingWork;
                const auto &b = triangles[right];
                if (a.maximum[1] < b.minimum[1] || b.maximum[1] < a.minimum[1] || a.maximum[2] < b.minimum[2] ||
                    b.maximum[2] < a.minimum[2])
                    continue;
                if (TrianglesIntersect(node, a, b))
                    return IntersectionCheck::Intersecting;
            }
        }
        return IntersectionCheck::Clear;
    }
}  // namespace Horo::Destruction::Detail
