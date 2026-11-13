#pragma once
/** @file Joint.h @brief Static Joint Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddJoint(const AddComponentContext &context) noexcept {
        return context.physicsEditable && context.physics.body.has_value() &&
               context.physics.constraints.size() < Runtime::MaximumPhysicsConstraintsPerObject;
    }

    /** @brief Inert label/category/action mapping for Joint. */
    inline constexpr BuiltinAddComponentDefinition Joint{"physics.joint",
                                                         "workspace.physics.add_constraint",
                                                         AddComponentCategory::Physics,
                                                         AddPhysicsComponent::Joint,
                                                         CanAddJoint,
                                                         false};
}  // namespace Horo::Editor::AddComponentDefinitions
