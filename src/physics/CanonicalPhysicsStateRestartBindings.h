#pragma once
#include "CanonicalPhysicsStateRestartShapes.h"
#include "Horo/Physics/PhysicsStateConstructionManifest.h"
#include "PhysicsStateRestartTypes.h"

namespace Horo::Physics::Detail {
    [[nodiscard]] Result<void> RegisterCanonicalRestartSceneShapes(CanonicalWorld &, PhysicsWorldId,
                                                                   const PhysicsStateConstructionManifest &, const CanonicalRestartShapes &,
                                                                   CanonicalRestartBindings &);
    [[nodiscard]] Result<void> CreateCanonicalRestartBodies(CanonicalWorldHandle, PhysicsWorldId, const PhysicsStateConstructionManifest &,
                                                            std::span<const CanonicalRestartBodyRoute>, CanonicalRestartBindings &,
                                                            const CancellationToken &);
    [[nodiscard]] Result<void> CreateCanonicalRestartFixtures(CanonicalWorldHandle, PhysicsWorldId,
                                                              const PhysicsStateConstructionManifest &, const CanonicalRestartShapes &,
                                                              CanonicalRestartBindings &, const CancellationToken &);
    [[nodiscard]] Result<void> CreateCanonicalRestartJoints(CanonicalWorldHandle, PhysicsWorldId, const PhysicsStateConstructionManifest &,
                                                            CanonicalRestartBindings &, const CancellationToken &);
}  // namespace Horo::Physics::Detail
