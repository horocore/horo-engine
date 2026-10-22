#pragma once

#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

namespace Horo::Runtime::PhysicsValidationDetail {
    /** @brief Distinguish persisted Scene identities from transient prefab-local endpoints before canonical remapping. */
    enum class BodyReferenceDomain {
        Scene,
        PrefabLocal
    };
    /** @brief Validate all collider fields in the given identity domain; local object zero denotes the prefab root. */
    [[nodiscard]] Result<void> ValidateCollider(const ColliderComponent &component, BodyReferenceDomain domain);
    /** @brief Validate all constraint fields without weakening ordinary Scene/runtime identity admission. */
    [[nodiscard]] Result<void> ValidateConstraint(const PhysicsConstraintComponent &component, BodyReferenceDomain domain);
}  // namespace Horo::Runtime::PhysicsValidationDetail
