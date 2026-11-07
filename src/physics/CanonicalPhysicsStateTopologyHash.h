#pragma once
#include "CanonicalPhysicsStateTopologyMap.h"
#include "PhysicsStateHashInternal.h"

namespace Horo::Physics::Detail {
    void HashStateMaterial(StateDetail::StateHash &hash, const PhysicsQueryMaterial &material) noexcept;
    void HashStateSurface(StateDetail::StateHash &hash, const PhysicsSurfaceEvidence &surface) noexcept;
    void HashStateNativeBody(StateDetail::StateHash &hash, const JPH::Body &body) noexcept;
    [[nodiscard]] Result<void> HashStateSceneBody(StateDetail::StateHash &hash, const CanonicalWorld &world,
                                                  const CanonicalPhysicsStateBodyBinding &binding,
                                                  const CanonicalPhysicsStateBodyRecordIndex &record,
                                                  const CanonicalPhysicsStateTopologyMap &map);
    [[nodiscard]] Result<void> HashStateFixtureBody(StateDetail::StateHash &hash, const CanonicalWorld &world,
                                                    const CanonicalPhysicsStateBodyBinding &binding,
                                                    const CanonicalPhysicsStateBodyRecordIndex &record,
                                                    const CanonicalPhysicsStateTopologyMap &map);
    [[nodiscard]] Result<void> HashStateJoint(StateDetail::StateHash &hash, const CanonicalWorld &world,
                                              const CanonicalPhysicsStateJointBinding &binding, std::size_t record,
                                              const CanonicalPhysicsStateBindings &bindings);
}  // namespace Horo::Physics::Detail
