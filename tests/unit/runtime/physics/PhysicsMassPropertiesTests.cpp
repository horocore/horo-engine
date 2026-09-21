#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMassProperties.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

namespace Horo::Physics {
    namespace {
        using Catch::Approx;

        [[nodiscard]] PhysicsMassPropertiesShape Box(const Math::Vec3 halfExtents, const Math::Vec3 translation = {}) {
            return {.geometry = PhysicsBoxShape{halfExtents}, .localPose = {translation, Math::Quaternion::Identity()}};
        }

        void RequireCode(const Result<PhysicsMassProperties> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Physics mass properties derive SI box mass and diagonal inertia", "[physics][mass-properties]") {
        const std::array shapes{Box({1.0F, 2.0F, 3.0F})};
        const auto result = ResolvePhysicsMassProperties({PhysicsDensity{2.0F}, shapes});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().massKilograms == Approx(96.0F));
        REQUIRE(result.Value().centerOfMassMeters == Math::Vec3{});
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == Approx(416.0F));
        REQUIRE(result.Value().inertia.yyKilogramSquareMeters == Approx(320.0F));
        REQUIRE(result.Value().inertia.zzKilogramSquareMeters == Approx(160.0F));
        REQUIRE(result.Value().inertia.xyKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.xzKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.yzKilogramSquareMeters == Approx(0.0F));
    }

    TEST_CASE("Physics mass properties derive SI sphere mass and isotropic inertia", "[physics][mass-properties]") {
        const std::array shapes{PhysicsMassPropertiesShape{PhysicsSphereShape{1.0F}, {}}};
        const auto result = ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, shapes});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().massKilograms == Approx(4.0F * Math::Pi / 3.0F));
        REQUIRE(result.Value().centerOfMassMeters == Math::Vec3{});
        const auto expectedInertia = Approx(8.0F * Math::Pi / 15.0F);
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == expectedInertia);
        REQUIRE(result.Value().inertia.yyKilogramSquareMeters == expectedInertia);
        REQUIRE(result.Value().inertia.zzKilogramSquareMeters == expectedInertia);
        REQUIRE(result.Value().inertia.xyKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.xzKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.yzKilogramSquareMeters == Approx(0.0F));
    }

    TEST_CASE("Physics mass properties preserve explicit total mass while deriving a uniform compound density",
              "[physics][mass-properties]") {
        const std::array shapes{Box({1.0F, 1.0F, 1.0F})};
        const auto result = ResolvePhysicsMassProperties({PhysicsMass{12.0F}, shapes});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().massKilograms == Approx(12.0F));
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == Approx(8.0F));
        REQUIRE(result.Value().inertia.yyKilogramSquareMeters == Approx(8.0F));
        REQUIRE(result.Value().inertia.zzKilogramSquareMeters == Approx(8.0F));
    }

    TEST_CASE("Physics mass properties support translated compounds and the parallel-axis theorem", "[physics][mass-properties]") {
        const std::array shapes{Box({0.5F, 0.5F, 0.5F}, {-2.0F, 0.0F, 0.0F}), Box({0.5F, 0.5F, 0.5F}, {2.0F, 0.0F, 0.0F})};
        const auto result = ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, shapes});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().massKilograms == Approx(2.0F));
        REQUIRE(result.Value().centerOfMassMeters == Math::Vec3{});
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == Approx(0.3333333F));
        REQUIRE(result.Value().inertia.yyKilogramSquareMeters == Approx(8.333333F));
        REQUIRE(result.Value().inertia.zzKilogramSquareMeters == Approx(8.333333F));
    }

    TEST_CASE("Physics mass properties rotate contributor inertia into the compound frame", "[physics][mass-properties]") {
        const auto rotation = Math::Quaternion::FromAxisAngle({0.0F, 0.0F, 1.0F}, Math::Pi / 4.0F);
        const std::array shapes{PhysicsMassPropertiesShape{PhysicsBoxShape{{1.0F, 2.0F, 3.0F}}, {{}, rotation}}};
        const auto result = ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, shapes});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == Approx(184.0F));
        REQUIRE(result.Value().inertia.yyKilogramSquareMeters == Approx(184.0F));
        REQUIRE(result.Value().inertia.zzKilogramSquareMeters == Approx(80.0F));
        REQUIRE(result.Value().inertia.xyKilogramSquareMeters == Approx(24.0F));
    }

    TEST_CASE("Physics mass properties validate capsule volume and axial symmetry", "[physics][mass-properties]") {
        const std::array shapes{PhysicsMassPropertiesShape{PhysicsCapsuleShape{1.0F, 1.0F}, {}}};
        const auto result = ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, shapes});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().massKilograms == Approx(10.0F * Math::Pi / 3.0F));
        REQUIRE(result.Value().inertia.xyKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.xzKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.yzKilogramSquareMeters == Approx(0.0F));
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == Approx(result.Value().inertia.zzKilogramSquareMeters));
        REQUIRE(result.Value().inertia.xxKilogramSquareMeters == Approx(121.0F * Math::Pi / 30.0F));
    }

    TEST_CASE("Physics mass property overrides are complete and geometry-independent", "[physics][mass-properties]") {
        const PhysicsMassProperties expected{5.0F, {1.0F, -2.0F, 3.0F}, {4.0F, 5.0F, 6.0F, 0.5F, -0.25F, 0.75F}};
        const auto result = ResolvePhysicsMassProperties({PhysicsMassPropertiesOverride{expected}, {}});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == expected);
    }

    TEST_CASE("Physics mass properties reject invalid inertia without repairing authored values", "[physics][mass-properties]") {
        const PhysicsMassProperties invalid{1.0F, {}, {1.0F, 1.0F, 1.0F, 2.0F, 0.0F, 0.0F}};
        RequireCode(ResolvePhysicsMassProperties({PhysicsMassPropertiesOverride{invalid}, {}}), PhysicsErrors::DescriptorInvalid);
        REQUIRE(invalid.inertia.xyKilogramSquareMeters == 2.0F);

        const PhysicsInertiaTensor nonFinite{1.0F, 1.0F, std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 0.0F};
        REQUIRE(ValidatePhysicsInertiaTensor(nonFinite).HasError());
    }

    TEST_CASE("Physics mass properties reject empty, static-plane, malformed, and oversized compounds", "[physics][mass-properties]") {
        RequireCode(ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, {}}), PhysicsErrors::DescriptorInvalid);
        const std::array plane{PhysicsMassPropertiesShape{PhysicsStaticPlaneShape{}, {}}};
        RequireCode(ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, plane}), PhysicsErrors::OperationUnsupported);

        auto malformed = Box({1.0F, 1.0F, 1.0F});
        malformed.localPose.rotation.w = 2.0F;
        const std::array malformedShapes{malformed};
        RequireCode(ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, malformedShapes}), PhysicsErrors::DescriptorInvalid);

        std::vector<PhysicsMassPropertiesShape> oversized(MaximumPhysicsMassPropertyShapes + 1, Box({1.0F, 1.0F, 1.0F}));
        RequireCode(ResolvePhysicsMassProperties({PhysicsDensity{1.0F}, oversized}), PhysicsErrors::DescriptorInvalid);
    }

    TEST_CASE("Physics mass properties reject derived values outside finite policy bounds", "[physics][mass-properties]") {
        const std::array shapes{Box({1.0F, 1.0F, 1.0F})};
        RequireCode(ResolvePhysicsMassProperties({PhysicsDensity{std::numeric_limits<float>::infinity()}, shapes}),
                    PhysicsErrors::DescriptorInvalid);
        RequireCode(ResolvePhysicsMassProperties({PhysicsMass{0.0F}, shapes}), PhysicsErrors::DescriptorInvalid);
        RequireCode(ResolvePhysicsMassProperties({PhysicsMass{MinimumPhysicsMassKilograms}, shapes}), PhysicsErrors::DescriptorInvalid);
        const std::array tiny{Box({0.001F, 0.001F, 0.001F})};
        RequireCode(ResolvePhysicsMassProperties({PhysicsMass{1.0F}, tiny}), PhysicsErrors::DescriptorInvalid);
        const std::array huge{Box({std::numeric_limits<float>::max(), 1.0F, 1.0F})};
        RequireCode(ResolvePhysicsMassProperties({PhysicsDensity{MaximumPhysicsDensity}, huge}), PhysicsErrors::DescriptorInvalid);
    }
}  // namespace Horo::Physics
