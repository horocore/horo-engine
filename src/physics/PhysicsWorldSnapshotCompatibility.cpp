#include "CanonicalPhysicsStateFpControl.h"
#include "PhysicsWorldSnapshotState.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::PrepareCompatibility */
    Result<void> PhysicsWorldSnapshotState::PrepareCompatibility(const PhysicsStateCaptureConfiguration &configuration) {
        const auto &build = *world->runtime->stateBuild;
        auto topology = workspace->TopologyFingerprint(world->native);
        if (topology.HasError())
            return Result<void>::Failure(topology.ErrorValue());
        auto floatingPoint = ReadCanonicalPhysicsStateFpControl();
        if (floatingPoint.HasError())
            return Result<void>::Failure(floatingPoint.ErrorValue());
        if (floatingPoint.Value() != build.floatingPoint)
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        compatibility.build = build.build;
        compatibility.platform = build.platform;
        compatibility.solver = build.solver;
        compatibility.floatingPoint = floatingPoint.Value();
        compatibility.settings = world->settings.Identity();
        compatibility.content = configuration.content;
        compatibility.topology = topology.Value();
        compatibility.filters = workspace->FilterFingerprint();
        compatibility.surfaces = workspace->SurfaceFingerprint();
        return ValidatePhysicsStateCompatibility(compatibility);
    }
}  // namespace Horo::Physics::Detail
