#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <thread>

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
