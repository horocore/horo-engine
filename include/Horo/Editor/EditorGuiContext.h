#pragma once

/** @file EditorGuiContext.h
 * @brief Borrowed GUI services, theme and optional drawing registry.
 */

#include "Horo/Editor/GuiLiveReload.h"

#include <cstddef>
#include <span>
#include <string>

namespace Horo {
    class EngineDataBus;
}

namespace Horo::Input {
    class InputRouter;
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

    /** @brief Editor-facing microphone permission status, independent of any capture session. */
    enum class EditorMicrophonePermissionStatus {
        Unavailable,
        Required,
        Pending,
        Granted,
        Denied
    };

    /** @brief State of the temporary editor microphone connection test. */
    enum class EditorMicrophoneTestStatus {
        Idle,
        Starting,
        Capturing,
        Failed
    };

    /** @brief Host-owned permission surface. Only explicit user activation may issue a request. */
    class IEditorMicrophonePermission {
    public:
        virtual ~IEditorMicrophonePermission() = default;
        /** @brief Returns the current permission status on the UI owner. */
        [[nodiscard]] virtual EditorMicrophonePermissionStatus Status() = 0;
        /** @brief Requests permission without opening a recording device. */
        virtual void Request() = 0;
        /** @brief Queues an input device refresh without opening a recording device. */
        virtual void ScanInputs() noexcept = 0;
        /** @brief Borrows available explicit input labels until the next input refresh. */
        [[nodiscard]] virtual std::span<const std::string> Inputs() const noexcept = 0;
        /** @brief Queues an explicit microphone connection test for the selected input. */
        virtual void StartTest(std::size_t input) noexcept = 0;
        /** @brief Cancels queued work and requests detachment of any live test at the next control update. */
        virtual void StopTest() noexcept = 0;
        /** @brief Reconciles queued work, focus, timeout and capture health outside GUI drawing. */
        virtual void AdvanceTest(bool foreground) noexcept = 0;
        /** @brief Reports the test state without polling native resources. */
        [[nodiscard]] virtual EditorMicrophoneTestStatus TestStatus() const noexcept = 0;

        /** @brief Returns the current microphone test display level in the 0–1 range, or zero when unavailable. */
        [[nodiscard]] virtual float TestPeakLevel() const noexcept {
            return 0.0F;
        }
    };

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
        UpdateExperienceSession *updates = nullptr;      /**< Borrowed host-owned update workflow, when configured. */
        const Input::InputRouter *inputRouter = nullptr; /**< Borrowed host-owned effective binding authority, outliving GUI draws. */
        IEditorMicrophonePermission *microphonePermission = nullptr; /**< Optional host-owned microphone permission controls. */
    };
}  // namespace Horo::Editor
