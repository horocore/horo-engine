#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace Horo::Physics {
    namespace {
        PhysicsMaterialAssetId AssetId(const std::uint8_t marker = 1) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return PhysicsMaterialAssetId::FromAssetId(Assets::AssetId::FromBytes(bytes));
        }

        PhysicsMaterialAssetDescriptor Descriptor() {
            return {.schemaVersion = PhysicsMaterialAssetSchemaVersion,
                    .id = AssetId(),
                    .sourceRevision = 7,
                    .friction = 0.8F,
                    .restitution = 0.25F,
                    .densityKilogramsPerCubicMeter = 1'000.0F,
                    .frictionCombine = PhysicsMaterialCombineMode::Average,
                    .restitutionCombine = PhysicsMaterialCombineMode::Maximum};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
            CHECK_FALSE(result.ErrorValue().message.empty());
            CHECK_FALSE(expected.remediationHint.empty());
        }
    }  // namespace

    TEST_CASE("Physics material assets preserve stable identity and own physical semantics", "[physics][material]") {
        auto descriptor = Descriptor();
        const auto asset = PhysicsMaterialAsset::Create(descriptor);
        REQUIRE(asset.HasValue());
        CHECK(asset.Value().Id() == descriptor.id);
        CHECK(asset.Value().SourceRevision() == descriptor.sourceRevision);
        CHECK(asset.Value().Friction() == descriptor.friction);
        CHECK(asset.Value().Restitution() == descriptor.restitution);
        CHECK(asset.Value().DensityKilogramsPerCubicMeter() == descriptor.densityKilogramsPerCubicMeter);
        CHECK(asset.Value().FrictionCombine() == descriptor.frictionCombine);
        CHECK(asset.Value().RestitutionCombine() == descriptor.restitutionCombine);

        descriptor.friction = 0.1F;
        descriptor.sourceRevision = 8;
        CHECK(asset.Value().Friction() == 0.8F);
        CHECK(asset.Value().SourceRevision() == 7);
        CHECK(asset.Value().Id() == AssetId());
        CHECK(AssetId() == PhysicsMaterialAssetId::FromAssetId(asset.Value().Id().Asset()));
    }

    TEST_CASE("Physics material assets admit every explicit combine policy", "[physics][material]") {
        using enum PhysicsMaterialCombineMode;
        for (const auto mode : {Average, Minimum, Multiply, Maximum}) {
            auto descriptor = Descriptor();
            descriptor.frictionCombine = mode;
            descriptor.restitutionCombine = mode;
            REQUIRE(PhysicsMaterialAsset::Create(descriptor).HasValue());
        }

        auto descriptor = Descriptor();
        descriptor.frictionCombine = static_cast<PhysicsMaterialCombineMode>(255);
        RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialCombineUnsupported);
    }

    TEST_CASE("Physics material assets combine contact coefficients by explicit policy precedence", "[physics][material][combine]") {
        using enum PhysicsMaterialCombineMode;
        auto leftDescriptor = Descriptor();
        leftDescriptor.friction = 0.2F;
        leftDescriptor.restitution = 0.3F;
        leftDescriptor.frictionCombine = Minimum;
        leftDescriptor.restitutionCombine = Multiply;
        auto rightDescriptor = Descriptor();
        rightDescriptor.id = AssetId(2);
        rightDescriptor.friction = 0.8F;
        rightDescriptor.restitution = 0.7F;
        rightDescriptor.frictionCombine = Maximum;
        rightDescriptor.restitutionCombine = Average;

        const auto left = PhysicsMaterialAsset::Create(leftDescriptor).Value();
        const auto right = PhysicsMaterialAsset::Create(rightDescriptor).Value();
        const auto combined = CombinePhysicsMaterialAssets(left, right);
        REQUIRE(combined.HasValue());
        CHECK(combined.Value().friction == 0.8F);
        CHECK(combined.Value().restitution == Catch::Approx(0.21F));

        RequireError(CombinePhysicsMaterialAssets({}, right), PhysicsErrors::MaterialDescriptorInvalid);
    }

    TEST_CASE("Physics material assets reject identity, schema, bounds, and non-finite input", "[physics][material][validation]") {
        REQUIRE(PhysicsMaterialAssetId::Parse("00000000-0000-0000-0000-000000000001").HasValue());
        RequireError(PhysicsMaterialAssetId::Parse("00000000-0000-0000-0000-000000000000"), PhysicsErrors::MaterialDescriptorInvalid);

        auto descriptor = Descriptor();
        descriptor.id = {};
        RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialDescriptorInvalid);

        descriptor = Descriptor();
        descriptor.schemaVersion = PhysicsMaterialAssetSchemaVersion + 1;
        RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialDescriptorInvalid);

        descriptor = Descriptor();
        descriptor.sourceRevision = 0;
        RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialDescriptorInvalid);

        for (const float friction : {std::nextafter(MinimumPhysicsMaterialFriction, -std::numeric_limits<float>::infinity()),
                                     std::nextafter(MaximumPhysicsMaterialFriction, std::numeric_limits<float>::infinity()),
                                     std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            descriptor = Descriptor();
            descriptor.friction = friction;
            RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialDescriptorInvalid);
        }

        for (const float restitution : {-0.01F, 1.01F, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity()}) {
            descriptor = Descriptor();
            descriptor.restitution = restitution;
            RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialDescriptorInvalid);
        }

        for (const float density :
             {std::nextafter(MinimumPhysicsDensity, 0.0F), std::nextafter(MaximumPhysicsDensity, std::numeric_limits<float>::infinity()),
              std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            descriptor = Descriptor();
            descriptor.densityKilogramsPerCubicMeter = density;
            RequireError(PhysicsMaterialAsset::Create(descriptor), PhysicsErrors::MaterialDescriptorInvalid);
        }

        descriptor = Descriptor();
        descriptor.friction = MinimumPhysicsMaterialFriction;
        descriptor.restitution = MaximumPhysicsMaterialRestitution;
        descriptor.densityKilogramsPerCubicMeter = MaximumPhysicsDensity;
        REQUIRE(PhysicsMaterialAsset::Create(descriptor).HasValue());
    }

    TEST_CASE("Physics material assets are independent value snapshots across worker lifetime", "[physics][material][thread]") {
        const auto descriptor = Descriptor();
        const auto asset = PhysicsMaterialAsset::Create(descriptor).Value();
        Result<PhysicsMaterialAsset> workerResult =
            Result<PhysicsMaterialAsset>::Failure(MakeError(PhysicsErrors::MaterialDescriptorInvalid, "worker result not assigned"));

        std::thread worker([asset, &workerResult] {  // NOSONAR(cpp:S6168) Test target toolchain has no std::jthread.
            workerResult = PhysicsMaterialAsset::Create(asset.Descriptor());
        });
        worker.join();

        REQUIRE(workerResult.HasValue());
        CHECK(workerResult.Value().Descriptor() == asset.Descriptor());
        CHECK(asset.Id() == descriptor.id);
    }
}  // namespace Horo::Physics

namespace {
    /** @brief Full nondefault immutable material used by valid and malformed wire regressions. */
    Horo::Physics::PhysicsMaterialAsset CodecMaterial() {
        using namespace Horo;
        using namespace Horo::Physics;
        std::array<std::uint8_t, 16> identity{};
        identity.back() = 87;
        const auto material = PhysicsMaterialAsset::Create({.id = PhysicsMaterialAssetId::FromAssetId(Assets::AssetId::FromBytes(identity)),
                                                            .sourceRevision = 7,
                                                            .friction = 0.75F,
                                                            .restitution = 0.25F,
                                                            .densityKilogramsPerCubicMeter = 725.0F,
                                                            .frictionCombine = PhysicsMaterialCombineMode::Maximum,
                                                            .restitutionCombine = PhysicsMaterialCombineMode::Multiply});
        REQUIRE(material.HasValue());
        return material.Value();
    }
}  // namespace

TEST_CASE("Canonical material payload round trips every immutable descriptor field", "[physics][material][codec]") {
    using namespace Horo;
    using namespace Horo::Physics;
    const auto material = CodecMaterial();
    const auto bytes = EncodePhysicsMaterialAsset(material);
    REQUIRE(bytes.HasValue());
    REQUIRE(bytes.Value().size() == PhysicsMaterialPayloadSize);
    const auto decoded = DecodePhysicsMaterialAsset(bytes.Value());
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().Descriptor() == material.Descriptor());
    REQUIRE(EncodePhysicsMaterialAsset(decoded.Value()).Value() == bytes.Value());
}

TEST_CASE("Canonical material payload rejects malformed and noncanonical bytes", "[physics][material][codec]") {
    using namespace Horo;
    using namespace Horo::Physics;
    const auto bytes = EncodePhysicsMaterialAsset(CodecMaterial());
    REQUIRE(bytes.HasValue());
    auto invalid = bytes.Value();
    SECTION("unknown format") {
        invalid[8] = std::byte{2};
    }
    SECTION("unknown descriptor schema") {
        invalid[12] = std::byte{2};
    }
    SECTION("zero identity") {
        for (std::size_t i = 16; i < 32; ++i)
            invalid[i] = std::byte{0};
    }
    SECTION("non-finite coefficient") {
        invalid[42] = std::byte{0x80};
        invalid[43] = std::byte{0x7f};
    }
    SECTION("NaN coefficient") {
        invalid[40] = std::byte{1};
        invalid[42] = std::byte{0x80};
        invalid[43] = std::byte{0x7f};
    }
    SECTION("noncanonical negative zero") {
        invalid[40] = std::byte{0};
        invalid[41] = std::byte{0};
        invalid[42] = std::byte{0};
        invalid[43] = std::byte{0x80};
    }
    SECTION("unknown combine mode") {
        invalid[52] = std::byte{255};
    }
    SECTION("reserved bytes") {
        invalid[55] = std::byte{1};
    }
    REQUIRE(DecodePhysicsMaterialAsset(invalid).HasError());
    REQUIRE(DecodePhysicsMaterialAsset(std::span{bytes.Value()}.first(PhysicsMaterialPayloadSize - 1)).HasError());
    std::vector duplicated(bytes.Value().begin(), bytes.Value().end());
    duplicated.insert(duplicated.end(), bytes.Value().begin(), bytes.Value().end());
    REQUIRE(DecodePhysicsMaterialAsset(duplicated).HasError());
}
