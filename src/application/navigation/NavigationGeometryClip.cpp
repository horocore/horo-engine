#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationGeometryCanonicalization.h"

#include <algorithm>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        namespace Errors = Navigation::NavigationErrors;
        using Geometry = NavigationPrimitiveGeometry;

        /** @brief Fixed stack polygon; clipping a triangle by six convex planes adds at most six vertices. */
        struct Polygon final {
            std::array<Math::Vec3, 16> vertices{};
            std::size_t count{};
        };

        struct Counts final {
            std::uint64_t vertices{};
            std::uint64_t triangles{};
        };

        /** @brief Selects an explicit coordinate without assuming Vec3's native layout. */
        double Coordinate(Math::Vec3 value, std::size_t axis) {
            if (axis == 0)
                return value.x;
            return axis == 1 ? value.y : value.z;
        }

        /** @brief Clips a convex polygon against one inclusive local box face. */
        Result<Polygon> ClipFace(const Polygon &input, std::size_t axis, double boundary, bool lower) {
            Polygon output;
            const auto append = [&output](Math::Vec3 point) {
                if (output.count > 0 && output.vertices[output.count - 1] == point)
                    return true;
                if (output.count == output.vertices.size())
                    return false;
                output.vertices[output.count++] = point;
                return true;
            };
            for (std::size_t i = 0; i < input.count; ++i) {
                const auto a = input.vertices[i];
                const auto b = input.vertices[(i + 1) % input.count];
                const double da = (Coordinate(a, axis) - boundary) * (lower ? 1 : -1);
                const double db = (Coordinate(b, axis) - boundary) * (lower ? 1 : -1);
                if (da >= 0 && !append(a))
                    return Result<Polygon>::Failure(MakeError(Errors::CapacityExceeded, "Navigation clipping polygon bound exceeded"));
                if ((da >= 0) == (db >= 0))
                    continue;
                const double t = da / (da - db);
                const auto mix = [t](float x, float y) {
                    return static_cast<float>(x + t * (static_cast<double>(y) - x));
                };
                if (!append({mix(a.x, b.x), mix(a.y, b.y), mix(a.z, b.z)}))
                    return Result<Polygon>::Failure(MakeError(Errors::CapacityExceeded, "Navigation clipping polygon bound exceeded"));
            }
            if (output.count > 1 && output.vertices[0] == output.vertices[output.count - 1])
                --output.count;
            return Result<Polygon>::Success(std::move(output));
        }

        /** @brief Transforms and clips one canonical triangle in the exact surface-local frame. */
        Result<Polygon> ClipTriangle(const Geometry &input, const Navigation::NavigationSourceTriangleInput &triangle,
                                     const Math::Aabb &bounds, const Math::Mat4 &inverse, const Math::Mat4 &world) {
            Polygon polygon;
            for (const auto index : triangle.vertexIndices) {
                auto point = Math::TryTransformPoint(inverse, input.vertices[index]);
                if (point.HasError())
                    return Result<Polygon>::Failure(point.ErrorValue());
                polygon.vertices[polygon.count++] = point.Value();
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                for (const bool lower : {true, false}) {
                    auto clipped = ClipFace(polygon, axis, Coordinate(lower ? bounds.minimum : bounds.maximum, axis), lower);
                    if (clipped.HasError())
                        return clipped;
                    polygon = std::move(clipped).Value();
                }
            }
            for (std::size_t i = 0; i < polygon.count; ++i) {
                auto point = Math::TryTransformPoint(world, polygon.vertices[i]);
                if (point.HasError())
                    return Result<Polygon>::Failure(point.ErrorValue());
                polygon.vertices[i] = point.Value();
            }
            return Result<Polygon>::Success(std::move(polygon));
        }

        /** @brief Counts or writes the same clipped triangles; count admission precedes output allocation. */
        Result<Counts> ClipPass(const Geometry &input, const Math::Aabb &bounds, const Math::Mat4 &inverse, const Math::Mat4 &world,
                                const NavigationPrimitiveGeometryPolicy &policy, Geometry *output, CancellationToken cancellation) {
            Counts count;
            for (const auto &triangle : input.triangles) {
                if (cancellation.IsCancellationRequested())
                    return Result<Counts>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation scope clipping cancelled"));
                auto clipped = ClipTriangle(input, triangle, bounds, inverse, world);
                if (clipped.HasError())
                    return Result<Counts>::Failure(clipped.ErrorValue());
                const auto &polygon = clipped.Value();
                for (std::size_t i = 1; i + 1 < polygon.count; ++i) {
                    const auto a = polygon.vertices[0];
                    const auto b = polygon.vertices[i];
                    const auto c = polygon.vertices[i + 1];
                    if (!TriangleHasArea(a, b, c))
                        continue;
                    count.vertices += 3;
                    ++count.triangles;
                    if (count.vertices > policy.storage.maxVertices || count.triangles > policy.storage.maxTriangles)
                        return Result<Counts>::Failure(
                            MakeError(Errors::CapacityExceeded, "Navigation scope clipping exceeds geometry counts"));
                    if (output) {
                        const auto base = static_cast<std::uint32_t>(output->vertices.size());
                        output->vertices.insert(output->vertices.end(), {a, b, c});
                        output->triangles.push_back({{base, base + 1, base + 2}, triangle.area, triangle.materialSlot});
                    }
                }
            }
            return Result<Counts>::Success(count);
        }

        /** @brief Preserves exact existing geometry bytes when every vertex is already inside the explicit scope. */
        Result<bool> AllInside(const Geometry &input, const Math::Aabb &bounds, const Math::Mat4 &inverse, CancellationToken cancellation) {
            for (const auto vertex : input.vertices) {
                if (cancellation.IsCancellationRequested())
                    return Result<bool>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation scope clipping cancelled"));
                auto local = Math::TryTransformPoint(inverse, vertex);
                if (local.HasError())
                    return Result<bool>::Failure(local.ErrorValue());
                for (std::size_t axis = 0; axis < 3; ++axis)
                    if (Coordinate(local.Value(), axis) < Coordinate(bounds.minimum, axis) ||
                        Coordinate(local.Value(), axis) > Coordinate(bounds.maximum, axis))
                        return Result<bool>::Success(false);
            }
            return Result<bool>::Success(true);
        }
    }  // namespace

    /** @copydoc ClipGeometry */
    Result<NavigationPrimitiveGeometry> ClipGeometry(NavigationPrimitiveGeometry input, const Math::Aabb &bounds,
                                                     const Math::Mat4 &localToWorld, const NavigationPrimitiveGeometryPolicy &policy,
                                                     Navigation::NavigationAreaId area, Navigation::NavigationSourceMaterialSlot material,
                                                     CancellationToken cancellation) {
        auto inverse = Math::TryInverseAffine(localToWorld);
        if (inverse.HasError())
            return Result<Geometry>::Failure(inverse.ErrorValue());
        auto inside = AllInside(input, bounds, inverse.Value(), cancellation);
        if (inside.HasError())
            return Result<Geometry>::Failure(inside.ErrorValue());
        if (inside.Value())
            return Result<Geometry>::Success(std::move(input));
        auto count = ClipPass(input, bounds, inverse.Value(), localToWorld, policy, nullptr, cancellation);
        if (count.HasError())
            return Result<Geometry>::Failure(count.ErrorValue());
        const auto ownedBytes =
            input.vertices.capacity() * sizeof(Math::Vec3) + input.triangles.capacity() * sizeof(Navigation::NavigationSourceTriangleInput);
        if (const auto clippedBytes =
                count.Value().vertices * sizeof(Math::Vec3) + count.Value().triangles * sizeof(Navigation::NavigationSourceTriangleInput);
            ownedBytes + clippedBytes > policy.storage.maxOwnedBytes)
            return Result<Geometry>::Failure(
                MakeError(Errors::CapacityExceeded, "Navigation scope clipping exceeds peak geometry storage"));
        Geometry output;
        output.vertices.reserve(static_cast<std::size_t>(count.Value().vertices));
        output.triangles.reserve(static_cast<std::size_t>(count.Value().triangles));
        if (auto written = ClipPass(input, bounds, inverse.Value(), localToWorld, policy, &output, cancellation); written.HasError())
            return Result<Geometry>::Failure(written.ErrorValue());
        return FinalizeGeometry(std::move(output), {}, {}, area, material, cancellation);
    }
}  // namespace Horo::Application::NavigationBakeDetail
