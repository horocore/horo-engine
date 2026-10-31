#pragma once

/** @file EditorGuiContext.h
 * @brief Borrowed GUI services, theme and optional drawing registry.
 */

#include "Horo/Editor/GuiLiveReload.h"

namespace Horo {
    class EngineDataBus;
}

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace Horo::Editor {
    class EditorDataBus;
    class NotificationService;
    class ILocalizationService;
    class UpdateExperienceSession;
    struct EditorSettingsSnapshot;

    /** @brief Provides theme configuration and font atlas access for the GUI. */
    struct ThemeContext {
        const Theme::Fonts &fonts;
        const GuiLive::IRegistry *liveReload = nullptr; /**< Borrowed; outlives every GUI frame using this context. */
    };

    /**
     * @brief Consolidated read-only context required by editor UI rendering passes.
     *
     * Groups common immutable services, buses, and settings snapshots, avoiding
     * parameter bloat in GUI components while decoupling them from global state.
     */
    struct EditorGuiContext {
        EngineDataBus &engineEvents;
        EditorDataBus &editorEvents;
        ILocalizationService &localization;
        const ThemeContext &theme;
        const EditorSettingsSnapshot &settings;
        UpdateExperienceSession *updates = nullptr; /**< Borrowed host-owned update workflow, when configured. */
    };
}  // namespace Horo::Editor
