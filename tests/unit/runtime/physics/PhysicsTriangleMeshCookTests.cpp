#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] Assets::AssetId Asset() {
            return Assets::AssetId::Parse("10000000-0000-0000-0000-000000000849").Value();
        }

        [[nodiscard]] PhysicsShapeCookTargetDigest Target(const std::uint8_t marker = 1) {
            PhysicsShapeCookTargetDigest target;
            target.digest.bytes[0] = marker;
            return target;
        }

        [[nodiscard]] std::array<Math::Vec3, 4> QuadVertices() {
            return {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}};
        }

        [[nodiscard]] std::array<PhysicsMaterialSlotId, 2> MaterialSlots() {
            return {PhysicsMaterialSlotId::FromValue(7), PhysicsMaterialSlotId::FromValue(9)};
        }

        [[nodiscard]] std::array<PhysicsTriangleMeshSourceTriangle, 2> QuadTriangles() {
            return {{{.vertexIndices = {0, 1, 2},
                      .subshape = PhysicsShapeSubresourceId::FromValue(101),
                      .materialSlot = PhysicsMaterialSlotId::FromValue(7)},
                     {.vertexIndices = {0, 2, 3},
                      .subshape = PhysicsShapeSubresourceId::FromValue(102),
                      .materialSlot = PhysicsMaterialSlotId::FromValue(9)}}};
        }

        [[nodiscard]] PhysicsTriangleMeshCookRequest Request(const std::span<const Math::Vec3> vertices,
                                                             const std::span<const PhysicsTriangleMeshSourceTriangle> triangles,
                                                             const std::span<const PhysicsMaterialSlotId> materialSlots) {
            return {.asset = Asset(),
                    .subresource = PhysicsShapeSubresourceId::FromValue(3),
                    .vertices = vertices,
                    .triangles = triangles,
                    .materialSlots = materialSlots,
                    .target = Target(),
                    .sourceContext = "assets/collision/level.mesh#static"};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        void RefreshPayloadDigest(PhysicsTriangleMeshCookResult &cooked) {
            cooked.descriptor.payloadDigest = ComputeSha256(std::as_bytes(std::span{cooked.payload}));
        }

        void WriteU64(std::vector<std::uint8_t> &payload, const std::size_t offset, const std::uint64_t value) {
            for (std::size_t byte = 0; byte < sizeof(value); ++byte)
                payload[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
        }
    }  // namespace

    TEST_CASE("Triangle-mesh cook is deterministic and preserves stable face material identity",
              "[unit][physics][shape][triangle_mesh_cook]") {
        const auto vertices = QuadVertices();
        const auto triangles = QuadTriangles();
        const auto slots = MaterialSlots();
        const auto request = Request(vertices, triangles, slots);
        const auto first = CookPhysicsTriangleMesh(request);
        const auto second = CookPhysicsTriangleMesh(request);

        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().payload == second.Value().payload);
        REQUIRE(first.Value().sourceDigest == second.Value().sourceDigest);
        REQUIRE(first.Value().descriptor.cacheKeyDigest == second.Value().descriptor.cacheKeyDigest);
        REQUIRE(first.Value().descriptor.payloadDigest == second.Value().descriptor.payloadDigest);
        REQUIRE(first.Value().descriptor.kind == PhysicsCookedShapeKind::TriangleMesh);
        REQUIRE(first.Value().vertexCount == 4);
        REQUIRE(first.Value().triangleCount == 2);
        REQUIRE(first.Value().materialSlotCount == 2);

        const auto loaded = LoadCookedPhysicsTriangleMesh(first.Value().descriptor, Target(), first.Value().payload);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().triangles[0].subshape == PhysicsShapeSubresourceId::FromValue(101));
        REQUIRE(loaded.Value().triangles[0].materialSlot == PhysicsMaterialSlotId::FromValue(7));
        REQUIRE(loaded.Value().triangles[1].subshape == PhysicsShapeSubresourceId::FromValue(102));
        REQUIRE(loaded.Value().triangles[1].materialSlot == PhysicsMaterialSlotId::FromValue(9));
    }

    TEST_CASE("Triangle-mesh cook key binds exact source settings target and asset-local identity",
              "[unit][physics][shape][triangle_mesh_cook][identity]") {
        auto vertices = QuadVertices();
        auto triangles = QuadTriangles();
        const auto slots = MaterialSlots();
        const auto baseline = CookPhysicsTriangleMesh(Request(vertices, triangles, slots)).Value();

        triangles[1].materialSlot = PhysicsMaterialSlotId::FromValue(7);
        const auto changedMapping = CookPhysicsTriangleMesh(Request(vertices, triangles, slots)).Value();
        REQUIRE(changedMapping.sourceDigest != baseline.sourceDigest);
        REQUIRE(changedMapping.descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);

        triangles = QuadTriangles();
        auto changedSettings = Request(vertices, triangles, slots);
        changedSettings.settings.limits.maxTriangles = 131'071;
        REQUIRE(CookPhysicsTriangleMesh(changedSettings).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);

        auto changedTarget = Request(vertices, triangles, slots);
        changedTarget.target = Target(2);
        REQUIRE(CookPhysicsTriangleMesh(changedTarget).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);

        auto changedSubresource = Request(vertices, triangles, slots);
        changedSubresource.subresource = PhysicsShapeSubresourceId::FromValue(4);
        REQUIRE(CookPhysicsTriangleMesh(changedSubresource).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);
    }

    TEST_CASE("Triangle-mesh cook rejects invalid vertices indices topology and material slots with stable diagnostics",
              "[unit][physics][shape][triangle_mesh_cook][validation]") {
        auto vertices = QuadVertices();
        auto triangles = QuadTriangles();
        auto slots = MaterialSlots();

        vertices[1].x = std::numeric_limits<float>::infinity();
        auto failure = CookPhysicsTriangleMesh(Request(vertices, triangles, slots));
        RequireError(failure, PhysicsErrors::ShapeCookSourceInvalid);
        REQUIRE(failure.ErrorValue().message.find("vertex 1") != std::string::npos);
        REQUIRE(failure.ErrorValue().message.find(Asset().ToString()) != std::string::npos);

        vertices = QuadVertices();
        triangles[0].vertexIndices[2] = 20;
        RequireError(CookPhysicsTriangleMesh(Request(vertices, triangles, slots)), PhysicsErrors::ShapeCookSourceInvalid);

        triangles = QuadTriangles();
        triangles[0].materialSlot = PhysicsMaterialSlotId::FromValue(10);
        failure = CookPhysicsTriangleMesh(Request(vertices, triangles, slots));
        RequireError(failure, PhysicsErrors::ShapeCookSourceInvalid);
        REQUIRE(failure.ErrorValue().message.find("undeclared material slot 10") != std::string::npos);

        triangles = QuadTriangles();
        triangles[1].subshape = triangles[0].subshape;
        RequireError(CookPhysicsTriangleMesh(Request(vertices, triangles, slots)), PhysicsErrors::ShapeCookSourceInvalid);

        triangles = QuadTriangles();
        triangles[1].vertexIndices = {0, 3, 2};
        RequireError(CookPhysicsTriangleMesh(Request(vertices, triangles, slots)), PhysicsErrors::ShapeCookSourceInvalid);

        slots[1] = slots[0];
        RequireError(CookPhysicsTriangleMesh(Request(vertices, QuadTriangles(), slots)), PhysicsErrors::ShapeCookSourceInvalid);
    }

    TEST_CASE("Triangle-mesh configured limits fail closed and cancellation publishes nothing",
              "[unit][physics][shape][triangle_mesh_cook][capacity]") {
        const auto vertices = QuadVertices();
        const auto triangles = QuadTriangles();
        const auto slots = MaterialSlots();
        auto request = Request(vertices, triangles, slots);
        request.settings.limits.maxTriangles = 1;
        RequireError(CookPhysicsTriangleMesh(request), PhysicsErrors::ShapeCookLimitExceeded);

        request = Request(vertices, triangles, slots);
        request.settings.limits.maxMaterialSlots = 1;
        RequireError(CookPhysicsTriangleMesh(request), PhysicsErrors::ShapeCookLimitExceeded);

        request = Request(vertices, triangles, slots);
        request.settings.limits.maxPayloadBytes = 200;
        RequireError(CookPhysicsTriangleMesh(request), PhysicsErrors::ShapeCookLimitExceeded);

        request = Request(vertices, triangles, slots);
        request.settings.limitPolicy = PhysicsTriangleMeshLimitPolicy::Count;
        RequireError(CookPhysicsTriangleMesh(request), PhysicsErrors::ProfileUnsupported);

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(CookPhysicsTriangleMesh(Request(vertices, triangles, slots), cancellation.Token()), PhysicsErrors::ShapeCookCancelled);
    }

    TEST_CASE("Triangle-mesh body admission rejects unsupported dynamic and kinematic motion with typed error",
              "[unit][physics][shape][triangle_mesh_cook][motion]") {
        REQUIRE(ValidatePhysicsTriangleMeshMotion(PhysicsMotionType::Static).HasValue());
        RequireError(ValidatePhysicsTriangleMeshMotion(PhysicsMotionType::Kinematic), PhysicsErrors::ShapeMotionUnsupported);
        RequireError(ValidatePhysicsTriangleMeshMotion(PhysicsMotionType::Dynamic), PhysicsErrors::ShapeMotionUnsupported);
        RequireError(ValidatePhysicsTriangleMeshMotion(static_cast<PhysicsMotionType>(100)), PhysicsErrors::OperationUnsupported);
    }

    TEST_CASE("Packaged runtime loads triangle identity without source mesh parsing or cooking",
              "[unit][physics][shape][triangle_mesh_cook][runtime_load]") {
        PhysicsTriangleMeshCookResult cooked;
        {
            const auto vertices = QuadVertices();
            const auto triangles = QuadTriangles();
            const auto slots = MaterialSlots();
            cooked = CookPhysicsTriangleMesh(Request(vertices, triangles, slots)).Value();
        }

        const auto loaded = LoadCookedPhysicsTriangleMesh(cooked.descriptor, Target(), cooked.payload);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().vertices.size() == cooked.vertexCount);
        REQUIRE(loaded.Value().triangles.size() == cooked.triangleCount);
        REQUIRE(loaded.Value().materialSlots.size() == cooked.materialSlotCount);
        REQUIRE(loaded.Value().sourceDigest == cooked.sourceDigest);
        REQUIRE(loaded.Value().schemaVersion == PhysicsTriangleMeshCookSettings::CurrentSchemaVersion);
        REQUIRE(loaded.Value().algorithmVersion == PhysicsTriangleMeshCookSettings::CurrentAlgorithmVersion);
        REQUIRE(loaded.Value().bounds.minimum == cooked.bounds.minimum);
        REQUIRE(loaded.Value().bounds.maximum == cooked.bounds.maximum);
    }

    TEST_CASE("Runtime triangle-mesh loading rejects corruption identity mismatches and allocation limits",
              "[unit][physics][shape][triangle_mesh_cook][runtime_load][validation]") {
        const auto vertices = QuadVertices();
        const auto triangles = QuadTriangles();
        const auto slots = MaterialSlots();
        const auto cooked = CookPhysicsTriangleMesh(Request(vertices, triangles, slots)).Value();

        auto corrupt = cooked.payload;
        corrupt.back() ^= 1U;
        RequireError(LoadCookedPhysicsTriangleMesh(cooked.descriptor, Target(), corrupt), PhysicsErrors::ShapeArtifactInvalid);
        RequireError(LoadCookedPhysicsTriangleMesh(cooked.descriptor, Target(2), cooked.payload), PhysicsErrors::ProfileUnsupported);

        auto wrongKind = cooked.descriptor;
        wrongKind.kind = PhysicsCookedShapeKind::ConvexHull;
        RequireError(LoadCookedPhysicsTriangleMesh(wrongKind, Target(), cooked.payload), PhysicsErrors::ShapeArtifactInvalid);

        auto undeclaredMaterial = cooked;
        constexpr std::size_t FirstTriangleMaterialOffset = 180 + 4U * sizeof(Math::Vec3) + 2U * sizeof(std::uint64_t) + 20U;
        WriteU64(undeclaredMaterial.payload, FirstTriangleMaterialOffset, 99);
        RefreshPayloadDigest(undeclaredMaterial);
        RequireError(LoadCookedPhysicsTriangleMesh(undeclaredMaterial.descriptor, Target(), undeclaredMaterial.payload),
                     PhysicsErrors::ShapeArtifactInvalid);

        auto unorderedIdentity = cooked;
        constexpr std::size_t SecondTriangleSubshapeOffset = 180 + 4U * sizeof(Math::Vec3) + 2U * sizeof(std::uint64_t) + 28U;
        WriteU64(unorderedIdentity.payload, SecondTriangleSubshapeOffset, 100);
        RefreshPayloadDigest(unorderedIdentity);
        RequireError(LoadCookedPhysicsTriangleMesh(unorderedIdentity.descriptor, Target(), unorderedIdentity.payload),
                     PhysicsErrors::ShapeArtifactInvalid);

        auto limits = PhysicsTriangleMeshCookLimits{};
        limits.maxTriangles = 1;
        RequireError(LoadCookedPhysicsTriangleMesh(cooked.descriptor, Target(), cooked.payload, limits),
                     PhysicsErrors::ShapeArtifactInvalid);
    }
}  // namespace Horo::Physics
