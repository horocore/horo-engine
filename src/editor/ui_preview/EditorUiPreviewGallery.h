#pragma once

/** @file EditorUiPreviewGallery.h
 * @brief Native editor UI preview canvas and scenario navigation.
 */

#include <optional>
#include <string_view>

namespace Horo::Editor {
    class LocalizationService;

    namespace Theme {
        struct Fonts;
    }

    /**
     * @brief Draws the isolated preview gallery behind the active workflow surface.
     * @param scenarioId Active preview scenario.
     * @param modalOpen Whether the scenario's workflow surface is currently open.
     * @param fonts Editor font handles.
     * @param localization Active editor localization service.
     * @return Scenario selected for reopening, if any.
     */
    [[nodiscard]] std::optional<std::string_view> DrawEditorUiPreviewGallery(std::string_view scenarioId, bool modalOpen,
                                                                             const Theme::Fonts &fonts,
                                                                             const LocalizationService &localization);
}  // namespace Horo::Editor
