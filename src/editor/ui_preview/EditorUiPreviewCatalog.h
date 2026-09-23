#pragma once

/** @file EditorUiPreviewCatalog.h
 * @brief Explicit editor UI preview scenarios available from the development CLI.
 */

#include <array>
#include <string_view>

namespace Horo::Editor {
    inline constexpr float EditorUiPreviewHeaderHeight = 64.0f;
    inline constexpr float EditorUiPreviewSidebarWidth = 240.0f;

    struct EditorUiPreviewScenario {
        std::string_view id;
        std::string_view titleKey;
        std::string_view description;
    };

    inline constexpr std::array<EditorUiPreviewScenario, 2> EditorUiPreviewScenarios{{
        {"asset-import", "ui_preview.asset_import", "Asset Import modal with representative files and importer settings"},
        {"asset-import-empty", "ui_preview.asset_import_empty", "Asset Import modal before files are selected"},
    }};
}  // namespace Horo::Editor
