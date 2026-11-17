#include "PhysicsConstraintParametersInternal.h"
#include "PhysicsStateConstructionManifestInternal.h"

#include <bit>

namespace Horo::Physics::StateDetail {
    namespace {
        bool ComValid(const PhysicsStateConstructionCom &v) noexcept {
            return Finite(v.position) && UnitRotation(v.rotation);
        }

        Result<void> Frames(const PhysicsStateConstructionJoint &j) {
            for (const auto *frame : {&j.firstLocalFrame, &j.secondLocalOrWorldFrame, &j.firstConstructorFrame, &j.secondConstructorFrame})
                if (auto pose = ValidatePhysicsPose(*frame); pose.HasError())
                    return pose;
            return Result<void>::Success();
        }

        template <std::size_t N> bool Exact(const std::array<float, N> &a, const std::array<float, N> &b) noexcept {
            for (std::size_t i = 0; i < N; ++i)
                if (std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[i]))
                    return false;
            return true;
        }

        bool ExactPose(const PhysicsPose &a, const PhysicsPose &b) noexcept {
            return Exact(std::array{a.translation.x, a.translation.y, a.translation.z, a.rotation.x, a.rotation.y, a.rotation.z,
                                    a.rotation.w},
                         std::array{b.translation.x, b.translation.y, b.translation.z, b.rotation.x, b.rotation.y, b.rotation.z,
                                    b.rotation.w});
        }

        bool FixedEndpoint(const PhysicsStateConstructionJoint &joint) noexcept {
            // Pinned Body::sFixedToWorld is immutable zero COM/identity orientation; replay never mutates that singleton.
            const PhysicsStateConstructionCom fixed;
            return Exact(joint.secondCom.position, fixed.position) && Exact(joint.secondCom.rotation, fixed.rotation) &&
                   ExactPose(joint.secondLocalOrWorldFrame, joint.secondConstructorFrame);
        }

        Result<void> Endpoints(const PhysicsStateConstructionManifest &manifest, const PhysicsStateConstructionJoint &joint) {
            if (!HasSceneBody(manifest, joint.first))
                return MalformedConstruction();
            if (joint.second.has_value()) {
                if (*joint.second == joint.first || !HasSceneBody(manifest, *joint.second))
                    return MalformedConstruction();
                if (joint.collisionPolicy == PhysicsJointCollisionPolicy::AllowBetweenBodies)
                    return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            } else if (!FixedEndpoint(joint))
                return MalformedConstruction();
            return Result<void>::Success();
        }

        Result<void> Joint(const PhysicsStateConstructionManifest &m, const PhysicsStateConstructionJoint &j,
                           PhysicsStateFpControl current) {
            if (auto endpoints = Endpoints(m, j); endpoints.HasError())
                return endpoints;
            if (j.floatingPoint != current)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            if (!ComValid(j.firstCom) || !ComValid(j.secondCom))
                return MalformedConstruction();
            if (auto frames = Frames(j); frames.HasError())
                return frames;
            return Detail::ValidateCanonicalConstraintParameters(j.parameters, j.collisionPolicy);
        }
    }  // namespace

    Result<void> ValidateConstructionJoints(const PhysicsStateConstructionManifest &m, PhysicsStateFpControl current) {
        if (!Ordered(m.joints, &PhysicsStateConstructionJoint::id))
            return MalformedConstruction();
        for (const auto &joint : m.joints)
            if (auto valid = Joint(m, joint, current); valid.HasError())
                return valid;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::StateDetail
