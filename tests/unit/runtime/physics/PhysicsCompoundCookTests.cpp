#include "Horo/Physics/PhysicsCompoundCook.h"
#include "Horo/Physics/PhysicsCookedShapeCache.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

namespace Horo::Physics {
    namespace {
        struct CompoundFixture final {
            PhysicsShapeCookTargetDigest target;
            PhysicsConvexHullCookResult hull;
            std::array<PhysicsCompoundCookChild, 2> children;
            PhysicsCompoundCookRequest request;

            CompoundFixture() {
                target.digest.bytes[0] = 19;
                const auto asset = Assets::AssetId::Parse("36cf9de0-5afd-4bf7-9f36-0dcb127fd784").Value();
                const std::array<Math::Vec3, 4> points{{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {0, 0, 4}}};
                auto cooked = CookPhysicsConvexHull(
                    {.asset = asset, .subresource = PhysicsShapeSubresourceId::FromValue(12), .vertices = points, .target = target});
                REQUIRE(cooked.HasValue());
                hull = std::move(cooked).Value();
                children = {
                    {{PhysicsShapeSubresourceId::FromValue(81), PhysicsMaterialSlotId::FromValue(5), hull.descriptor, hull.payload},
                     {PhysicsShapeSubresourceId::FromValue(27), PhysicsMaterialSlotId::FromValue(9), hull.descriptor, hull.payload}}};
                request.asset = asset;
                request.subresource = PhysicsShapeSubresourceId::FromValue(7);
                request.target = target;
                request.dependencyDigest.bytes[0] = 35;
                request.children = children;
            }
        };

        /** @brief Rebinds outer integrity to exercise semantic parser failures beyond the digest admission check. */
        void Rebind(PhysicsCompoundCookResult &artifact) {
            artifact.descriptor.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            Sha256Builder hash;
            constexpr std::array<std::uint8_t, 4> domain{'P', 'C', 'K', '1'};
            (void)hash.Update(std::as_bytes(std::span{domain}));
            (void)hash.Update(std::as_bytes(std::span{artifact.payload}));
            artifact.descriptor.cacheKeyDigest = hash.Finalize();
        }
    }  // namespace

    TEST_CASE("Compound artifact encodes stable child identity and source-free convex leaves", "[physics][compound]") {
        CompoundFixture fixture;
        const auto first = CookPhysicsCompound(fixture.request);
        REQUIRE(first.HasValue());
        std::ranges::reverse(fixture.children);
        const auto second = CookPhysicsCompound(fixture.request);
        REQUIRE(second.HasValue());
        CHECK(first.Value().payload == second.Value().payload);
        fixture.hull.payload.clear();
        const auto loaded = LoadCookedPhysicsCompound(first.Value().descriptor, fixture.target, first.Value().payload);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().children.size() == 2);
        CHECK(loaded.Value().children[0].id.Value() == 27);
        CHECK(loaded.Value().children[0].material.Value() == 9);
        CHECK(loaded.Value().children[1].id.Value() == 81);
        CHECK(loaded.Value().children[1].hull.vertices.size() == 4);
    }

    TEST_CASE("Compound cook fails explicitly for invalid children, budgets and cancellation", "[physics][compound]") {
        CompoundFixture fixture;
        SECTION("duplicate ID") {
            fixture.children[1].id = fixture.children[0].id;
        }
        SECTION("invalid material") {
            fixture.children[0].material = {};
        }
        SECTION("wrong target") {
            fixture.request.target.digest.bytes[0]++;
        }
        SECTION("corrupt child") {
            std::as_writable_bytes(std::span{fixture.hull.payload}).back() ^= std::byte{1};
        }
        SECTION("unsupported child kind") {
            fixture.children[0].descriptor.kind = PhysicsCookedShapeKind::TriangleMesh;
        }
        SECTION("nested compound") {
            fixture.children[0].descriptor.kind = PhysicsCookedShapeKind::Compound;
        }
        SECTION("child limit") {
            fixture.request.limits.maximumChildren = 1;
        }
        SECTION("byte limit") {
            fixture.request.limits.maximumPayloadBytes = 100;
        }
        SECTION("unqualified limits") {
            fixture.request.limits.maximumChildren = 257;
        }
        SECTION("missing asset") {
            fixture.request.asset = {};
        }
        SECTION("missing children") {
            fixture.request.children = {};
        }
        CHECK(CookPhysicsCompound(fixture.request).HasError());
        CancellationSource source;
        source.RequestCancellation();
        auto cancelled = CookPhysicsCompound(fixture.request, source.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == PhysicsErrors::ShapeCookCancelled.code.Value());
    }

    TEST_CASE("Compound loader rejects hostile envelopes even with rebound outer digests", "[physics][compound]") {
        CompoundFixture fixture;
        auto cooked = CookPhysicsCompound(fixture.request);
        REQUIRE(cooked.HasValue());
        auto artifact = std::move(cooked).Value();
        SECTION("magic") {
            std::as_writable_bytes(std::span{artifact.payload})[0] ^= std::byte{1};
        }
        SECTION("schema") {
            artifact.payload[4] = 2;
        }
        SECTION("target") {
            std::as_writable_bytes(std::span{artifact.payload})[32] ^= std::byte{1};
        }
        SECTION("count") {
            artifact.payload[96] = 255;
        }
        SECTION("child identity") {
            std::fill_n(artifact.payload.begin() + 100, 8, 0);
        }
        SECTION("child material") {
            std::fill_n(artifact.payload.begin() + 108, 8, 0);
        }
        SECTION("child extent") {
            std::fill_n(artifact.payload.begin() + 204, 8, 255);
        }
        SECTION("inner payload integrity") {
            std::as_writable_bytes(std::span{artifact.payload}).back() ^= std::byte{1};
        }
        SECTION("truncated") {
            artifact.payload.pop_back();
        }
        SECTION("trailing") {
            artifact.payload.push_back(0);
        }
        Rebind(artifact);
        CHECK(LoadCookedPhysicsCompound(artifact.descriptor, fixture.target, artifact.payload).HasError());
    }

    TEST_CASE("Compound loader enforces exact keys", "[physics][compound]") {
        CompoundFixture fixture;
        const auto cooked = CookPhysicsCompound(fixture.request);
        REQUIRE(cooked.HasValue());
        auto descriptor = cooked.Value().descriptor;
        SECTION("cook key") {
            std::as_writable_bytes(std::span{descriptor.cacheKeyDigest->bytes})[0] ^= std::byte{1};
        }
        SECTION("payload key") {
            std::as_writable_bytes(std::span{descriptor.payloadDigest->bytes})[0] ^= std::byte{1};
        }
        SECTION("asset") {
            descriptor.asset = {};
        }
        SECTION("kind") {
            descriptor.kind = PhysicsCookedShapeKind::ConvexHull;
        }
        CHECK(LoadCookedPhysicsCompound(descriptor, fixture.target, cooked.Value().payload).HasError());
    }

    TEST_CASE("Compound loader enforces lowered runtime allocation limits", "[physics][compound]") {
        CompoundFixture fixture;
        const auto cooked = CookPhysicsCompound(fixture.request);
        REQUIRE(cooked.HasValue());
        PhysicsCompoundCookLimits limits;
        SECTION("child count") {
            limits.maximumChildren = 1;
        }
        SECTION("payload bytes") {
            limits.maximumPayloadBytes = cooked.Value().payload.size() - 1;
        }
        SECTION("convex vertex count") {
            limits.convex.maxHullVertices = 3;
        }
        CHECK(LoadCookedPhysicsCompound(cooked.Value().descriptor, fixture.target, cooked.Value().payload, limits).HasError());
    }

    TEST_CASE("Compound cache replacement and shutdown preserve old immutable leased children", "[physics][compound][shape_cache]") {
        CompoundFixture fixture;
        const auto first = CookPhysicsCompound(fixture.request);
        REQUIRE(first.HasValue());
        auto cache = PhysicsCookedShapeCache::Create(fixture.target).Value();
        auto old = cache.Acquire(first.Value().descriptor, first.Value().payload);
        REQUIRE(old.HasValue());
        fixture.request.dependencyDigest.bytes[0]++;
        const auto replacement = CookPhysicsCompound(fixture.request);
        REQUIRE(replacement.HasValue());
        auto next = cache.Acquire(replacement.Value().descriptor, replacement.Value().payload);
        REQUIRE(next.HasValue());
        CHECK_FALSE(old.Value().SharesResourceWith(next.Value()));
        auto shared = cache.Acquire(replacement.Value().descriptor, {});
        REQUIRE(shared.HasValue());
        CHECK(next.Value().SharesResourceWith(shared.Value()));
        REQUIRE(cache.Evict(first.Value().descriptor).Value());
        REQUIRE(cache.Shutdown().HasValue());
        REQUIRE(cache.Shutdown().HasValue());
        CHECK(cache.Acquire(replacement.Value().descriptor, replacement.Value().payload).HasError());
        REQUIRE(old.Value().Compound() != nullptr);
        CHECK(old.Value().Compound()->children[0].id.Value() == 27);
        CHECK(next.Value().Compound()->dependencyDigest == fixture.request.dependencyDigest);
        CHECK(cache.Stats().residentShapes == 0);
    }
}  // namespace Horo::Physics
