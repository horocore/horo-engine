#include "CanonicalPhysicsStateBodyControl.h"

#include <Jolt/Core/FPException.h>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::Detail {
    namespace {
        PhysicsStateFpControl ActualControl() {
            auto actual = ReadCanonicalPhysicsStateFpControl();
            INFO((actual.HasError() ? actual.ErrorValue().message : "success"));
            REQUIRE(actual.HasValue());
            return actual.Value();
        }
    }  // namespace

    TEST_CASE("Actual pinned mass invalid-exception guard matches recorded compiled subprofile and restores caller controls",
              "[physics][native][state][fp][constructor]") {
        const auto original = ActualControl();
        const auto expected = CanonicalBodyMassSubprofile(original);
        REQUIRE(expected.has_value());
        PhysicsStateFpControl actual;
        {
            JPH::FPExceptionDisableInvalid guard;
            actual = ActualControl();
        }
        CHECK(actual == *expected);
        CHECK(ActualControl() == original);
        CHECK(CanonicalBodyConstructionKnown(original, original, expected));
    }

    TEST_CASE("Body arithmetic provenance is sticky and cannot be repaired by a later matching boundary",
              "[physics][native][state][fp][constructor]") {
        const auto original = ActualControl();
        auto altered = original;
        altered.value ^= std::uint64_t{1} << 13;
        std::optional<PhysicsStateFpControl> retained = original;
        FoldCanonicalBodyControl(retained, original, original);
        REQUIRE(retained.has_value());
        FoldCanonicalBodyControl(retained, altered, altered);
        CHECK_FALSE(retained.has_value());
        FoldCanonicalBodyControl(retained, original, original);
        CHECK_FALSE(retained.has_value());
        CHECK_FALSE(CanonicalBodyConstructionKnown(original, retained, CanonicalBodyMassSubprofile(original)));
        CHECK_FALSE(CompleteCanonicalBodyControl(original, altered).has_value());
        CHECK_FALSE(CompleteCanonicalBodyControl(std::nullopt, original).has_value());
    }

    TEST_CASE("Required body arithmetic admission rejects mismatch while Optional preserves ordinary construction policy",
              "[physics][native][state][fp][constructor]") {
        const auto original = ActualControl();
        constexpr PhysicsStateConstructionLimits limits{2, 0, 2, 2, 16384};
        auto required = StateDetail::ConstructionRecipePool::Prepare(limits, original, PhysicsStateConstructionTracking::Required);
        auto optional = StateDetail::ConstructionRecipePool::Prepare(limits, original, PhysicsStateConstructionTracking::Optional);
        REQUIRE(required.HasValue());
        REQUIRE(optional.HasValue());
        auto altered = original;
        altered.value ^= std::uint64_t{1} << 13;
        CHECK(AdmitCanonicalBodyControl(required.Value().get(), original).HasValue());
        CHECK(AdmitCanonicalBodyControl(required.Value().get(), altered).HasError());
        CHECK(AdmitCanonicalBodyControl(required.Value().get(), std::nullopt).HasError());
        CHECK(AdmitCanonicalBodyControl(optional.Value().get(), altered).HasValue());
        CHECK(AdmitCanonicalBodyControl(optional.Value().get(), std::nullopt).HasValue());
        CHECK(AdmitCanonicalBodyControl(nullptr, std::nullopt).HasValue());
    }
}  // namespace Horo::Physics::Detail
