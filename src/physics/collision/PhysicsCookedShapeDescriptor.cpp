#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>

namespace Horo::Physics {
    namespace {
        constexpr std::array SupportedCookedShapeKinds{PhysicsCookedShapeKind::ConvexHull, PhysicsCookedShapeKind::TriangleMesh,
                                                       PhysicsCookedShapeKind::HeightField, PhysicsCookedShapeKind::Compound};
    }  // namespace

    /** @copydoc ValidatePhysicsCookedShapeDescriptor */
    Result<void> ValidatePhysicsCookedShapeDescriptor(const PhysicsCookedShapeDescriptor &descriptor,
                                                      const PhysicsShapeCookTargetDigest &expectedTarget) {
        if (!descriptor.asset.IsValid() || !descriptor.subresource.IsValid())
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Cooked shape requires exact asset and subresource identities."));
        if (!descriptor.cacheKeyDigest || !descriptor.payloadDigest || !descriptor.target)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Cooked shape requires cache key, payload and target digests."));
        if (std::ranges::find(SupportedCookedShapeKinds, descriptor.kind) == SupportedCookedShapeKinds.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown cooked shape kind."));
        if (*descriptor.target != expectedTarget)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::ProfileUnsupported, "Cooked shape target does not match the captured Physics target."));
        return Result<void>::Success();
    }
}  // namespace Horo::Physics
