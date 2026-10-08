#include "CanonicalWorldSettings.h"

#include "CanonicalSolver.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        static_assert(JPH::cDefaultCollisionTolerance == 1.0e-4F);

        /** @brief Maps CanonicalV1 numeric and serial-work policy rather than inheriting unreviewed upstream overrides. */
        JPH::PhysicsSettings SolverSettings(const PhysicsWorldSettingsDescriptor &values) {
            JPH::PhysicsSettings native;
            native.mMaxInFlightBodyPairs = static_cast<int>(values.budgets.maximumInFlightPairs);
            native.mStepListenersBatchSize = 8;
            native.mStepListenerBatchesPerJob = std::numeric_limits<int>::max();
            native.mNumVelocitySteps = values.step.velocityIterations;
            native.mNumPositionSteps = values.step.positionIterations;
            native.mAllowSleeping = values.step.sleepingEnabled;
            native.mPointVelocitySleepThreshold = values.step.sleepPointSpeedMetersPerSecond;
            native.mTimeBeforeSleep = values.step.sleepDelaySeconds;
            native.mManifoldTolerance = 1.0e-3F;
            native.mSpeculativeContactDistance = 0.02F;
            native.mPenetrationSlop = 0.02F;
            native.mMaxPenetrationDistance = 0.2F;
            native.mContactPointPreserveLambdaMaxDistSq = 0.01F * 0.01F;
            native.mLinearCastThreshold = values.step.linearCastThresholdFraction;
            native.mLinearCastMaxPenetration = values.step.linearCastPenetrationFraction;
            native.mUseLargeIslandSplitter = false;
            native.mDeterministicSimulation = true;
            return native;
        }
    }  // namespace

    /** @copydoc TranslateDefaultMotionQuality */
    Result<JPH::EMotionQuality> TranslateDefaultMotionQuality(const PhysicsDefaultMotionQuality quality) {
        switch (quality) {
            case PhysicsDefaultMotionQuality::Discrete:
                return Result<JPH::EMotionQuality>::Success(JPH::EMotionQuality::Discrete);
            case PhysicsDefaultMotionQuality::LinearCast:
                return Result<JPH::EMotionQuality>::Success(JPH::EMotionQuality::LinearCast);
        }
        return Result<JPH::EMotionQuality>::Failure(
            MakeError(PhysicsErrors::OperationUnsupported, "Unknown default motion-quality policy."));
    }

    /** @copydoc TranslateCanonicalWorldSettings */
    Result<CanonicalWorldSettings> TranslateCanonicalWorldSettings(const PhysicsWorldSettings &settings) {
        if (!IsCanonicalSolverBuildCompatible())
            return Result<CanonicalWorldSettings>::Failure(
                MakeError(PhysicsErrors::ProfileUnsupported, "Canonical solver ABI is incompatible."));
        const auto &v = settings.Values();
        const auto motionQuality = TranslateDefaultMotionQuality(v.step.defaultMotionQuality);
        if (motionQuality.HasError())
            return Result<CanonicalWorldSettings>::Failure(motionQuality.ErrorValue());
        return Result<CanonicalWorldSettings>::Success({settings,
                                                        SolverSettings(v),
                                                        {v.world.gravity.x, v.world.gravity.y, v.world.gravity.z},
                                                        motionQuality.Value(),
                                                        static_cast<float>(v.world.fixedDeltaSeconds),
                                                        static_cast<int>(v.step.substepsPerTick),
                                                        v.world.capacity.maximumBodies,
                                                        v.budgets.maximumContactPairs,
                                                        v.budgets.maximumContactConstraints,
                                                        v.budgets.scratchBytes});
    }
}  // namespace Horo::Physics::Detail
