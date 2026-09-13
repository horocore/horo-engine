#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsCookedShapeCache.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] Assets::AssetId Asset(const char marker) {
            std::string text = "20000000-0000-0000-0000-00000000085";
            text.push_back(marker);
            return Assets::AssetId::Parse(text).Value();
        }

        [[nodiscard]] PhysicsShapeCookTargetDigest Target(const std::uint8_t marker = 1) {
            Sha256Digest digest;
            digest.bytes.front() = marker;
            return PhysicsShapeCookTargetDigest{digest};
        }

        [[nodiscard]] PhysicsConvexHullCookResult CookCube(const char assetMarker = '2', const float extent = 1.0F) {
            const std::array<Math::Vec3, 8> vertices{{{-extent, -extent, -extent},
                                                      {-extent, -extent, extent},
                                                      {-extent, extent, -extent},
                                                      {-extent, extent, extent},
                                                      {extent, -extent, -extent},
                                                      {extent, -extent, extent},
                                                      {extent, extent, -extent},
                                                      {extent, extent, extent}}};
            return CookPhysicsConvexHull({.asset = Asset(assetMarker),
                                          .subresource = PhysicsShapeSubresourceId::FromValue(7),
                                          .vertices = vertices,
                                          .target = Target(),
                                          .sourceContext = "cache-test-cube"})
                .Value();
        }

        [[nodiscard]] PhysicsTriangleMeshCookResult CookQuad() {
            const std::array<Math::Vec3, 4> vertices{{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}};
            const std::array<PhysicsMaterialSlotId, 1> materials{PhysicsMaterialSlotId::FromValue(5)};
            const std::array<PhysicsTriangleMeshSourceTriangle, 2> triangles{{
                {.vertexIndices = {0, 1, 2}, .subshape = PhysicsShapeSubresourceId::FromValue(11), .materialSlot = materials[0]},
                {.vertexIndices = {0, 2, 3}, .subshape = PhysicsShapeSubresourceId::FromValue(12), .materialSlot = materials[0]},
            }};
            return CookPhysicsTriangleMesh({.asset = Asset('4'),
                                            .subresource = PhysicsShapeSubresourceId::FromValue(8),
                                            .vertices = vertices,
                                            .triangles = triangles,
                                            .materialSlots = materials,
                                            .target = Target(),
                                            .sourceContext = "cache-test-quad"})
                .Value();
        }

        [[nodiscard]] PhysicsCookedShapeCache OneShapeCache() {
            return PhysicsCookedShapeCache::Create(Target(), {.maximumShapes = 1, .maximumResidentBytes = MaximumPhysicsResidentShapeBytes})
                .Value();
        }

        template <typename T> [[nodiscard]] bool HasPhysicsError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            return result.HasError() && result.ErrorValue().code.Value() == descriptor.code.Value();
        }
    }  // namespace

    TEST_CASE("Cooked shape cache shares one exact immutable resource across callers", "[unit][physics][shape_cache]") {
        auto cache = PhysicsCookedShapeCache::Create(Target()).Value();
        const auto cooked = CookCube();

        auto first = cache.Acquire(cooked.descriptor, cooked.payload);
        auto second = cache.Acquire(cooked.descriptor, cooked.payload);

        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().SharesResourceWith(second.Value()));
        REQUIRE(first.Value().Descriptor().cacheKeyDigest == cooked.descriptor.cacheKeyDigest);
        REQUIRE(first.Value().ResidentBytes() > 0);
        REQUIRE(first.Value().ConvexHull() != nullptr);
        REQUIRE(first.Value().ConvexHull()->vertices.size() == cooked.hullVertexCount);
        REQUIRE(first.Value().TriangleMesh() == nullptr);
        REQUIRE(cache.Stats().residentShapes == 1);
        REQUIRE(cache.Stats().residentBytes == first.Value().ResidentBytes());

        auto irrelevantOnHit = cooked.payload;
        irrelevantOnHit.back() ^= 1U;
        auto hit = cache.Acquire(cooked.descriptor, irrelevantOnHit);
        REQUIRE(hit.HasValue());
        REQUIRE(first.Value().SharesResourceWith(hit.Value()));
    }

    TEST_CASE("Concurrent cold acquisition publishes only one shared resource", "[unit][physics][shape_cache][thread]") {
        auto cache = PhysicsCookedShapeCache::Create(Target()).Value();
        const auto cooked = CookCube();
        std::optional<PhysicsCookedShapeLease> first;
        std::optional<PhysicsCookedShapeLease> second;
        std::barrier start{3};
        bool firstSucceeded = false;
        bool secondSucceeded = false;

        std::thread firstWorker([&] {
            start.arrive_and_wait();
            auto acquired = cache.Acquire(cooked.descriptor, cooked.payload);
            firstSucceeded = acquired.HasValue();
            if (firstSucceeded)
                first.emplace(std::move(acquired).Value());
        });
        std::thread secondWorker([&] {
            start.arrive_and_wait();
            auto acquired = cache.Acquire(cooked.descriptor, cooked.payload);
            secondSucceeded = acquired.HasValue();
            if (secondSucceeded)
                second.emplace(std::move(acquired).Value());
        });
        start.arrive_and_wait();
        firstWorker.join();
        secondWorker.join();

        REQUIRE(firstSucceeded);
        REQUIRE(secondSucceeded);
        REQUIRE(first->SharesResourceWith(*second));
        REQUIRE(cache.Stats().residentShapes == 1);
    }

    TEST_CASE("Cooked shape cache owns verified static triangle identity tables", "[unit][physics][shape_cache][triangle]") {
        auto cache = PhysicsCookedShapeCache::Create(Target()).Value();
        const auto cooked = CookQuad();

        auto lease = cache.Acquire(cooked.descriptor, cooked.payload);

        REQUIRE(lease.HasValue());
        REQUIRE(lease.Value().ConvexHull() == nullptr);
        REQUIRE(lease.Value().TriangleMesh() != nullptr);
        REQUIRE(lease.Value().TriangleMesh()->triangles.size() == cooked.triangleCount);
        REQUIRE(lease.Value().TriangleMesh()->triangles[1].subshape == PhysicsShapeSubresourceId::FromValue(12));
    }

    TEST_CASE("Cooked shape cache validates exact payload target kind and configured limits", "[unit][physics][shape_cache][validation]") {
        REQUIRE(PhysicsCookedShapeCache::Create(Target(), {.maximumShapes = 0, .maximumResidentBytes = 1}).HasError());
        auto cache = PhysicsCookedShapeCache::Create(Target(), {.maximumShapes = 1, .maximumResidentBytes = 1}).Value();
        const auto cooked = CookCube();
        REQUIRE(HasPhysicsError(cache.Acquire(cooked.descriptor, cooked.payload), PhysicsErrors::CapacityExceeded));
        REQUIRE(cache.Stats().residentShapes == 0);

        cache = PhysicsCookedShapeCache::Create(Target()).Value();
        auto corrupt = cooked.payload;
        corrupt.back() ^= 1U;
        REQUIRE(HasPhysicsError(cache.Acquire(cooked.descriptor, corrupt), PhysicsErrors::ShapeArtifactInvalid));

        auto wrongTarget = cooked.descriptor;
        wrongTarget.target = Target(2);
        REQUIRE(HasPhysicsError(cache.Acquire(wrongTarget, cooked.payload), PhysicsErrors::ProfileUnsupported));

        auto unsupported = cooked.descriptor;
        unsupported.kind = PhysicsCookedShapeKind::HeightField;
        REQUIRE(HasPhysicsError(cache.Acquire(unsupported, cooked.payload), PhysicsErrors::OperationUnsupported));
        REQUIRE(cache.Stats().residentShapes == 0);
    }

    TEST_CASE("Active leases block budget eviction and explicit eviction only drops the cache retain",
              "[unit][physics][shape_cache][lifetime]") {
        auto cache = OneShapeCache();
        const auto firstCook = CookCube('2', 1.0F);
        const auto secondCook = CookCube('3', 2.0F);
        auto first = cache.Acquire(firstCook.descriptor, firstCook.payload).Value();

        REQUIRE(HasPhysicsError(cache.Acquire(secondCook.descriptor, secondCook.payload), PhysicsErrors::CapacityExceeded));
        REQUIRE(cache.Stats().residentShapes == 1);
        REQUIRE(cache.Evict(firstCook.descriptor).Value());
        REQUIRE(cache.Stats().residentShapes == 0);
        REQUIRE(first);
        REQUIRE(first.Descriptor().payloadDigest == firstCook.descriptor.payloadDigest);

        auto second = cache.Acquire(secondCook.descriptor, secondCook.payload);
        REQUIRE(second.HasValue());
        REQUIRE_FALSE(first.SharesResourceWith(second.Value()));
    }

    TEST_CASE("Cooked shape cache evicts the least-recently-used unleased resource", "[unit][physics][shape_cache][eviction]") {
        auto cache = OneShapeCache();
        const auto firstCook = CookCube('2', 1.0F);
        const auto secondCook = CookCube('3', 2.0F);
        {
            auto first = cache.Acquire(firstCook.descriptor, firstCook.payload);
            REQUIRE(first.HasValue());
        }

        auto second = cache.Acquire(secondCook.descriptor, secondCook.payload);
        REQUIRE(second.HasValue());
        REQUIRE(cache.Stats().residentShapes == 1);
        REQUIRE_FALSE(cache.Evict(firstCook.descriptor).Value());
        REQUIRE(cache.Evict(secondCook.descriptor).Value());
    }

    TEST_CASE("Shutdown is idempotent closes admission and preserves cross-thread leases",
              "[unit][physics][shape_cache][shutdown][thread]") {
        auto cache = PhysicsCookedShapeCache::Create(Target()).Value();
        const auto cooked = CookCube();
        auto ownerLease = cache.Acquire(cooked.descriptor, cooked.payload).Value();
        std::optional<PhysicsCookedShapeLease> workerLease;
        bool workerSucceeded = false;

        std::thread worker([&] {
            auto acquired = cache.Acquire(cooked.descriptor, cooked.payload);
            workerSucceeded = acquired.HasValue();
            if (workerSucceeded)
                workerLease.emplace(std::move(acquired).Value());
        });
        worker.join();

        REQUIRE(workerSucceeded);
        REQUIRE(workerLease.has_value());
        REQUIRE(ownerLease.SharesResourceWith(*workerLease));
        REQUIRE(cache.Shutdown().HasValue());
        REQUIRE(cache.Shutdown().HasValue());
        REQUIRE(cache.Stats() == PhysicsCookedShapeCacheStats{0, 0, true});
        REQUIRE(ownerLease);
        REQUIRE(ownerLease.Descriptor().kind == PhysicsCookedShapeKind::ConvexHull);
        workerLease.reset();
        REQUIRE(HasPhysicsError(cache.Acquire(cooked.descriptor, cooked.payload), PhysicsErrors::InvalidState));
        REQUIRE(HasPhysicsError(cache.Evict(cooked.descriptor), PhysicsErrors::InvalidState));
    }
}  // namespace Horo::Physics
