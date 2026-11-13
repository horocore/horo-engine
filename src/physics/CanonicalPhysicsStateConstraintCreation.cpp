#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateFpControl.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Direct actual SIMD components, not a derived origin/COM subtraction
         * or quaternion-axis roundtrip. */
        CanonicalConstraintEndpointCreation Endpoint(const JPH::Body &body) noexcept {
            const auto position = body.GetCenterOfMassPosition();
            const auto rotation = body.GetRotation();
            return {{position.GetX(), position.GetY(), position.GetZ()},
                    {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()}};
        }
    }  // namespace

    /** @copydoc CaptureCanonicalConstraintCreation */
    Result<CanonicalConstraintCreation> CaptureCanonicalConstraintCreation(const JPH::Body &first, const JPH::Body &second,
                                                                           const PhysicsPose &firstFrame, const PhysicsPose &secondFrame) {
        auto floatingPoint = ReadCanonicalPhysicsStateFpControl();
        if (floatingPoint.HasError())
            return Result<CanonicalConstraintCreation>::Failure(floatingPoint.ErrorValue());
        return Result<CanonicalConstraintCreation>::Success(
            {firstFrame, secondFrame, Endpoint(first), Endpoint(second), floatingPoint.Value()});
    }
}  // namespace Horo::Physics::Detail
