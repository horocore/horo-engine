#include "canonical/solver/CanonicalWorldSettings.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics {
    TEST_CASE("Native motion-quality translation maps known representations and rejects unknown values", "[physics][settings][native]") {
        const auto discrete = Detail::TranslateDefaultMotionQuality(PhysicsDefaultMotionQuality::Discrete);
        REQUIRE(discrete.HasValue());
        REQUIRE(discrete.Value() == JPH::EMotionQuality::Discrete);
        const auto linear = Detail::TranslateDefaultMotionQuality(PhysicsDefaultMotionQuality::LinearCast);
        REQUIRE(linear.HasValue());
        REQUIRE(linear.Value() == JPH::EMotionQuality::LinearCast);
        const auto unknown = Detail::TranslateDefaultMotionQuality(static_cast<PhysicsDefaultMotionQuality>(255));
        REQUIRE(unknown.HasError());
        REQUIRE(unknown.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
    }

    TEST_CASE("Canonical settings translation preserves captured native values and owner policy", "[physics][settings][native]") {
        PhysicsWorldSettingsDescriptor descriptor;
        descriptor.world.gravity = {1, -5, 2};
        descriptor.world.capacity.maximumBodies = 123;
        descriptor.budgets.maximumContactPairs = 20'000;
        descriptor.budgets.maximumContactConstraints = 5'000;
        descriptor.budgets.maximumInFlightPairs = 100;
        descriptor.budgets.scratchBytes = 32 * 1024 * 1024;
        descriptor.nonFinitePolicy = PhysicsNonFinitePolicy::QuarantineBody;
        const auto captured = PhysicsWorldSettings::Capture(descriptor);
        REQUIRE(captured.HasValue());
        const auto translated = Detail::TranslateCanonicalWorldSettings(captured.Value());
        REQUIRE(translated.HasValue());
        const auto &native = translated.Value();
        REQUIRE(native.source.Identity() == captured.Value().Identity());
        REQUIRE(native.source.Values().nonFinitePolicy == PhysicsNonFinitePolicy::QuarantineBody);
        REQUIRE(native.gravity == JPH::Vec3{1, -5, 2});
        REQUIRE(native.defaultMotionQuality == JPH::EMotionQuality::Discrete);
        REQUIRE(native.fixedDeltaSeconds == static_cast<float>(1.0 / 60.0));
        REQUIRE(native.collisionSteps == 1);
        REQUIRE(native.maximumBodies == 123);
        REQUIRE(native.maximumBodyPairs == 20'000);
        REQUIRE(native.maximumContactConstraints == 5'000);
        REQUIRE(native.scratchBytes == 32 * 1024 * 1024);
        REQUIRE(native.solver.mMaxInFlightBodyPairs == 100);
        descriptor.world.gravity.y = -10;
        REQUIRE(native.source.Values().world.gravity.y == -5);
    }

    TEST_CASE("Canonical solver settings map pinned tolerances sleep and serial work policy", "[physics][settings][native]") {
        const auto captured = PhysicsWorldSettings::Capture({});
        REQUIRE(captured.HasValue());
        const auto translated = Detail::TranslateCanonicalWorldSettings(captured.Value());
        REQUIRE(translated.HasValue());
        const auto &s = translated.Value().solver;
        REQUIRE(s.mNumVelocitySteps == 10);
        REQUIRE(s.mNumPositionSteps == 2);
        REQUIRE(s.mAllowSleeping);
        REQUIRE(s.mPointVelocitySleepThreshold == 0.03F);
        REQUIRE(s.mTimeBeforeSleep == 0.5F);
        REQUIRE(s.mManifoldTolerance == 1.0e-3F);
        REQUIRE(s.mSpeculativeContactDistance == 0.02F);
        REQUIRE(s.mPenetrationSlop == 0.02F);
        REQUIRE(s.mMaxPenetrationDistance == 0.2F);
        REQUIRE(s.mContactPointPreserveLambdaMaxDistSq == 0.01F * 0.01F);
        REQUIRE(s.mLinearCastThreshold == 0.75F);
        REQUIRE(s.mLinearCastMaxPenetration == 0.25F);
        REQUIRE(s.mStepListenersBatchSize == 8);
        REQUIRE(s.mStepListenerBatchesPerJob == std::numeric_limits<int>::max());
        REQUIRE_FALSE(s.mUseLargeIslandSplitter);
        REQUIRE(s.mDeterministicSimulation);
    }
}  // namespace Horo::Physics
