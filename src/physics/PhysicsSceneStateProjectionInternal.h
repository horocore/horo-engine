#pragma once
#include "PhysicsSceneStateProjection.h"

namespace Horo::Physics::Detail {
    [[nodiscard]] Result<SceneStateKey> ResolveSceneStateKey(const Runtime::PersistentEntityIdentityMap &, Runtime::EntityRef,
                                                             SceneStateKeyRole, std::uint64_t slot);
    [[nodiscard]] bool SceneStateOwnerCountsFit(const CanonicalStateSceneCounts &, const PhysicsStructuralState &,
                                                SceneStateProjectionLimits) noexcept;
}  // namespace Horo::Physics::Detail
