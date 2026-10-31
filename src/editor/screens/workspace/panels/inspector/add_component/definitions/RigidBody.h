#pragma once
/** @file RigidBody.h @brief Static RigidBody Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddRigidBody(const AddComponentContext &context) noexcept {
        return context.physicsEditable && !context.physics.body &&
               context.physics.colliders.size() < Runtime::MaximumPhysicsCollidersPerBody;
    }

    /** @brief Inert label/category/action mapping for RigidBody. */
    inline constexpr BuiltinAddComponentDefinition RigidBody{"physics.body",
                                                             "workspace.physics.add_body",
                                                             AddComponentCategory::Physics,
                                                             AddPhysicsComponent::Body,
                                                             CanAddRigidBody,
                                                             false};
}  // namespace Horo::Editor::AddComponentDefinitions
