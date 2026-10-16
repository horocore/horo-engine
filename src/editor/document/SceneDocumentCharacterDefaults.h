#pragma once

/** @file SceneDocumentCharacterDefaults.h
 * @brief Private authored Character defaults sharing the existing Scene command/history authority.
 */
#include "editor/document/SceneDocumentInternalCore.h"

namespace Horo::Editor::SceneDocumentDetail {
    /** @brief Chooses the first free local component identity; project bindings deliberately remain absent. */
    [[nodiscard]] inline Runtime::CharacterControllerComponent DefaultCharacter(const SceneObjectComponentSet &components) {
        std::uint64_t identity = 1;
        const auto used = [&](std::uint64_t value) {
            return (components.rigidBody && components.rigidBody->id.value == value) ||
                   std::ranges::any_of(components.colliders, [value](const auto &collider) {
                return collider.id.value == value;
            }) || std::ranges::any_of(components.physicsConstraints, [value](const auto &constraint) {
                return constraint.id.value == value;
            });
        };
        while (used(identity))
            ++identity;
        return {.id = {identity}};
    }

}  // namespace Horo::Editor::SceneDocumentDetail
