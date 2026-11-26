#pragma once

/** @file ViewportPlayCaptions.h @brief Desktop Editor Play preview for semantic runtime captions. */

struct ImDrawList;
struct ImVec2;

namespace Horo {
    class ConfigurationService;
}

namespace Horo::Runtime::Ui {
    class CaptionTimeline;
}

namespace Horo::Editor {
    class LocalizationService;
    namespace Theme {
        struct Fonts;
    }

    /** @brief Borrows the owner-lane authorities for one desktop Play preview. */
    struct EditorPlayCaptionPreview final {
        const Runtime::Ui::CaptionTimeline &timeline;
        const ConfigurationService &configuration;
        const LocalizationService &localization;
    };

    /** @brief Draws current captions in the desktop preview using canonical caption placement.
     * @param draw Active editor viewport draw list.
     * @param origin Screen position of the viewport in GUI logical pixels.
     * @param extent Current visible viewport width and height in GUI logical pixels.
     * @param fonts Borrowed editor theme fonts for the current frame.
     * @param preview Owner-lane caption, configuration and locale sources.
     */
    void DrawViewportPlayCaptions(ImDrawList &draw, const ImVec2 &origin, const ImVec2 &extent, const Theme::Fonts &fonts,
                                  const EditorPlayCaptionPreview &preview);
}  // namespace Horo::Editor
