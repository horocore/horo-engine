#pragma once
#include "CanonicalPhysicsStateBackendInternal.h"

namespace Horo::Physics::Detail {
    [[nodiscard]] Result<PhysicsEventEndpoint> BindStateSceneEvent(const CanonicalWorld &world, const CanonicalSceneBodyRecord &body,
                                                                   const PhysicsStateEventEndpoint &endpoint,
                                                                   const std::optional<PhysicsEventMaterial> &material,
                                                                   const std::optional<PhysicsSurfaceEvidence> &surface);
    [[nodiscard]] Result<PhysicsEventEndpoint> BindStateFixtureEvent(const CanonicalWorld &world, const CanonicalQueryFixtureRecord &body,
                                                                     const PhysicsStateEventEndpoint &endpoint,
                                                                     const std::optional<PhysicsEventMaterial> &material,
                                                                     const std::optional<PhysicsSurfaceEvidence> &surface);
    [[nodiscard]] bool SameStateMaterial(const std::optional<PhysicsQueryMaterial> &actual,
                                         const std::optional<PhysicsEventMaterial> &material) noexcept;
}  // namespace Horo::Physics::Detail
