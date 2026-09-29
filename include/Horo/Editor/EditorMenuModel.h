#pragma once

#include "Horo/Runtime/Scene/PrimitiveCatalog.h"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /**
     * @file EditorMenuModel.h
     * @brief Platform-neutral editor application menu hierarchy.
     */

    /** @brief Stable editor actions emitted by native and in-window menu renderers. */
    enum class EditorMenuAction {
        None,
        NewProject,
        OpenProject,
        SaveScene,
        SaveSceneAs,
        SaveSceneCopyAs,
        Undo,
        Redo,
        OpenEditorSettings,
        ImportAssets,
        ExitApplication,
        CreatePrimitive,
    };

    /** @brief Typed command emitted by every platform menu adapter. */
    struct EditorMenuInvocation {
        EditorMenuAction action{EditorMenuAction::None};       /**< Requested application command. */
        std::optional<Runtime::PrimitiveId> primitive;         /**< Optional primitive selected by Create. */
        std::optional<std::filesystem::path> assetDestination; /**< Absolute Import Assets destination override. */
    };

    /** @brief Structural kind of one menu model entry. */
    enum class EditorMenuItemKind {
        Command,
        Separator,
        Submenu,
    };

    /** @brief Platform-neutral menu item including localization, command, and child metadata. */
    struct EditorMenuItem {
        EditorMenuItemKind kind{EditorMenuItemKind::Command};
        std::string_view labelKey;
        EditorMenuAction action{EditorMenuAction::None};
        std::string_view shortcut;
        std::string_view macKeyEquivalent;
        std::string_view iconToken;
        std::optional<Runtime::PrimitiveId> primitive;
        bool enabledByDefault{false};
        std::vector<EditorMenuItem> children;
    };

    /** @brief Complete ordered application menu hierarchy shared by every platform renderer. */
    struct EditorMenuModel {
        std::vector<EditorMenuItem> menus;
    };

    /**
     * @brief Returns the immutable editor application menu model.
     * @return Process-lifetime menu model whose string views reference static storage.
     */
    [[nodiscard]] const EditorMenuModel &GetEditorMenuModel();

    /**
     * @brief Returns the catalog-generated children of the shared Create submenu.
     * @return Process-lifetime menu items shared by menu bar and hierarchy adapters.
     */
    [[nodiscard]] const std::vector<EditorMenuItem> &GetPrimitiveCreateMenuItems();
}  // namespace Horo::Editor
