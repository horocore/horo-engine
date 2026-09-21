#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/SettingsModalDraft.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc SettingsModal.h */
    [[nodiscard]] EditorSettings CollectDraftSettings(const SettingsState &st) {
        EditorSettings out{};
        out.startupBehavior = static_cast<EditorStartupBehavior>(std::clamp(st.general.startupAction, 0, 2));
        out.autoSaveIntervalMinutes = st.general.autoSaveInterval;
        out.confirmExitWithUnsavedChanges = st.general.confirmExit;
        out.restoreWorkspaceLayout = st.general.restoreWorkspace;
        out.defaultSceneOnProjectOpen = st.general.defaultScene;
        out.languageTag = st.general.languageTag;

        out.themePreset = static_cast<EditorThemePreset>(st.appearance.themeIndex);
        out.accentColorHex = st.appearance.accentHex;
        out.uiScalePercent = st.appearance.uiScale;
        out.codeFontSizePx = std::atoi(st.appearance.editorFontSize.c_str());

        out.orbitSensitivity = st.input.orbitSensitivity;
        out.panSensitivity = st.input.panSensitivity;
        out.invertOrbitY = st.input.invertOrbitY;

        out.viewportMode = static_cast<EditorViewportMode>(std::clamp(st.rendering.viewportMode, 0, 3));
        out.gridOverlay = st.rendering.gridOverlay;
        out.renderingTier = static_cast<EditorRenderingTier>(std::clamp(st.rendering.renderingTier, 0, 3));
        out.textureStreamingBudget = st.rendering.textureBudget;

        out.masterVolume = st.audio.masterVolume;
        out.audioOutputDevice = static_cast<EditorAudioOutputDevice>(std::clamp(st.audio.audioOutputDevice, 0, 2));
        out.audioEnabled = st.audio.audioEnabled;

        out.networkPreviewPreferences.maxPreviewClients = static_cast<std::uint32_t>(std::max(st.network.maxPreviewClients, 0));
        out.networkPreviewPreferences.simulatedLatencyMilliseconds = static_cast<std::uint32_t>(std::max(st.network.simulatedLatencyMs, 0));
        out.packages.downloadThreads = st.packages.downloadThreads;

        out.consoleLogLevel = static_cast<EditorConsoleLogLevel>(std::clamp(st.diagnostics.consoleLogLevel, 0, 3));
        out.writeLogToFile = st.diagnostics.writeLogToFile;
        out.autoCaptureOnStutter = st.diagnostics.autoCaptureStutter;
        out.stutterThresholdMs = st.diagnostics.stutterThresholdMs;

        return out;
    }

    /** @copydoc SettingsModal.h */
    void ApplySettingsToDraft(SettingsState &st, const EditorSettings &settings) {
        st.general.startupAction = static_cast<int>(settings.startupBehavior);
        st.general.autoSaveInterval = settings.autoSaveIntervalMinutes;
        st.general.confirmExit = settings.confirmExitWithUnsavedChanges;
        st.general.restoreWorkspace = settings.restoreWorkspaceLayout;
        st.general.defaultScene = settings.defaultSceneOnProjectOpen;
        st.general.languageTag = settings.languageTag;

        st.appearance.themeIndex = static_cast<int>(settings.themePreset);
        st.appearance.uiScale = settings.uiScalePercent;
        st.appearance.editorFontSize = std::to_string(settings.codeFontSizePx);
        st.appearance.accentHex = settings.accentColorHex;

        st.input.orbitSensitivity = settings.orbitSensitivity;
        st.input.panSensitivity = settings.panSensitivity;
        st.input.invertOrbitY = settings.invertOrbitY;

        st.rendering.viewportMode = static_cast<int>(settings.viewportMode);
        st.rendering.gridOverlay = settings.gridOverlay;
        st.rendering.renderingTier = static_cast<int>(settings.renderingTier);
        st.rendering.textureBudget = settings.textureStreamingBudget;

        st.audio.masterVolume = settings.masterVolume;
        st.audio.audioOutputDevice = static_cast<int>(settings.audioOutputDevice);
        st.audio.audioEnabled = settings.audioEnabled;

        st.network.maxPreviewClients = static_cast<int>(settings.networkPreviewPreferences.maxPreviewClients);
        st.network.simulatedLatencyMs = static_cast<int>(settings.networkPreviewPreferences.simulatedLatencyMilliseconds);
        st.packages.downloadThreads = settings.packages.downloadThreads;

        st.diagnostics.consoleLogLevel = static_cast<int>(settings.consoleLogLevel);
        st.diagnostics.writeLogToFile = settings.writeLogToFile;
        st.diagnostics.autoCaptureStutter = settings.autoCaptureOnStutter;
        st.diagnostics.stutterThresholdMs = settings.stutterThresholdMs;
    }

    /** @copydoc SettingsModal.h */
    void LoadSettingsForModal(SettingsState &state, const EditorSettingsService &settings) {
        const EditorSettingsSnapshot snapshot = settings.Snapshot();
        state.committed = snapshot.settings;
        state.settingsRevision = snapshot.revision;
        ApplySettingsToDraft(state, state.committed);
        state.dirty = false;
        state.initialized = true;
        state.statusIsError = false;
        state.statusMessage = "Loaded committed editor settings.";
    }

    /** @copydoc SettingsModal.h */
    [[nodiscard]] bool ApplySettings(SettingsState &state, EditorSettingsService &settings) {
        const EditorSettings draft = CollectDraftSettings(state);
        const Result<EditorSettingsSnapshot> result =
            settings.Commit(EditorSettingsDraft{.baseRevision = state.settingsRevision, .settings = draft});
        if (result.HasError()) {
            state.statusMessage = result.ErrorValue().message;
            state.statusIsError = true;
            state.dirty = true;
            return false;
        }

        state.committed = result.Value().settings;
        state.settingsRevision = result.Value().revision;
        ApplySettingsToDraft(state, state.committed);
        state.dirty = false;
        state.statusMessage = "Settings applied.";
        state.statusIsError = false;
        return true;
    }
}  // namespace Horo::Editor
