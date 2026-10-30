#pragma once
/** @file AddComponentMenu.h @brief Registry-only menu rendering and typed selection dispatch. */
#include "AddComponentRegistry.h"
#include "Horo/Editor/EditorGuiContext.h"

namespace Horo::Editor {
    /** @brief Draws current registered categories and entries; invokes no source/draft mutations.
     * @param registry Successfully synchronized catalog. @param state Current selection and draft.
     * @param context Borrowed presentation services. @return Selected allowed intent or no selection. */
    [[nodiscard]] std::optional<AddComponentAction> DrawAddComponentMenu(const AddComponentRegistry &registry,
                                                                         const AddComponentContext &state, const EditorGuiContext &context);
    /** @brief Translates a selected intent into existing commands or detached Physics drafts.
     * @param action Selected typed intent. @param object Current selected object.
     * @param projectRoot Exact workspace project root. @param physics Panel-owned detached Physics draft.
     * @param command Receives an existing canonical command; no document is published here. */
    void ApplyAddComponentAction(const AddComponentAction &action, const SceneObject &object, std::string_view projectRoot,
                                 PhysicsInspectorSession &physics, EditorWorkspaceViewCommandData &command);
}  // namespace Horo::Editor
