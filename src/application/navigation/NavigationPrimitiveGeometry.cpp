#include "Horo/Application/NavigationPrimitiveGeometry.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationGeometryCanonicalization.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>

namespace Horo::Application {
    namespace {
        using Geometry = NavigationPrimitiveGeometry;
        namespace Errors = Navigation::NavigationErrors;

        /** @brief Produces a typed capture failure without partial geometry publication. */
        Result<Geometry> Failure(const ErrorCodeDescriptor &code, const char *message) {
            return Result<Geometry>::Failure(MakeError(code, message));
        }

        /** @brief Validates reducible capture limits before any geometry allocation. */
        bool ValidPolicy(const NavigationPrimitiveGeometryPolicy &policy) {
            const auto &limits = policy.storage;
            const bool validBounds =
                !policy.planeBounds || (Math::IsFinite(policy.planeBounds->minimum) && Math::IsFinite(policy.planeBounds->maximum) &&
                                        policy.planeBounds->minimum.x < policy.planeBounds->maximum.x &&
                                        policy.planeBounds->minimum.y < policy.planeBounds->maximum.y &&
                                        policy.planeBounds->minimum.z < policy.planeBounds->maximum.z);
            return validBounds && std::isfinite(policy.maximumChordErrorMeters) && policy.maximumChordErrorMeters > 0 &&
                   policy.maximumSegments >= 12 && policy.maximumSegments <= 1024 && limits.maxContributions > 0 &&
                   limits.maxContributions <= Navigation::NavigationSourceGeometryLimits::MaximumContributions && limits.maxVertices > 0 &&
                   limits.maxVertices <= Navigation::NavigationSourceGeometryLimits::MaximumVertices && limits.maxTriangles > 0 &&
                   limits.maxTriangles <= Navigation::NavigationSourceGeometryLimits::MaximumTriangles && limits.maxOwnedBytes > 0 &&
                   limits.maxOwnedBytes <= Navigation::NavigationSourceGeometryLimits::MaximumOwnedBytes;
        }

        /** @brief Admits both output arrays together before reserving either array. */
        bool Fits(std::uint64_t vertices, std::uint64_t triangles, const NavigationPrimitiveGeometryPolicy &policy) {
            return vertices <= policy.storage.maxVertices && triangles <= policy.storage.maxTriangles &&
                   vertices * sizeof(Math::Vec3) + triangles * sizeof(Navigation::NavigationSourceTriangleInput) <=
                       policy.storage.maxOwnedBytes;
        }

        /** @brief Computes a conservative rounded-surface resolution; zero means the policy cannot admit it. */
        std::uint32_t SegmentCount(float radius, const NavigationPrimitiveGeometryPolicy &policy) {
            const double cosine = std::clamp(1.0 - static_cast<double>(policy.maximumChordErrorMeters) / (4.0 * radius), -1.0, 1.0);
            const double angle = 2.0 * std::acos(cosine);
            if (!(angle > 0))
                return 0;
            const double required = std::ceil(2.0 * std::numbers::pi / angle);
            if (!std::isfinite(required) || required > policy.maximumSegments)
                return 0;
            const auto count = (std::max(12U, static_cast<std::uint32_t>(required)) + 3U) / 4U * 4U;
            return count <= policy.maximumSegments ? count : 0;
        }

        /** @brief Appends exact box topology in outward winding. */
        void Box(Geometry &output, Math::Vec3 half) {
            output.vertices = {{-half.x, -half.y, -half.z}, {half.x, -half.y, -half.z}, {half.x, half.y, -half.z},
                               {-half.x, half.y, -half.z},  {-half.x, -half.y, half.z}, {half.x, -half.y, half.z},
                               {half.x, half.y, half.z},    {-half.x, half.y, half.z}};
            constexpr std::array<std::array<std::uint32_t, 3>, 12> faces{{{0, 3, 2},
                                                                          {0, 2, 1},
                                                                          {4, 5, 6},
                                                                          {4, 6, 7},
                                                                          {0, 4, 7},
                                                                          {0, 7, 3},
                                                                          {1, 2, 6},
                                                                          {1, 6, 5},
                                                                          {0, 1, 5},
                                                                          {0, 5, 4},
                                                                          {3, 7, 6},
                                                                          {3, 6, 2}}};
            for (const auto &face : faces)
                output.triangles.push_back({face, {}, {}});
        }

        /** @brief Generates latitude rings and exact poles for a sphere or Y-axis capsule. */
        bool Rounded(Geometry &output, float radius, float halfHeight, std::uint32_t segments, CancellationToken cancellation) {
            const auto quarter = segments / 4;
            const auto rows = halfHeight > 0 ? segments / 2 : segments / 2 - 1;
            output.vertices.push_back({0, radius + halfHeight, 0});
            for (std::uint32_t row = 0; row < rows; ++row) {
                if (cancellation.IsCancellationRequested())
                    return false;
                const bool lower = halfHeight > 0 && row >= quarter;
                const auto latitude = row + 1 - (lower ? 1U : 0U);
                const double theta = static_cast<double>(latitude) * std::numbers::pi / (2.0 * quarter);
                const double ringRadius = radius * std::sin(theta);
                const auto y = static_cast<float>(radius * std::cos(theta) + (lower ? -halfHeight : halfHeight));
                for (std::uint32_t column = 0; column < segments; ++column) {
                    const double phi = static_cast<double>(column) * 2.0 * std::numbers::pi / segments;
                    output.vertices.push_back(
                        {static_cast<float>(ringRadius * std::cos(phi)), y, static_cast<float>(ringRadius * std::sin(phi))});
                }
            }
            const auto bottom = static_cast<std::uint32_t>(output.vertices.size());
            output.vertices.push_back({0, -radius - halfHeight, 0});
            const auto triangle = [&output](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
                output.triangles.push_back({{a, b, c}, {}, {}});
            };
            for (std::uint32_t column = 0; column < segments; ++column) {
                const auto next = (column + 1) % segments;
                triangle(0, 1 + next, 1 + column);
                triangle(1 + (rows - 1) * segments + column, 1 + (rows - 1) * segments + next, bottom);
            }
            for (std::uint32_t row = 0; row + 1 < rows; ++row) {
                if (cancellation.IsCancellationRequested())
                    return false;
                for (std::uint32_t column = 0; column < segments; ++column) {
                    const auto next = (column + 1) % segments;
                    const auto a = 1 + row * segments + column;
                    const auto b = 1 + row * segments + next;
                    const auto c = a + segments;
                    triangle(a, b, c);
                    triangle(b, b + segments, c);
                }
            }
            return true;
        }

        /** @brief Fixed stack storage for the at most twelve box-edge intersections and eight exact corners. */
        struct PlanePolygon final {
            std::array<Math::Vec3, 24> vertices;
            std::size_t count{};
        };

        /** @brief Intersects a plane with each box edge once; never allocates or assigns an implicit plane extent. */
        Result<PlanePolygon> IntersectPlane(Math::Vec3 normal, Math::Vec3 point, const Math::Aabb &bounds, CancellationToken cancellation) {
            const auto dot = [](Math::Vec3 a, Math::Vec3 b) {
                return static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z;
            };
            const double distance = dot(normal, point);
            std::array<Math::Vec3, 8> corners;
            for (std::size_t i = 0; i < corners.size(); ++i)
                corners[i] = {i & 1 ? bounds.maximum.x : bounds.minimum.x, i & 2 ? bounds.maximum.y : bounds.minimum.y,
                              i & 4 ? bounds.maximum.z : bounds.minimum.z};
            PlanePolygon intersection;
            auto &polygon = intersection.vertices;
            auto &count = intersection.count;
            const auto append = [&polygon, &count](Math::Vec3 vertex) {
                const auto end = polygon.begin() + static_cast<std::ptrdiff_t>(count);
                if (std::find(polygon.begin(), end, vertex) == end)
                    polygon[count++] = vertex;
            };
            for (std::size_t i = 0; i < corners.size(); ++i) {
                if (cancellation.IsCancellationRequested())
                    return Result<PlanePolygon>::Failure(MakeError(Errors::BakeInputCancelled, "Navigation plane capture cancelled"));
                const double a = dot(normal, corners[i]) - distance;
                if (a == 0)
                    append(corners[i]);
                for (const auto axis : {1U, 2U, 4U}) {
                    if ((i & axis) != 0)
                        continue;
                    const auto j = i | axis;
                    const double b = dot(normal, corners[j]) - distance;
                    if ((a < 0 && b > 0) || (a > 0 && b < 0)) {
                        const double t = a / (a - b);
                        const auto interpolate = [t](float start, float end) {
                            return static_cast<float>(static_cast<double>(start) + t * (static_cast<double>(end) - start));
                        };
                        append({interpolate(corners[i].x, corners[j].x), interpolate(corners[i].y, corners[j].y),
                                interpolate(corners[i].z, corners[j].z)});
                    }
                }
            }
            return Result<PlanePolygon>::Success(std::move(intersection));
        }

        /** @brief Orders a convex clipped polygon counterclockwise around its plane normal using bounded double projections. */
        void OrderPlanePolygon(std::span<Math::Vec3> polygon, Math::Vec3 normal) {
            std::array<double, 3> center{};
            for (const auto &vertex : polygon) {
                center[0] += vertex.x / static_cast<double>(polygon.size());
                center[1] += vertex.y / static_cast<double>(polygon.size());
                center[2] += vertex.z / static_cast<double>(polygon.size());
            }
            const auto tangent = Math::Cross(normal, std::abs(normal.x) < 0.5F ? Math::Vec3{1, 0, 0} : Math::Vec3{0, 1, 0});
            const auto bitangent = Math::Cross(normal, tangent);
            const auto angle = [&center, tangent, bitangent](Math::Vec3 vertex) {
                const auto project = [&center, vertex](Math::Vec3 axis) {
                    return (static_cast<double>(vertex.x) - center[0]) * axis.x + (static_cast<double>(vertex.y) - center[1]) * axis.y +
                           (static_cast<double>(vertex.z) - center[2]) * axis.z;
                };
                return std::atan2(project(bitangent), project(tangent));
            };
            std::ranges::sort(polygon, {}, angle);
        }

        /** @brief Clips an oriented world plane against finite bounds without allocating candidate intersections. */
        Result<Geometry> Plane(const Physics::PhysicsStaticPlaneShape &plane, const Physics::PhysicsPose &local,
                               const Physics::PhysicsPose &world, const NavigationPrimitiveGeometryPolicy &policy,
                               CancellationToken cancellation) {
            if (!policy.planeBounds)
                return Failure(Errors::SourceGeometryInvalid,
                               "Infinite collision planes require explicit finite navigation clipping bounds");
            const auto normal = world.rotation.Rotate(local.rotation.Rotate(plane.normal));
            const double squaredNorm = static_cast<double>(plane.normal.x) * plane.normal.x +
                                       static_cast<double>(plane.normal.y) * plane.normal.y +
                                       static_cast<double>(plane.normal.z) * plane.normal.z;
            const double offset = static_cast<double>(plane.signedDistanceMeters) / squaredNorm;
            const Math::Vec3 localPoint{static_cast<float>(plane.normal.x * offset), static_cast<float>(plane.normal.y * offset),
                                        static_cast<float>(plane.normal.z * offset)};
            const auto point = world.rotation.Rotate(local.rotation.Rotate(localPoint) + local.translation) + world.translation;
            if (!Math::IsFinite(normal) || !Math::IsFinite(point))
                return Failure(Errors::SourceGeometryInvalid, "Navigation collision plane world coordinates overflow");
            auto intersection = IntersectPlane(normal, point, *policy.planeBounds, cancellation);
            if (intersection.HasError())
                return Result<Geometry>::Failure(intersection.ErrorValue());
            const auto count = intersection.Value().count;
            auto polygon = std::move(intersection).Value().vertices;
            Geometry output;
            if (count < 3)
                return Result<Geometry>::Success(std::move(output));
            if (!Fits(count, count - 2, policy))
                return Failure(Errors::CapacityExceeded, "Clipped navigation plane exceeds capture storage limits");
            OrderPlanePolygon(std::span<Math::Vec3>{polygon}.first(count), normal);
            output.vertices.assign(polygon.begin(), polygon.begin() + static_cast<std::ptrdiff_t>(count));
            output.triangles.reserve(count - 2);
            for (std::uint32_t i = 1; i + 1 < count; ++i)
                output.triangles.push_back({{0, i, i + 1}, {}, {}});
            return Result<Geometry>::Success(std::move(output));
        }

        /** @brief Admits finite topology sizes before allocating and generating any output. */
        Result<Geometry> FiniteGeometry(const Physics::PhysicsShapeDescriptor &descriptor, const NavigationPrimitiveGeometryPolicy &policy,
                                        CancellationToken cancellation) {
            const auto *box = std::get_if<Physics::PhysicsBoxShape>(&descriptor);
            const auto *sphere = std::get_if<Physics::PhysicsSphereShape>(&descriptor);
            const auto *capsule = std::get_if<Physics::PhysicsCapsuleShape>(&descriptor);
            float radius = 0;
            float halfHeight = 0;
            if (sphere)
                radius = sphere->radiusMeters;
            else if (capsule) {
                radius = capsule->radiusMeters;
                halfHeight = capsule->cylindricalHalfHeightMeters;
            } else if (!box)
                return Failure(Errors::OperationUnsupported, "Unsupported finite navigation primitive");
            const auto segments = box ? 0 : SegmentCount(radius, policy);
            if (!box && segments == 0)
                return Failure(Errors::CapacityExceeded, "Navigation primitive chord tolerance exceeds the segment budget");
            const std::uint64_t rows = segments / 2 - (sphere ? 1 : 0);
            const std::uint64_t vertexCount = box ? 8 : 2 + rows * segments;
            const std::uint64_t triangleCount = box ? 12 : 2 * rows * segments;
            if (!Fits(vertexCount, triangleCount, policy))
                return Failure(Errors::CapacityExceeded, "Navigation primitive geometry exceeds capture storage limits");
            Geometry output;
            output.vertices.reserve(static_cast<std::size_t>(vertexCount));
            output.triangles.reserve(static_cast<std::size_t>(triangleCount));
            if (box)
                Box(output, box->halfExtentsMeters);
            else if (!Rounded(output, radius, halfHeight, segments, cancellation))
                return Failure(Errors::BakeInputCancelled, "Navigation primitive capture cancelled");
            return Result<Geometry>::Success(std::move(output));
        }

        /** @brief Hashes explicit little-endian scalars without native padding or a second geometry buffer. */
        void HashWord(Sha256Builder &hash, std::uint32_t word) {
            const std::array<std::byte, 4> bytes{static_cast<std::byte>(word), static_cast<std::byte>(word >> 8U),
                                                 static_cast<std::byte>(word >> 16U), static_cast<std::byte>(word >> 24U)};
            // Qualified geometry bounds are far below SHA-256's maximum input length.
            static_cast<void>(hash.Update(bytes));
        }

    }  // namespace

    /** @copydoc NavigationBakeDetail::IsValidGeometryPolicy */
    bool NavigationBakeDetail::IsValidGeometryPolicy(const NavigationPrimitiveGeometryPolicy &policy) noexcept {
        return ValidPolicy(policy);
    }

    /** @copydoc NavigationBakeDetail::TriangleHasArea */
    bool NavigationBakeDetail::TriangleHasArea(Math::Vec3 a, Math::Vec3 b, Math::Vec3 c) noexcept {
        const double ux = static_cast<double>(b.x) - a.x;
        const double uy = static_cast<double>(b.y) - a.y;
        const double uz = static_cast<double>(b.z) - a.z;
        const double vx = static_cast<double>(c.x) - a.x;
        const double vy = static_cast<double>(c.y) - a.y;
        const double vz = static_cast<double>(c.z) - a.z;
        return uy * vz - uz * vy != 0 || uz * vx - ux * vz != 0 || ux * vy - uy * vx != 0;
    }

    /** @copydoc NavigationBakeDetail::FinalizeGeometry */
    Result<NavigationPrimitiveGeometry> NavigationBakeDetail::FinalizeGeometry(Geometry output, const Physics::PhysicsPose &local,
                                                                               const Physics::PhysicsPose &world,
                                                                               Navigation::NavigationAreaId area,
                                                                               Navigation::NavigationSourceMaterialSlot material,
                                                                               CancellationToken cancellation) {
        Sha256Builder hash;
        HashWord(hash, 1);  // Version of this canonical analytic faceting policy.
        HashWord(hash, static_cast<std::uint32_t>(output.vertices.size()));
        HashWord(hash, static_cast<std::uint32_t>(output.triangles.size()));
        for (auto &vertex : output.vertices) {
            if (cancellation.IsCancellationRequested())
                return Failure(Errors::BakeInputCancelled, "Navigation primitive capture cancelled");
            vertex = world.rotation.Rotate(local.rotation.Rotate(vertex) + local.translation) + world.translation;
            if (!Math::IsFinite(vertex))
                return Failure(Errors::SourceGeometryInvalid, "Navigation primitive world coordinates overflow");
            // Signed zero is not a distinct geometric coordinate or cache identity.
            for (float *coordinate : {&vertex.x, &vertex.y, &vertex.z}) {
                if (*coordinate == 0)
                    *coordinate = 0;
                HashWord(hash, std::bit_cast<std::uint32_t>(*coordinate));
            }
        }
        for (auto &triangle : output.triangles) {
            if (cancellation.IsCancellationRequested())
                return Failure(Errors::BakeInputCancelled, "Navigation primitive capture cancelled");
            if (!TriangleHasArea(output.vertices[triangle.vertexIndices[0]], output.vertices[triangle.vertexIndices[1]],
                                 output.vertices[triangle.vertexIndices[2]]))
                return Failure(Errors::SourceGeometryInvalid, "Navigation primitive triangles collapse at the requested world precision");
            triangle.area = area;
            triangle.materialSlot = material;
            for (const auto index : triangle.vertexIndices)
                HashWord(hash, index);
            HashWord(hash, static_cast<std::uint32_t>(area.Value()));
            HashWord(hash, static_cast<std::uint32_t>(area.Value() >> 32U));
            HashWord(hash, material.value);
        }
        output.digest = hash.Finalize();
        return Result<Geometry>::Success(std::move(output));
    }

    /** @copydoc CaptureNavigationPrimitiveGeometry */
    Result<NavigationPrimitiveGeometry> CaptureNavigationPrimitiveGeometry(const Physics::PhysicsPrimitiveShapeRequest &primitive,
                                                                           const Physics::PhysicsPose &bodyWorldPose,
                                                                           Navigation::NavigationAreaId area,
                                                                           Navigation::NavigationSourceMaterialSlot material,
                                                                           const NavigationPrimitiveGeometryPolicy &policy,
                                                                           CancellationToken cancellation) {
        if (!ValidPolicy(policy) || !area.IsValid())
            return Failure(Errors::SourceGeometryInvalid, "Invalid navigation primitive policy or area identity");
        if (cancellation.IsCancellationRequested())
            return Failure(Errors::BakeInputCancelled, "Navigation primitive capture cancelled");
        if (auto pose = Physics::ValidatePhysicsPose(bodyWorldPose); pose.HasError())
            return Result<Geometry>::Failure(pose.ErrorValue());
        auto resolved = Physics::ResolvePhysicsPrimitiveShape(primitive);
        if (resolved.HasError())
            return Result<Geometry>::Failure(resolved.ErrorValue());
        const auto &shape = resolved.Value();
        try {
            if (const auto *plane = std::get_if<Physics::PhysicsStaticPlaneShape>(&shape.geometry)) {
                auto clipped = Plane(*plane, shape.localPose, bodyWorldPose, policy, cancellation);
                if (clipped.HasError())
                    return clipped;
                // Plane vertices have already been transformed to canonical world coordinates.
                return NavigationBakeDetail::FinalizeGeometry(std::move(clipped).Value(), {}, {}, area, material, cancellation);
            }
            auto geometry = FiniteGeometry(shape.geometry, policy, cancellation);
            if (geometry.HasError())
                return geometry;
            return NavigationBakeDetail::FinalizeGeometry(std::move(geometry).Value(), shape.localPose, bodyWorldPose, area, material,
                                                          cancellation);
        } catch (const std::bad_alloc &) {
            return Failure(Errors::CapacityExceeded, "Navigation primitive geometry allocation failed");
        }
    }
}  // namespace Horo::Application
