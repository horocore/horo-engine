#include "CanonicalPhysicsStateTopologyHash.h"

namespace Horo::Physics::Detail {
    namespace {
        void Motion(StateDetail::StateHash &hash, const JPH::MotionProperties &motion) noexcept {
            const auto inertia = motion.GetInverseInertiaDiagonal();
            hash.Float(inertia.GetX());
            hash.Float(inertia.GetY());
            hash.Float(inertia.GetZ());
            const auto rotation = motion.GetInertiaRotation();
            hash.Float(rotation.GetX());
            hash.Float(rotation.GetY());
            hash.Float(rotation.GetZ());
            hash.Float(rotation.GetW());
        }

        void Settings(StateDetail::StateHash &hash, const JPH::BodyCreationSettings &value) noexcept {
            hash.Word(static_cast<std::uint8_t>(value.mMotionType));
            hash.Word(static_cast<std::uint8_t>(value.mAllowedDOFs));
            hash.Word(value.mAllowDynamicOrKinematic);
            hash.Word(value.mIsSensor);
            hash.Word(value.mCollideKinematicVsNonDynamic);
            hash.Word(value.mUseManifoldReduction);
            hash.Word(value.mApplyGyroscopicForce);
            hash.Word(value.mEnhancedInternalEdgeRemoval);
            hash.Float(value.mFriction);
            hash.Float(value.mRestitution);
            hash.Float(value.mLinearDamping);
            hash.Float(value.mAngularDamping);
            hash.Float(value.mMaxLinearVelocity);
            hash.Float(value.mMaxAngularVelocity);
            hash.Float(value.mGravityFactor);
            hash.Word(value.mNumVelocityStepsOverride);
            hash.Word(value.mNumPositionStepsOverride);
            hash.Word(value.mObjectLayer);
        }
    }  // namespace

    /** @copydoc HashStateNativeBody */
    void HashStateNativeBody(StateDetail::StateHash &hash, const JPH::Body &body) noexcept {
        Settings(hash, body.GetBodyCreationSettings());
        const auto *motion = body.GetMotionPropertiesUnchecked();
        hash.Word(motion != nullptr);
        if (motion) {
            hash.Float(motion->GetInverseMassUnchecked());
            if (body.IsDynamic())
                Motion(hash, *motion);
        }
        // Pose, velocities, allow-sleep, active state, and effective CCD are restored state, not immutable compatibility.
        // Native collision-group pointers/user data are not durable authority; canonical Horo endpoint/profile policies
        // and joint collision policy are hashed by their respective typed owners.
    }
}  // namespace Horo::Physics::Detail
