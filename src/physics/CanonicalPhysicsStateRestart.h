#pragma once
#include "CanonicalPhysicsRuntime.h"
#include "PhysicsStateRestartTypes.h"

namespace Horo::Physics::Detail {
    /** @brief Cold detached canonical topology under the SAME receiving PhysicsWorld identity/runtime.
     * Actual owner must synchronously adopt or destroy the returned native ownership before releasing its receiving World.
     * The exact unpublished Scene target fence is checked by the non-installed caller; no worker may call this seam.
     * All source rows are fully checked before constructors. Failure writes only caller-private bindings and disposes private native state.
     * Successful topology is NOT complete restore/publication: full solver/event restoration, recapture and final owner arm remain
     * mandatory.
     */
    [[nodiscard]] Result<CanonicalWorldHandle> ReconstructCanonicalPhysicsStateTopology(
        CanonicalWorldHandle receiving, PhysicsWorldId owner, const PhysicsWorldSettings &, const PhysicsStateConstructionManifest &,
        const PhysicsStateConstructionManifestLimits &, std::span<const CanonicalRestartBodyRoute>,
        const PhysicsContinuousCollisionPolicy &, const CancellationToken &, CanonicalRestartBindings &privateBindings);
}  // namespace Horo::Physics::Detail
