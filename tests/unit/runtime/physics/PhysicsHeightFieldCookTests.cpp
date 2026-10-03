#include "Horo/Physics/PhysicsCookedShapeCache.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsHeightFieldCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <span>
#include <string>
#include <thread>

namespace Horo::Physics {
    namespace {
        [[nodiscard]] Assets::AssetId Asset() {
            return Assets::AssetId::Parse("10000000-0000-0000-0000-000000000850").Value();
        }

        [[nodiscard]] PhysicsShapeCookTargetDigest Target(const std::uint8_t marker = 1) {
            PhysicsShapeCookTargetDigest target;
            target.digest.bytes[0] = marker;
            return target;
        }

        struct Tile final {
            std::array<float, 9> samples{0, 1, 2, 3, 4, 5, 6, 7, 8};
            std::array<std::uint8_t, 4> holes{0, 1, 0, 0};
            std::array<PhysicsMaterialSlotId, 4> cellMaterials{PhysicsMaterialSlotId::FromValue(7),
                                                               {},
                                                               PhysicsMaterialSlotId::FromValue(9),
                                                               PhysicsMaterialSlotId::FromValue(7)};
            std::array<PhysicsMaterialSlotId, 2> slots{PhysicsMaterialSlotId::FromValue(7), PhysicsMaterialSlotId::FromValue(9)};

            [[nodiscard]] PhysicsHeightFieldCookRequest Request() const {
                return {.asset = Asset(),
                        .subresource = PhysicsShapeSubresourceId::FromValue(5),
                        .width = 3,
                        .height = 3,
                        .origin = {10, 20, 30},
                        .spacingX = 2,
                        .spacingZ = 3,
                        .sampleScaleY = 0.5F,
                        .samples = samples,
                        .cellHoles = holes,
                        .cellMaterials = cellMaterials,
                        .materialSlots = slots,
                        .target = Target(),
                        .sourceContext = "terrain/tile-5"};
            }
        };

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == code.code.Value());
        }

        void RefreshDigest(PhysicsHeightFieldCookResult &cooked) {
            cooked.descriptor.payloadDigest = ComputeSha256(std::as_bytes(std::span{cooked.payload}));
        }
    }  // namespace

    TEST_CASE("Heightfield tile cook is deterministic, source-free and preserves holes/materials", "[unit][physics][heightfield]") {
        const Tile tile;
        const auto first = CookPhysicsHeightField(tile.Request());
        const auto second = CookPhysicsHeightField(tile.Request());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().payload == second.Value().payload);
        REQUIRE(first.Value().descriptor.cacheKeyDigest == second.Value().descriptor.cacheKeyDigest);
        REQUIRE(first.Value().descriptor.kind == PhysicsCookedShapeKind::HeightField);
        REQUIRE(first.Value().bounds.minimum == (Math::Vec3{10, 20, 30}));
        REQUIRE(first.Value().bounds.maximum == (Math::Vec3{14, 24, 36}));
        const auto loaded = LoadCookedPhysicsHeightField(first.Value().descriptor, Target(), first.Value().payload);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().samples == std::vector<float>(tile.samples.begin(), tile.samples.end()));
        REQUIRE(loaded.Value().cellHoles == std::vector<std::uint8_t>(tile.holes.begin(), tile.holes.end()));
        REQUIRE(loaded.Value().cellMaterials[1].IsValid() == false);
        REQUIRE(loaded.Value().cellMaterials[2] == PhysicsMaterialSlotId::FromValue(9));
    }

    TEST_CASE("Heightfield tile exact identity binds source, settings, target and persistent tile id",
              "[unit][physics][heightfield][identity]") {
        Tile tile;
        const auto baseline = CookPhysicsHeightField(tile.Request()).Value();
        tile.samples[0] = 0.25F;
        REQUIRE(CookPhysicsHeightField(tile.Request()).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);
        tile.samples[0] = 0;
        tile.holes[1] = 0;
        tile.cellMaterials[1] = PhysicsMaterialSlotId::FromValue(9);
        REQUIRE(CookPhysicsHeightField(tile.Request()).Value().sourceDigest != baseline.sourceDigest);
        tile = Tile{};
        auto request = tile.Request();
        request.target = Target(2);
        REQUIRE(CookPhysicsHeightField(request).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);
        request = tile.Request();
        request.subresource = PhysicsShapeSubresourceId::FromValue(6);
        REQUIRE(CookPhysicsHeightField(request).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);
        request = tile.Request();
        request.settings.limits.maxSamples = 1'048'575;
        REQUIRE(CookPhysicsHeightField(request).Value().descriptor.cacheKeyDigest != baseline.descriptor.cacheKeyDigest);
    }

    TEST_CASE("Heightfield rejects invalid grid, scales, holes and material mappings without publication",
              "[unit][physics][heightfield][validation]") {
        Tile tile;
        auto request = tile.Request();
        request.width = 1;
        RequireError(CookPhysicsHeightField(request), PhysicsErrors::ShapeCookLimitExceeded);
        request = tile.Request();
        request.spacingX = 0;
        RequireError(CookPhysicsHeightField(request), PhysicsErrors::ShapeCookSourceInvalid);
        request = tile.Request();
        request.sampleScaleY = std::numeric_limits<float>::infinity();
        RequireError(CookPhysicsHeightField(request), PhysicsErrors::ShapeCookSourceInvalid);
        tile.samples[2] = std::numeric_limits<float>::quiet_NaN();
        RequireError(CookPhysicsHeightField(tile.Request()), PhysicsErrors::ShapeCookSourceInvalid);
        tile = Tile{};
        tile.holes[0] = 2;
        RequireError(CookPhysicsHeightField(tile.Request()), PhysicsErrors::ShapeCookSourceInvalid);
        tile = Tile{};
        tile.cellMaterials[1] = PhysicsMaterialSlotId::FromValue(7);
        RequireError(CookPhysicsHeightField(tile.Request()), PhysicsErrors::ShapeCookSourceInvalid);
        tile = Tile{};
        tile.cellMaterials[0] = PhysicsMaterialSlotId::FromValue(10);
        RequireError(CookPhysicsHeightField(tile.Request()), PhysicsErrors::ShapeCookSourceInvalid);
        tile = Tile{};
        tile.slots[1] = tile.slots[0];
        RequireError(CookPhysicsHeightField(tile.Request()), PhysicsErrors::ShapeCookSourceInvalid);
        tile = Tile{};
        request = tile.Request();
        request.settings.limits.maxPayloadBytes = 192;
        RequireError(CookPhysicsHeightField(request), PhysicsErrors::ShapeCookLimitExceeded);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(CookPhysicsHeightField(tile.Request(), cancellation.Token()), PhysicsErrors::ShapeCookCancelled);
    }

    TEST_CASE("Heightfield loader rejects tampering, mismatched target and hostile table extents",
              "[unit][physics][heightfield][artifact]") {
        const Tile tile;
        auto cooked = CookPhysicsHeightField(tile.Request()).Value();
        auto corrupt = cooked.payload;
        corrupt.back() ^= 1U;
        RequireError(LoadCookedPhysicsHeightField(cooked.descriptor, Target(), corrupt), PhysicsErrors::ShapeArtifactInvalid);
        RequireError(LoadCookedPhysicsHeightField(cooked.descriptor, Target(2), cooked.payload), PhysicsErrors::ProfileUnsupported);
        cooked.payload.resize(20);
        RefreshDigest(cooked);
        RequireError(LoadCookedPhysicsHeightField(cooked.descriptor, Target(), cooked.payload), PhysicsErrors::ShapeArtifactInvalid);
        cooked = CookPhysicsHeightField(tile.Request()).Value();
        // The row-major width is the first source word after the fixed envelope.
        cooked.payload[192] = 0xff;
        RefreshDigest(cooked);
        RequireError(LoadCookedPhysicsHeightField(cooked.descriptor, Target(), cooked.payload), PhysicsErrors::ShapeArtifactInvalid);

        cooked = CookPhysicsHeightField(tile.Request()).Value();
        // The payload-length word is in the envelope, outside the source digest.
        cooked.payload[184] ^= 1U;
        RefreshDigest(cooked);
        const auto malformedLength = LoadCookedPhysicsHeightField(cooked.descriptor, Target(), cooked.payload);
        RequireError(malformedLength, PhysicsErrors::ShapeArtifactInvalid);
        REQUIRE(malformedLength.ErrorValue().message.find("envelope") != std::string::npos);

        cooked = CookPhysicsHeightField(tile.Request()).Value();
        // A forged but finite envelope bound must be checked against decoded samples.
        cooked.payload[164] ^= 1U;
        RefreshDigest(cooked);
        const auto forgedBounds = LoadCookedPhysicsHeightField(cooked.descriptor, Target(), cooked.payload);
        RequireError(forgedBounds, PhysicsErrors::ShapeArtifactInvalid);
        REQUIRE(forgedBounds.ErrorValue().message.find("bounds do not match") != std::string::npos);
    }

    TEST_CASE("Heightfield cache pins exact tile generations through eviction and shutdown",
              "[unit][physics][heightfield][lifetime][thread]") {
        Tile tile;
        const auto first = CookPhysicsHeightField(tile.Request()).Value();
        tile.samples[0] = 1.0F;
        const auto next = CookPhysicsHeightField(tile.Request()).Value();
        auto cache = PhysicsCookedShapeCache::Create(Target()).Value();
        auto oldLease = cache.Acquire(first.descriptor, first.payload).Value();
        REQUIRE(oldLease.HeightField() != nullptr);
        REQUIRE(oldLease.TriangleMesh() == nullptr);
        REQUIRE(cache.Evict(first.descriptor).Value());
        auto newLease = cache.Acquire(next.descriptor, next.payload).Value();
        REQUIRE_FALSE(oldLease.SharesResourceWith(newLease));
        REQUIRE(oldLease.HeightField()->samples[0] == 0.0F);
        REQUIRE(newLease.HeightField()->samples[0] == 1.0F);
        std::thread releaser([lease = std::move(oldLease)]() mutable {
            lease = {};
        });
        releaser.join();
        REQUIRE(cache.Shutdown().HasValue());
        REQUIRE(newLease.HeightField()->samples[0] == 1.0F);
        RequireError(cache.Acquire(next.descriptor, next.payload), PhysicsErrors::InvalidState);
    }

    TEST_CASE("Heightfield body compatibility is static-only", "[unit][physics][heightfield][motion]") {
        REQUIRE(ValidatePhysicsHeightFieldMotion(PhysicsMotionType::Static).HasValue());
        RequireError(ValidatePhysicsHeightFieldMotion(PhysicsMotionType::Kinematic), PhysicsErrors::ShapeMotionUnsupported);
        RequireError(ValidatePhysicsHeightFieldMotion(PhysicsMotionType::Dynamic), PhysicsErrors::ShapeMotionUnsupported);
    }
}  // namespace Horo::Physics
