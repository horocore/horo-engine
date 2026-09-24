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

    inline constexpr std::array<EditorUiPreviewScenario, 5> EditorUiPreviewScenarios{{
        {"viewport", "ui_preview.viewport", "Viewport controls with a representative scene"},
        {"asset-import", "ui_preview.asset_import", "Asset Import modal with representative files and importer settings"},
        {"asset-import-advanced", "ui_preview.asset_import_advanced", "Asset Import modal with advanced settings expanded"},
        {"asset-import-unsupported", "ui_preview.asset_import_unsupported", "Asset Import modal with an unsupported source file"},
        {"asset-import-empty", "ui_preview.asset_import_empty", "Asset Import modal before files are selected"},
    }};
}  // namespace Horo::Editor
