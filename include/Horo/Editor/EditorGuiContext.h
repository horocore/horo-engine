#pragma once

/** @file EditorGuiContext.h
 * @brief Borrowed GUI services, theme and optional drawing registry.
 */

#include "Horo/Editor/GuiLiveReload.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

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

    /** @brief Permission-scoped recording and background save state. */
    enum class EditorMicrophoneRecordingStatus {
        Idle,
        Starting,
        Recording,
        Saving,
        Saved,
        Failed
    };

    /** @brief Optional project recording controls backed by the same owner as microphone permission. */
    class IEditorMicrophoneRecorder {
    public:
        virtual ~IEditorMicrophoneRecorder() = default;
        /** @brief Binds the active workspace project, or an empty path when no project can accept a recording. */
        virtual void SetProject(const std::filesystem::path &root) = 0;
        /** @brief True when a project is active and no capture or save already occupies this owner. */
        [[nodiscard]] virtual bool CanRecord() const noexcept = 0;
        /** @brief Requests a foreground recording to one explicit new project-relative WAVE path. */
        virtual void StartRecording(std::size_t input, std::string_view wavePath) = 0;
        /** @brief Requests normal stop and background publication. */
        virtual void SaveRecording() noexcept = 0;
        /** @brief Cancels the captured take, queued start or pending save. */
        virtual void CancelRecording() noexcept = 0;
        /** @brief Returns the current control-lane projection. */
        [[nodiscard]] virtual EditorMicrophoneRecordingStatus RecordingStatus() const noexcept = 0;
        /** @brief Returns normalized recorded peak, or zero outside active capture. */
        [[nodiscard]] virtual float RecordingPeakLevel() const noexcept = 0;
        /** @brief Returns the published project-relative path until the next recording action. */
        [[nodiscard]] virtual std::string_view SavedRecordingPath() const noexcept = 0;
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
        IEditorMicrophoneRecorder *microphoneRecorder = nullptr;     /**< Optional host-owned project recording controls. */
    };
}  // namespace Horo::Editor
