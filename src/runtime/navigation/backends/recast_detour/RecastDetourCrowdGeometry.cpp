#include "RecastDetourCrowdGeometry.h"

#include <algorithm>
#include <cmath>

namespace Horo::Navigation::Detail {
    namespace {
        struct Point2 final {
            double x{};
            double z{};
        };

        [[nodiscard]] Point2 Difference(const Point2 a, const Point2 b) noexcept {
            return {a.x - b.x, a.z - b.z};
        }

        [[nodiscard]] double Dot(const Point2 a, const Point2 b) noexcept {
            return a.x * b.x + a.z * b.z;
        }

        [[nodiscard]] double Cross(const Point2 a, const Point2 b) noexcept {
            return a.x * b.z - a.z * b.x;
        }

        [[nodiscard]] double PointSegmentDistanceSquared(const Point2 point, const Point2 first, const Point2 second) noexcept {
            const Point2 edge = Difference(second, first);
            const double lengthSquared = Dot(edge, edge);
            const double t = lengthSquared > 0.0 ? std::clamp(Dot(Difference(point, first), edge) / lengthSquared, 0.0, 1.0) : 0.0;
            const Point2 delta = Difference(point, {first.x + t * edge.x, first.z + t * edge.z});
            return Dot(delta, delta);
        }

        [[nodiscard]] bool Intersects(const Point2 first, const Point2 second, const Point2 otherFirst, const Point2 otherSecond) noexcept {
            const Point2 direction = Difference(second, first);
            const Point2 otherDirection = Difference(otherSecond, otherFirst);
            const double divisor = Cross(direction, otherDirection);
            if (std::abs(divisor) < 1.0e-12)
                return false;
            const Point2 displacement = Difference(otherFirst, first);
            const double t = Cross(displacement, otherDirection) / divisor;
            const double u = Cross(displacement, direction) / divisor;
            return t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0;
        }

        [[nodiscard]] double SegmentDistanceSquared(const Point2 first, const Point2 second, const Point2 otherFirst,
                                                    const Point2 otherSecond) noexcept {
            if (Intersects(first, second, otherFirst, otherSecond))
                return 0.0;
            return std::min(
                {PointSegmentDistanceSquared(first, otherFirst, otherSecond), PointSegmentDistanceSquared(second, otherFirst, otherSecond),
                 PointSegmentDistanceSquared(otherFirst, first, second), PointSegmentDistanceSquared(otherSecond, first, second)});
        }
    }  // namespace

    bool IsAvoidanceCandidateClear(const NavigationCrowdSnapshot &snapshot, const NavigationCrowdAgentFact &agent,
                                   const Math::Vec3 &candidate, const float horizonSeconds) noexcept {
        const Point2 start{agent.position.x, agent.position.z};
        const Point2 end{start.x + static_cast<double>(candidate.x) * horizonSeconds,
                         start.z + static_cast<double>(candidate.z) * horizonSeconds};
        for (std::uint32_t offset = 0; offset < agent.neighborCount; ++offset) {
            const auto index = snapshot.NeighborIndices()[agent.firstNeighbor + offset];
            const auto &neighbor = snapshot.Agents()[index];
            const Point2 relativeStart{start.x - neighbor.position.x, start.z - neighbor.position.z};
            const Point2 relativeEnd{relativeStart.x + (static_cast<double>(candidate.x) - neighbor.velocity.x) * horizonSeconds,
                                     relativeStart.z + (static_cast<double>(candidate.z) - neighbor.velocity.z) * horizonSeconds};
            const double radius = static_cast<double>(agent.radiusMeters) + neighbor.radiusMeters;
            if (PointSegmentDistanceSquared({0.0, 0.0}, relativeStart, relativeEnd) <= radius * radius)
                return false;
        }
        for (std::uint32_t offset = 0; offset < agent.boundaryCount; ++offset) {
            const auto index = snapshot.BoundaryIndices()[agent.firstBoundary + offset];
            const auto &boundary = snapshot.BoundarySegments()[index];
            const double radius = agent.radiusMeters;
            if (SegmentDistanceSquared(start, end, {boundary.first.x, boundary.first.z}, {boundary.second.x, boundary.second.z}) <=
                radius * radius)
                return false;
        }
        return true;
    }
}  // namespace Horo::Navigation::Detail
