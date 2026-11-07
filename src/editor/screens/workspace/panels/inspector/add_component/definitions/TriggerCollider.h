#pragma once
/** @file TriggerCollider.h @brief Static TriggerCollider Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddTriggerCollider(const AddComponentContext &context) noexcept {
        return context.physicsEditable && context.physics.colliders.size() < Runtime::MaximumPhysicsCollidersPerBody;
    }

    /** @brief Inert label/category/action mapping for TriggerCollider. */
    inline constexpr BuiltinAddComponentDefinition TriggerCollider{"physics.trigger_collider",    "workspace.physics.add_trigger",
                                                                   AddComponentCategory::Physics, AddPhysicsComponent::TriggerCollider,
                                                                   CanAddTriggerCollider,         false};
}  // namespace Horo::Editor::AddComponentDefinitions
