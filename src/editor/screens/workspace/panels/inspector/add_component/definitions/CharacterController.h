#pragma once
/** @file CharacterController.h @brief Static CharacterController Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddCharacterController(const AddComponentContext &context) noexcept {
        return context.physicsEditable && !context.object.components.characterController;
    }

    /** @brief Inert label/category/action mapping for CharacterController. */
    inline constexpr BuiltinAddComponentDefinition CharacterController{"component.character_controller",
                                                                       "workspace.inspector.character.title",
                                                                       AddComponentCategory::Physics,
                                                                       ComponentType::CharacterController,
                                                                       CanAddCharacterController,
                                                                       false};
}  // namespace Horo::Editor::AddComponentDefinitions
