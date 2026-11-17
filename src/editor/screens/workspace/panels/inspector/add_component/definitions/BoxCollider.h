#pragma once
/** @file BoxCollider.h @brief Static BoxCollider Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddBoxCollider(const AddComponentContext &context) noexcept {
        return context.physicsEditable && context.physics.colliders.size() < Runtime::MaximumPhysicsCollidersPerBody;
    }

    /** @brief Inert label/category/action mapping for BoxCollider. */
    inline constexpr BuiltinAddComponentDefinition BoxCollider{"physics.box_collider",
                                                               "workspace.physics.add_collider",
                                                               AddComponentCategory::Physics,
                                                               AddPhysicsComponent::BoxCollider,
                                                               CanAddBoxCollider,
                                                               false};
}  // namespace Horo::Editor::AddComponentDefinitions
