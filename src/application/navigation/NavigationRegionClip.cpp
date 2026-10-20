#include "NavigationRegionClipInternal.h"

namespace Horo::Application::NavigationBakeDetail::RegionClip {
    namespace {
        namespace Errors = Navigation::NavigationErrors;

        double Coordinate(Math::Vec3 point, std::size_t axis) {
            if (axis == 0)
                return point.x;
            return axis == 1 ? point.y : point.z;
        }

        /** @brief Deduplicates adjacent vertices before the fixed convex polygon bound is consumed. */
        bool Point(Polygon &output, Math::Vec3 point) {
            if (output.count > 0 && output.vertices[output.count - 1] == point)
                return true;
            if (output.count == output.vertices.size())
                return false;
            output.vertices[output.count++] = point;
            return true;
        }

        /** @brief Inclusive inside and strict outside assign coplanar faces to exactly one side. */
        Result<Polygon> ClipFace(const Polygon &input, std::size_t axis, double boundary, bool lower, bool inside, Work &work) {
            if (auto admitted = work.Spend(input.count); admitted.HasError())
                return Result<Polygon>::Failure(admitted.ErrorValue());
            Polygon output;
            for (std::size_t i = 0; i < input.count; ++i) {
                const auto a = input.vertices[i];
                const auto b = input.vertices[(i + 1) % input.count];
                const double da = (Coordinate(a, axis) - boundary) * (lower ? 1 : -1);
                const double db = (Coordinate(b, axis) - boundary) * (lower ? 1 : -1);
                const bool keepA = inside ? da >= 0 : da < 0;
                const bool keepB = inside ? db >= 0 : db < 0;
                if (keepA && !Point(output, a))
                    return Result<Polygon>::Failure(MakeError(Errors::CapacityExceeded));
                if (keepA == keepB)
                    continue;
                const double t = da / (da - db);
                const auto mix = [t](float x, float y) {
                    return static_cast<float>(x + t * (static_cast<double>(y) - x));
                };
                if (!Point(output, {mix(a.x, b.x), mix(a.y, b.y), mix(a.z, b.z)}))
                    return Result<Polygon>::Failure(MakeError(Errors::CapacityExceeded));
            }
            if (output.count > 1 && output.vertices[0] == output.vertices[output.count - 1])
                --output.count;
            return Result<Polygon>::Success(std::move(output));
        }

        /** @brief Preserves source winding and transforms only retained vertices back to canonical metres. */
        Result<void> Emit(const Polygon &polygon, const Math::Mat4 &world, Fragments &output, std::uint32_t maximumFragments, Work &work) {
            if (auto admitted = work.Spend(polygon.count); admitted.HasError())
                return admitted;
            Polygon canonical;
            canonical.count = polygon.count;
            for (std::size_t i = 0; i < polygon.count; ++i) {
                auto point = Math::TryTransformPoint(world, polygon.vertices[i]);
                if (point.HasError())
                    return Result<void>::Failure(point.ErrorValue());
                canonical.vertices[i] = point.Value();
            }
            for (std::size_t i = 1; i + 1 < canonical.count; ++i)
                if (auto appended =
                        Append(output, {canonical.vertices[0], canonical.vertices[i], canonical.vertices[i + 1]}, maximumFragments, work);
                    appended.HasError())
                    return appended;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc Work::Spend */
    Result<void> Work::Spend(std::uint64_t units) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation region selection cancelled"));
        if (units > remaining)
            return Result<void>::Failure(MakeError(Errors::CapacityExceeded, "Navigation region work budget exhausted"));
        remaining -= units;
        return Result<void>::Success();
    }

    /** @copydoc Append */
    Result<void> Append(Fragments &output, const Triangle &triangle, std::uint32_t maximumFragments, Work &work) {
        if (auto admitted = work.Spend(1); admitted.HasError())
            return admitted;
        if (!TriangleHasArea(triangle[0], triangle[1], triangle[2]))
            return Result<void>::Success();
        if (output.count >= maximumFragments)
            return Result<void>::Failure(MakeError(Errors::CapacityExceeded, "Navigation region fragment budget exhausted"));
        output.values[output.count++] = triangle;
        return Result<void>::Success();
    }

    /** @copydoc Split */
    Result<void> Split(const Triangle &triangle, const SourceRegion &region, Fragments *inside, Fragments &outside,
                       std::uint32_t maximumFragments, Work &work) {
        if (auto admitted = work.Spend(3); admitted.HasError())
            return admitted;
        Polygon remainder;
        for (const auto vertex : triangle) {
            auto point = Math::TryTransformPoint(region.inverse, vertex);
            if (point.HasError())
                return Result<void>::Failure(point.ErrorValue());
            remainder.vertices[remainder.count++] = point.Value();
        }
        const auto &bounds = region.authored.localBounds;
        for (std::size_t face = 0; face < 6 && remainder.count >= 3; ++face) {
            const auto axis = face / 2;
            const bool lower = face % 2 == 0;
            const double boundary = Coordinate(lower ? bounds.center - bounds.halfExtents : bounds.center + bounds.halfExtents, axis);
            auto removed = ClipFace(remainder, axis, boundary, lower, false, work);
            if (removed.HasError())
                return Result<void>::Failure(removed.ErrorValue());
            if (auto emitted = Emit(removed.Value(), region.world, outside, maximumFragments, work); emitted.HasError())
                return emitted;
            auto kept = ClipFace(remainder, axis, boundary, lower, true, work);
            if (kept.HasError())
                return Result<void>::Failure(kept.ErrorValue());
            remainder = std::move(kept).Value();
        }
        if (inside)
            return Emit(remainder, region.world, *inside, maximumFragments, work);
        return Result<void>::Success();
    }
}  // namespace Horo::Application::NavigationBakeDetail::RegionClip
