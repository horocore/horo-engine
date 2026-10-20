#include "Horo/Application/NavigationPrimitiveGeometry.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

using namespace Horo;
using namespace Horo::Application;

namespace {
    Navigation::NavigationAreaId Area(std::uint64_t value = 7) {
        return Navigation::NavigationAreaId::Create(value).Value();
    }

    Physics::PhysicsPrimitiveShapeRequest Primitive(Physics::PhysicsShapeDescriptor shape) {
        return {std::move(shape), {}, {}};
    }

    /** @brief Checks a closed oriented two-manifold and positive volume rather than matching implementation indices. */
    void CheckClosed(const NavigationPrimitiveGeometry &geometry) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
        double volume = 0;
        for (const auto &triangle : geometry.triangles) {
            CHECK(triangle.area == Area());
            CHECK(triangle.materialSlot.value == 3);
            for (const auto index : triangle.vertexIndices)
                REQUIRE(index < geometry.vertices.size());
            const auto &indices = triangle.vertexIndices;
            for (std::size_t i = 0; i < 3; ++i) {
                const auto a = indices[i];
                const auto b = indices[(i + 1) % 3];
                edges[{std::min(a, b), std::max(a, b)}] += a < b ? 1 : -1;
            }
            const auto a = geometry.vertices[indices[0]];
            const auto b = geometry.vertices[indices[1]];
            const auto c = geometry.vertices[indices[2]];
            const auto normal = Math::Cross(b - a, c - a);
            CHECK(Math::Dot(normal, a + b + c) > 0);
            volume += static_cast<double>(Math::Dot(a, Math::Cross(b, c))) / 6;
        }
        for (const auto &[edge, orientation] : edges) {
            static_cast<void>(edge);
            CHECK(orientation == 0);
        }
        CHECK(volume > 0);
        CHECK(static_cast<std::int64_t>(geometry.vertices.size()) - static_cast<std::int64_t>(edges.size()) +
                  static_cast<std::int64_t>(geometry.triangles.size()) ==
              2);
    }

    /** @brief Checks every clipped vertex and triangle against the declared cube and oriented plane equation. */
    void CheckPlane(const NavigationPrimitiveGeometry &geometry, Math::Vec3 expectedNormal, float expectedDistance) {
        for (const auto &vertex : geometry.vertices) {
            CHECK(vertex.x >= -2);
            CHECK(vertex.x <= 2);
            CHECK(vertex.y >= -2);
            CHECK(vertex.y <= 2);
            CHECK(vertex.z >= -2);
            CHECK(vertex.z <= 2);
            CHECK(Math::Dot(vertex, expectedNormal) == Catch::Approx(expectedDistance).margin(0.000002));
        }
        for (const auto &triangle : geometry.triangles) {
            const auto a = geometry.vertices[triangle.vertexIndices[0]];
            const auto b = geometry.vertices[triangle.vertexIndices[1]];
            const auto c = geometry.vertices[triangle.vertexIndices[2]];
            CHECK(Math::Dot(Math::Cross(b - a, c - a), expectedNormal) > 0);
        }
    }
}  // namespace

TEST_CASE("Offline analytic navigation geometry is closed outward-wound and preserves source semantics",
          "[navigation][geometry][automation]") {
    const Physics::PhysicsShapeDescriptor shape = GENERATE(Physics::PhysicsShapeDescriptor{Physics::PhysicsBoxShape{{1, 2, 3}}},
                                                           Physics::PhysicsShapeDescriptor{Physics::PhysicsSphereShape{2}},
                                                           Physics::PhysicsShapeDescriptor{Physics::PhysicsCapsuleShape{1, 2}});
    const auto geometry = CaptureNavigationPrimitiveGeometry(Primitive(shape), {}, Area(), {3});
    REQUIRE(geometry.HasValue());
    CheckClosed(geometry.Value());
}

TEST_CASE("Analytic navigation geometry folds scale before collider-local and body-world poses", "[navigation][geometry][automation]") {
    auto primitive = Primitive(Physics::PhysicsBoxShape{{1, 2, 3}});
    primitive.scale.factors = {2, 3, 4};
    primitive.localPose.translation = {1, 0, 0};
    primitive.localPose.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 2);
    Physics::PhysicsPose world{{10, 20, 30}, Math::Quaternion::FromAxisAngle({0, 0, 1}, std::numbers::pi_v<float> / 2)};
    const auto output = CaptureNavigationPrimitiveGeometry(primitive, world, Area(), {3});
    REQUIRE(output.HasValue());
    REQUIRE(output.Value().vertices.size() == 8);
    const auto first = output.Value().vertices.front();
    CHECK(first.x == Catch::Approx(16));
    CHECK(first.y == Catch::Approx(9));
    CHECK(first.z == Catch::Approx(32));
}

TEST_CASE("Rounded navigation facets meet the requested geometric error at triangle interiors", "[navigation][geometry][automation]") {
    NavigationPrimitiveGeometryPolicy policy;
    policy.maximumChordErrorMeters = GENERATE(0.1F, 0.01F, 0.001F);
    const bool capsule = GENERATE(false, true);
    const auto primitive = Primitive(capsule ? Physics::PhysicsShapeDescriptor{Physics::PhysicsCapsuleShape{2, 3}}
                                             : Physics::PhysicsShapeDescriptor{Physics::PhysicsSphereShape{2}});
    const auto geometry = CaptureNavigationPrimitiveGeometry(primitive, {}, Area(), {3}, policy);
    REQUIRE(geometry.HasValue());
    for (const auto &triangle : geometry.Value().triangles) {
        for (const auto weights :
             {Math::Vec3{1.0F / 3, 1.0F / 3, 1.0F / 3}, Math::Vec3{0.5F, 0.5F, 0}, Math::Vec3{0.5F, 0, 0.5F}, Math::Vec3{0, 0.5F, 0.5F}}) {
            const auto point = geometry.Value().vertices[triangle.vertexIndices[0]] * weights.x +
                               geometry.Value().vertices[triangle.vertexIndices[1]] * weights.y +
                               geometry.Value().vertices[triangle.vertexIndices[2]] * weights.z;
            const double dy = capsule ? std::max(std::abs(static_cast<double>(point.y)) - 3.0, 0.0) : point.y;
            const double distance = std::sqrt(static_cast<double>(point.x) * point.x + dy * dy + static_cast<double>(point.z) * point.z);
            CHECK(std::abs(distance - 2) <= policy.maximumChordErrorMeters + 0.000002);
        }
    }
}

TEST_CASE("Primitive capture fails before returning geometry for invalid policy cancellation scale and precision",
          "[navigation][geometry][automation]") {
    auto primitive = Primitive(Physics::PhysicsSphereShape{1});
    NavigationPrimitiveGeometryPolicy policy;
    Physics::PhysicsPose world;
    CancellationSource cancellation;
    SECTION("cancelled") {
        cancellation.RequestCancellation();
    }
    SECTION("invalid tolerance") {
        policy.maximumChordErrorMeters = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("unattainable tolerance") {
        policy.maximumChordErrorMeters = std::numeric_limits<float>::min();
    }
    SECTION("segment bound") {
        policy.maximumSegments = 13;
        policy.maximumChordErrorMeters = 0.00001F;
    }
    SECTION("vertex budget") {
        policy.storage.maxVertices = 3;
    }
    SECTION("triangle budget") {
        policy.storage.maxTriangles = 3;
    }
    SECTION("byte budget") {
        policy.storage.maxOwnedBytes = 3;
    }
    SECTION("unqualified budget") {
        policy.storage.maxVertices = 1'000'001;
    }
    SECTION("nonuniform sphere") {
        primitive.scale.factors.x = 2;
    }
    SECTION("nonfinite world") {
        world.translation.x = std::numeric_limits<float>::infinity();
    }
    SECTION("collapsed world precision") {
        world.translation = {1.0e30F, 1.0e30F, 1.0e30F};
    }
    SECTION("plane needs finite scope") {
        primitive.geometry = Physics::PhysicsStaticPlaneShape{};
    }
    CHECK(CaptureNavigationPrimitiveGeometry(primitive, world, Area(), {3}, policy, cancellation.Token()).HasError());
}

TEST_CASE("Infinite collision planes require explicit finite scope and preserve oriented world intersections",
          "[navigation][geometry][automation]") {
    auto primitive = Primitive(Physics::PhysicsStaticPlaneShape{});
    NavigationPrimitiveGeometryPolicy policy;
    policy.planeBounds = Math::Aabb{{-2, -2, -2}, {2, 2, 2}};
    Physics::PhysicsPose world;
    std::size_t expectedTriangles = 2;
    Math::Vec3 expectedNormal{0, 1, 0};
    float expectedDistance = 0;
    SECTION("axis aligned") {
        CHECK(world.translation == Math::Vec3{});
    }
    SECTION("coincident box face") {
        world.translation.y = 2;
        expectedDistance = 2;
    }
    SECTION("translated outside") {
        world.translation.y = 3;
        expectedTriangles = 0;
    }
    SECTION("edge contact") {
        primitive.geometry = Physics::PhysicsStaticPlaneShape{{std::sqrt(0.5F), std::sqrt(0.5F), 0}, 0};
        world.translation = {2, 2, 0};
        expectedTriangles = 0;
    }
    SECTION("rotated collider and body") {
        expectedTriangles = 4;
        primitive.localPose.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, std::numbers::pi_v<float> / 4);
        primitive.localPose.translation = {0, 0.5F, 0};
        world.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 4);
        world.translation = {0.25F, 0, 0};
        expectedNormal = world.rotation.Rotate(primitive.localPose.rotation.Rotate({0, 1, 0}));
        expectedDistance = Math::Dot(expectedNormal, world.rotation.Rotate(primitive.localPose.translation) + world.translation);
    }
    const auto geometry = CaptureNavigationPrimitiveGeometry(primitive, world, Area(), {3}, policy);
    REQUIRE(geometry.HasValue());
    CHECK(geometry.Value().triangles.size() == expectedTriangles);
    CheckPlane(geometry.Value(), expectedNormal, expectedDistance);
}

TEST_CASE("Geometry content hashing is stable and includes exact world geometry area and material", "[navigation][geometry][automation]") {
    const auto primitive = Primitive(Physics::PhysicsBoxShape{});
    const auto first = CaptureNavigationPrimitiveGeometry(primitive, {}, Area(), {3});
    const auto repeat = CaptureNavigationPrimitiveGeometry(primitive, {}, Area(), {3});
    REQUIRE(first.HasValue());
    REQUIRE(repeat.HasValue());
    CHECK(first.Value().digest == repeat.Value().digest);
    const auto differentArea = CaptureNavigationPrimitiveGeometry(primitive, {}, Area(8), {3});
    const auto differentMaterial = CaptureNavigationPrimitiveGeometry(primitive, {}, Area(), {4});
    const auto moved = CaptureNavigationPrimitiveGeometry(primitive, {{1, 0, 0}, {}}, Area(), {3});
    REQUIRE(differentArea.HasValue());
    REQUIRE(differentMaterial.HasValue());
    REQUIRE(moved.HasValue());
    CHECK(first.Value().digest != differentArea.Value().digest);
    CHECK(first.Value().digest != differentMaterial.Value().digest);
    CHECK(first.Value().digest != moved.Value().digest);
}

TEST_CASE("Job parent cancellation reaches primitive capture without a stop-token adapter", "[navigation][geometry][cancellation]") {
    CancellationSource parent;
    CancellationSource child{parent.Token()};
    parent.RequestCancellation();
    const auto result = CaptureNavigationPrimitiveGeometry(Primitive(Physics::PhysicsSphereShape{2}), {}, Area(), {3}, {}, child.Token());
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), Navigation::NavigationErrors::BakeInputCancelled.domain,
                             Navigation::NavigationErrors::BakeInputCancelled.code));
}
