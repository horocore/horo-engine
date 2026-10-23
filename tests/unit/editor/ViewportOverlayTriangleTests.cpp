#include "editor/renderer/EditorViewportScene.h"
#include "editor/renderer/grid/ViewportOverlayTriangles.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    TEST_CASE("Viewport overlay ribbons retain requested physical pixel width at different depths", "[editor][viewport][overlay]") {
        for (const float depth : {2.0F, 20.0F}) {
            const std::array positions{Math::Vec3{-0.5F, 0, -depth}, Math::Vec3{0.5F, 0, -depth}};
            const ViewportOverlayTriangleRequest request{{positions, {1, 1, 1}}, {}, {800, 600}, 3.0F};
            std::array<Render::MeshVertex, 6> vertices;
            std::size_t count{};
            REQUIRE(BuildViewportOverlayTriangles(request, vertices, count));
            REQUIRE(count == 6);
            const auto matrix = BuildRenderMvp(request.camera, Math::Mat4::Identity(), 800.0F / 600, Math::ClipDepthRange::ZeroToOne);
            REQUIRE(matrix.HasValue());
            const auto first = Math::TransformPoint(matrix.Value(), vertices[0].position);
            const auto other = Math::TransformPoint(matrix.Value(), vertices[2].position);
            CHECK(std::fabs(std::fabs(first.y - other.y) * 300 - 3.0F) < 0.001F);
        }
    }

    TEST_CASE("Viewport overlay clipping preserves finite near-plane crossing and removes behind-camera lines",
              "[editor][viewport][overlay]") {
        std::array positions{Math::Vec3{-0.5F, 0, 1}, Math::Vec3{0.5F, 0, -5}};
        ViewportOverlayTriangleRequest request{{positions, {1, 1, 1}}, {}, {800, 600}, 1};
        std::array<Render::MeshVertex, 6> vertices;
        std::size_t count{};
        REQUIRE(BuildViewportOverlayTriangles(request, vertices, count));
        REQUIRE(count == 6);
        for (const auto &vertex : vertices)
            CHECK(Math::IsFinite(vertex.position));
        positions[1].z = 2;
        REQUIRE(BuildViewportOverlayTriangles(request, vertices, count));
        CHECK(count == 0);
    }

    TEST_CASE("Viewport overlay rejects invalid and insufficient storage without partial geometry", "[editor][viewport][overlay]") {
        const std::array positions{Math::Vec3{-1, 0, -5}, Math::Vec3{1, 0, -5}};
        ViewportOverlayTriangleRequest request{{positions, {1, 1, 1}}, {}, {800, 600}, 1};
        std::array<Render::MeshVertex, 6> vertices;
        std::size_t count{99};
        SECTION("Capacity") {
            CHECK_FALSE(BuildViewportOverlayTriangles(request, std::span{vertices}.first(3), count));
        }
        SECTION("Unavailable extent") {
            request.extent = {};
            CHECK_FALSE(BuildViewportOverlayTriangles(request, vertices, count));
        }
        SECTION("Nonfinite width") {
            request.lineWidthPixels = std::numeric_limits<float>::infinity();
            CHECK_FALSE(BuildViewportOverlayTriangles(request, vertices, count));
        }
        CHECK(count == 0);
        CHECK(vertices[0].position == Math::Vec3{});
    }

    TEST_CASE("Viewport overlay copies shared grid triangles without changing their world positions", "[editor][viewport][overlay]") {
        const std::array positions{Math::Vec3{-1, 0, -5}, Math::Vec3{1, 0, -5}, Math::Vec3{0, 0, -6}};
        const ViewportOverlayTriangleRequest request{{positions, {1, 1, 1}, ViewportGridPrimitiveTopology::Triangles}, {}, {800, 600}, 1};
        std::array<Render::MeshVertex, 6> vertices;
        std::size_t count{};
        REQUIRE(BuildViewportOverlayTriangles(request, vertices, count));
        REQUIRE(count == positions.size());
        for (std::size_t index = 0; index < count; ++index)
            CHECK(vertices[index].position == positions[index]);
        CHECK(vertices[3].position == Math::Vec3{});
    }
}  // namespace
