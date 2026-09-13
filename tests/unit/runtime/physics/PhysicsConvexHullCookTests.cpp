#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] Assets::AssetId Asset() {
            return Assets::AssetId::Parse("10000000-0000-0000-0000-000000000848").Value();
        }

        [[nodiscard]] PhysicsShapeCookTargetDigest Target(const std::uint8_t marker = 1) {
            PhysicsShapeCookTargetDigest target;
            target.digest.bytes[0] = marker;
            return target;
        }

        [[nodiscard]] std::array<Math::Vec3, 8> Cube() {
            return {{{-1, -1, -1}, {-1, -1, 1}, {-1, 1, -1}, {-1, 1, 1}, {1, -1, -1}, {1, -1, 1}, {1, 1, -1}, {1, 1, 1}}};
        }

        [[nodiscard]] PhysicsConvexHullCookRequest Request(const std::span<const Math::Vec3> vertices) {
            return {.asset = Asset(),
                    .subresource = PhysicsShapeSubresourceId::FromValue(3),
                    .vertices = vertices,
                    .target = Target(),
                    .sourceContext = "assets/collision/crate.mesh#hull"};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        void RefreshPayloadDigest(PhysicsConvexHullCookResult &cooked) {
            cooked.descriptor.payloadDigest = ComputeSha256(std::as_bytes(std::span{cooked.payload}));
        }

        void WriteU32(std::vector<std::uint8_t> &payload, const std::size_t offset, const std::uint32_t value) {
            for (std::size_t byte = 0; byte < sizeof(value); ++byte)
                payload[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
        }
    }  // namespace

    TEST_CASE("Convex cook produces a deterministic key and byte-equivalent artifact", "[unit][physics][shape][convex_cook]") {
        const auto cube = Cube();
        const auto request = Request(cube);
        const auto first = CookPhysicsConvexHull(request);
        const auto second = CookPhysicsConvexHull(request);

        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().descriptor.cacheKeyDigest == second.Value().descriptor.cacheKeyDigest);
        REQUIRE(first.Value().descriptor.payloadDigest == second.Value().descriptor.payloadDigest);
        REQUIRE(first.Value().payload == second.Value().payload);
        REQUIRE(first.Value().sourceDigest == second.Value().sourceDigest);
        REQUIRE(first.Value().sourceVertexCount == cube.size());
        REQUIRE(first.Value().hullVertexCount == cube.size());
        REQUIRE(first.Value().triangleCount == 12);
        REQUIRE(first.Value().bounds.minimum == Math::Vec3{-1, -1, -1});
        REQUIRE(first.Value().bounds.maximum == Math::Vec3{1, 1, 1});
    }

    TEST_CASE("Convex cook key binds source settings target and stable subresource identity",
              "[unit][physics][shape][convex_cook][identity]") {
        auto cube = Cube();
        const auto baseline = CookPhysicsConvexHull(Request(cube)).Value();

        cube[7].x = 2;
        const auto changedSource = CookPhysicsConvexHull(Request(cube)).Value();
        REQUIRE(changedSource.sourceDigest != baseline.sourceDigest);
        REQUIRE(changedSource.descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);

        cube = Cube();
        auto changedSettingsRequest = Request(cube);
        changedSettingsRequest.settings.limits.maxSourceVertices = 65'535;
        const auto changedSettings = CookPhysicsConvexHull(changedSettingsRequest).Value();
        REQUIRE(changedSettings.descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);

        auto changedTargetRequest = Request(cube);
        changedTargetRequest.target = Target(2);
        const auto changedTarget = CookPhysicsConvexHull(changedTargetRequest).Value();
        REQUIRE(changedTarget.descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);

        auto changedSubresourceRequest = Request(cube);
        changedSubresourceRequest.subresource = PhysicsShapeSubresourceId::FromValue(4);
        const auto changedSubresource = CookPhysicsConvexHull(changedSubresourceRequest).Value();
        REQUIRE(changedSubresource.descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);
    }

    TEST_CASE("Convex cook rejects empty non-finite collinear and coplanar sources with context",
              "[unit][physics][shape][convex_cook][validation]") {
        const std::span<const Math::Vec3> empty;
        auto failure = CookPhysicsConvexHull(Request(empty));
        RequireError(failure, PhysicsErrors::ShapeCookSourceInvalid);
        REQUIRE(failure.ErrorValue().message.find("assets/collision/crate.mesh#hull") != std::string::npos);
        REQUIRE(failure.ErrorValue().message.find(Asset().ToString()) != std::string::npos);

        auto cube = Cube();
        cube[2].z = std::numeric_limits<float>::quiet_NaN();
        failure = CookPhysicsConvexHull(Request(cube));
        RequireError(failure, PhysicsErrors::ShapeCookSourceInvalid);
        REQUIRE(failure.ErrorValue().message.find("vertex 2") != std::string::npos);

        const std::array<Math::Vec3, 4> collinear{{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}}};
        RequireError(CookPhysicsConvexHull(Request(collinear)), PhysicsErrors::ShapeCookSourceInvalid);

        const std::array<Math::Vec3, 5> coplanar{{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5F, 0.5F, 0}}};
        RequireError(CookPhysicsConvexHull(Request(coplanar)), PhysicsErrors::ShapeCookSourceInvalid);
    }

    TEST_CASE("Convex cook limit policy fails explicitly without silent topology loss", "[unit][physics][shape][convex_cook][capacity]") {
        const auto cube = Cube();
        auto request = Request(cube);
        request.settings.limits.maxSourceVertices = 7;
        RequireError(CookPhysicsConvexHull(request), PhysicsErrors::ShapeCookLimitExceeded);

        request = Request(cube);
        request.settings.limits.maxHullVertices = 4;
        const auto outputLimit = CookPhysicsConvexHull(request);
        RequireError(outputLimit, PhysicsErrors::ShapeCookLimitExceeded);
        REQUIRE(outputLimit.ErrorValue().message.find("forbids simplification") != std::string::npos);

        request = Request(cube);
        request.settings.limitPolicy = PhysicsConvexHullLimitPolicy::Count;
        RequireError(CookPhysicsConvexHull(request), PhysicsErrors::ProfileUnsupported);

        request = Request(cube);
        request.settings.limits.maxPayloadBytes = 1;
        RequireError(CookPhysicsConvexHull(request), PhysicsErrors::ShapeCookLimitExceeded);
    }

    TEST_CASE("Convex cook observes cancellation before and during bounded work", "[unit][physics][shape][convex_cook][cancellation]") {
        const auto cube = Cube();
        CancellationSource source;
        source.RequestCancellation();
        RequireError(CookPhysicsConvexHull(Request(cube), source.Token()), PhysicsErrors::ShapeCookCancelled);
    }

    TEST_CASE("Packaged runtime loads owned convex tables without source mesh input or hull generation",
              "[unit][physics][shape][convex_cook][runtime_load]") {
        PhysicsConvexHullCookResult cooked;
        {
            const auto cube = Cube();
            cooked = CookPhysicsConvexHull(Request(cube)).Value();
        }

        const auto loaded = LoadCookedPhysicsConvexHull(cooked.descriptor, Target(), cooked.payload);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().vertices.size() == cooked.hullVertexCount);
        REQUIRE(loaded.Value().triangleIndices.size() == cooked.triangleCount * 3U);
        REQUIRE(loaded.Value().sourceDigest == cooked.sourceDigest);
        REQUIRE(loaded.Value().schemaVersion == PhysicsConvexHullCookSettings::CurrentSchemaVersion);
        REQUIRE(loaded.Value().algorithmVersion == PhysicsConvexHullCookSettings::CurrentAlgorithmVersion);
        REQUIRE(loaded.Value().bounds.minimum == cooked.bounds.minimum);
        REQUIRE(loaded.Value().bounds.maximum == cooked.bounds.maximum);
    }

    TEST_CASE("Runtime convex loading rejects corruption wrong target kind and allocation limits",
              "[unit][physics][shape][convex_cook][runtime_load][validation]") {
        const auto cube = Cube();
        const auto cooked = CookPhysicsConvexHull(Request(cube)).Value();

        auto corrupt = cooked.payload;
        corrupt.back() ^= 1U;
        RequireError(LoadCookedPhysicsConvexHull(cooked.descriptor, Target(), corrupt), PhysicsErrors::ShapeArtifactInvalid);

        auto unsupportedVersion = cooked;
        unsupportedVersion.payload[8] = 2;
        RefreshPayloadDigest(unsupportedVersion);
        RequireError(LoadCookedPhysicsConvexHull(unsupportedVersion.descriptor, Target(), unsupportedVersion.payload),
                     PhysicsErrors::ShapeArtifactInvalid);

        auto duplicateVertex = cooked;
        constexpr std::size_t VertexTableOffset = 172;
        std::ranges::copy_n(duplicateVertex.payload.begin() + VertexTableOffset, 12,
                            duplicateVertex.payload.begin() + VertexTableOffset + 12);
        RefreshPayloadDigest(duplicateVertex);
        RequireError(LoadCookedPhysicsConvexHull(duplicateVertex.descriptor, Target(), duplicateVertex.payload),
                     PhysicsErrors::ShapeArtifactInvalid);

        auto negativeZero = cooked;
        constexpr std::array<std::uint8_t, 4> negativeZeroBits{0, 0, 0, 0x80};
        std::ranges::copy(negativeZeroBits, negativeZero.payload.begin() + VertexTableOffset);
        RefreshPayloadDigest(negativeZero);
        RequireError(LoadCookedPhysicsConvexHull(negativeZero.descriptor, Target(), negativeZero.payload),
                     PhysicsErrors::ShapeArtifactInvalid);

        RequireError(LoadCookedPhysicsConvexHull(cooked.descriptor, Target(2), cooked.payload), PhysicsErrors::ProfileUnsupported);

        auto wrongKind = cooked.descriptor;
        wrongKind.kind = PhysicsCookedShapeKind::TriangleMesh;
        RequireError(LoadCookedPhysicsConvexHull(wrongKind, Target(), cooked.payload), PhysicsErrors::ShapeArtifactInvalid);

        auto limits = PhysicsConvexHullCookLimits{};
        limits.maxHullVertices = 4;
        RequireError(LoadCookedPhysicsConvexHull(cooked.descriptor, Target(), cooked.payload, limits), PhysicsErrors::ShapeArtifactInvalid);
    }

    TEST_CASE("Runtime convex loading rejects non-manifold non-convex and inward triangle tables",
              "[unit][physics][shape][convex_cook][runtime_load][validation]") {
        const auto cube = Cube();
        const auto cooked = CookPhysicsConvexHull(Request(cube)).Value();
        constexpr std::size_t TriangleCountOffset = 144;
        constexpr std::size_t VertexTableOffset = 172;
        constexpr std::size_t TriangleTableOffset = VertexTableOffset + 8U * 3U * sizeof(float);

        auto nonManifold = cooked;
        WriteU32(nonManifold.payload, TriangleCountOffset, cooked.triangleCount - 1U);
        nonManifold.payload.resize(nonManifold.payload.size() - 3U * sizeof(std::uint32_t));
        RefreshPayloadDigest(nonManifold);
        const auto nonManifoldLoad = LoadCookedPhysicsConvexHull(nonManifold.descriptor, Target(), nonManifold.payload);
        RequireError(nonManifoldLoad, PhysicsErrors::ShapeArtifactInvalid);
        REQUIRE(nonManifoldLoad.ErrorValue().message.find("non-manifold") != std::string::npos);

        auto nonConvex = cooked;
        constexpr std::size_t SecondVertexZOffset = VertexTableOffset + sizeof(Math::Vec3) + 2U * sizeof(float);
        WriteU32(nonConvex.payload, SecondVertexZOffset, 0U);
        RefreshPayloadDigest(nonConvex);
        const auto nonConvexLoad = LoadCookedPhysicsConvexHull(nonConvex.descriptor, Target(), nonConvex.payload);
        RequireError(nonConvexLoad, PhysicsErrors::ShapeArtifactInvalid);
        REQUIRE(nonConvexLoad.ErrorValue().message.find("not convex") != std::string::npos);

        auto inward = cooked;
        std::swap_ranges(inward.payload.begin() + TriangleTableOffset + sizeof(std::uint32_t),
                         inward.payload.begin() + TriangleTableOffset + 2U * sizeof(std::uint32_t),
                         inward.payload.begin() + TriangleTableOffset + 2U * sizeof(std::uint32_t));
        RefreshPayloadDigest(inward);
        const auto inwardLoad = LoadCookedPhysicsConvexHull(inward.descriptor, Target(), inward.payload);
        RequireError(inwardLoad, PhysicsErrors::ShapeArtifactInvalid);
        REQUIRE(inwardLoad.ErrorValue().message.find("winding") != std::string::npos);
    }
}  // namespace Horo::Physics
