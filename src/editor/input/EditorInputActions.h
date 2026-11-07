#pragma once

#include "Horo/Runtime/Input.h"

#include <optional>
#include <vector>

namespace Horo::Editor {
    inline constexpr const char *kGameplayMoveAction = "gameplay.move";
    inline constexpr auto kEditorWorkspaceInputContext = "editor.workspace";
    inline constexpr auto kActionSave = "editor.save";
    inline constexpr auto kActionUndo = "editor.undo";
    inline constexpr auto kActionRedo = "editor.redo";
    inline constexpr auto kActionDuplicate = "editor.duplicate";
    inline constexpr auto kActionDelete = "editor.delete";
    inline constexpr auto kActionToolSelect = "editor.tool.select";
    inline constexpr auto kActionToolMove = "editor.tool.move";
    inline constexpr auto kActionToolRotate = "editor.tool.rotate";
    inline constexpr auto kActionToolScale = "editor.tool.scale";
    inline constexpr auto kActionViewportFocusSelected = "editor.viewport.focus_selected";

    inline constexpr auto kActionSourceSelectAll = "editor.source.select_all";
    inline constexpr auto kActionSourceCopy = "editor.source.copy";
    inline constexpr auto kActionSourcePaste = "editor.source.paste";
    inline constexpr auto kActionSourceCut = "editor.source.cut";
    inline constexpr auto kActionSourceFind = "editor.source.find";
    inline constexpr auto kActionSourceGoToLine = "editor.source.go_to_line";
    inline constexpr auto kActionSourceZoomIn = "editor.source.zoom_in";
    inline constexpr auto kActionSourceZoomOut = "editor.source.zoom_out";
    inline constexpr auto kActionSourceZoomReset = "editor.source.zoom_reset";

    /** @brief Returns inert editor action metadata; new actions automatically appear in Settings.
     * @return Process-lifetime descriptors with no routing or registration side effects.
     */
    [[nodiscard]] std::span<const Input::ActionDescriptor> RegisteredEditorInputActions();
    /** @brief Resolves a profile override or registered defaults for one stable action.
     * @param id Action identity. @param profile Current committed or draft profile.
     * @return Bindings borrowed from the profile or inert registry; empty for an unknown action.
     */
    [[nodiscard]] std::span<const Input::InputBinding> EditorActionBindings(std::string_view id, const Input::InputBindingProfile &profile);

    /** @brief Builds validation metadata, retaining dormant module overrides without making them executable.
     * @param profile Persisted user bindings. @param contributions Currently admitted module command actions.
     * @return Builtin and live contribution descriptors plus inert descriptors for absent module overrides.
     */
    [[nodiscard]] std::vector<Input::ActionDescriptor> EditorKeymapActions(const Input::InputBindingProfile &profile,
                                                                           std::span<const Input::ActionDescriptor> contributions = {});
    /** @brief Filters a complete editor profile for the builtin workspace router. */
    [[nodiscard]] Input::InputBindingProfile BuiltinEditorInputProfile(const Input::InputBindingProfile &profile);
    /** @brief Parses a normalized extension command shortcut into one physical key binding; unsupported keys fail closed. */
    [[nodiscard]] std::optional<Input::InputBinding> ParseEditorShortcut(std::string_view shortcut);

    /** @brief Returns the authoritative editor action descriptors and default bindings. */
    [[nodiscard]] std::vector<Input::ActionDescriptor> BuildEditorInputActions();
}  // namespace Horo::Editor
