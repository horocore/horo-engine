#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/SettingsModalDraft.h"
#include "SettingsModalInternal.h"

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

        out.themePreset =
            st.appearance.themeIndex < 3 ? static_cast<EditorThemePreset>(std::max(st.appearance.themeIndex, 0)) : st.committed.themePreset;
        out.themeId = st.appearance.themeId;
        out.accentColorHex = st.appearance.accentHex;
        out.uiScalePercent = st.appearance.uiScale;
        out.codeEditor = st.appearance.codeEditor;
        out.uiFontFamily = st.appearance.uiFontFamily;
        out.codeFontFamily = st.appearance.codeFontFamily;
        out.codeFontSizePx = std::atoi(st.appearance.editorFontSize.c_str());

        out.orbitSensitivity = st.input.orbitSensitivity;
        out.panSensitivity = st.input.panSensitivity;
        out.invertOrbitY = st.input.invertOrbitY;
        out.keymap = st.input.keymap;

        out.viewportMode = static_cast<EditorViewportMode>(std::clamp(st.rendering.viewportMode, 0, 3));
        out.gridOverlay = st.rendering.gridOverlay;
        out.renderingTier = static_cast<EditorRenderingTier>(std::clamp(st.rendering.renderingTier, 0, 3));
        out.textureStreamingBudget = st.rendering.textureBudget;

        out.masterVolume = st.audio.masterVolume;
        out.audioOutputDevice = static_cast<EditorAudioOutputDevice>(std::clamp(st.audio.audioOutputDevice, 0, 2));
        out.audioEnabled = st.audio.audioEnabled;

        out.navigationOverlay = st.navigationOverlay;
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
        st.appearance.pendingThemeIndex = -1;
        st.general.startupAction = static_cast<int>(settings.startupBehavior);
        st.general.autoSaveInterval = settings.autoSaveIntervalMinutes;
        st.general.confirmExit = settings.confirmExitWithUnsavedChanges;
        st.general.restoreWorkspace = settings.restoreWorkspaceLayout;
        st.general.defaultScene = settings.defaultSceneOnProjectOpen;
        st.general.languageTag = settings.languageTag;

        st.appearance.themeId = settings.themeId;
        st.appearance.themeIndex = static_cast<int>(settings.themePreset);
        const auto &themes = Theme::GetThemeList();
        for (std::size_t i = 0; i < themes.size(); ++i)
            if (!settings.themeId.empty() && themes[i].id == settings.themeId)
                st.appearance.themeIndex = static_cast<int>(i);
        st.appearance.uiScale = settings.uiScalePercent;
        st.appearance.codeEditor = settings.codeEditor;
        st.appearance.uiFontFamily = settings.uiFontFamily;
        st.appearance.codeFontFamily = settings.codeFontFamily;
        st.appearance.editorFontSize = std::to_string(settings.codeFontSizePx);
        st.appearance.accentHex = settings.accentColorHex;

        st.input.orbitSensitivity = settings.orbitSensitivity;
        st.input.panSensitivity = settings.panSensitivity;
        st.input.invertOrbitY = settings.invertOrbitY;
        st.input.keymap = settings.keymap;
        st.input.recordingAction.clear();

        st.rendering.viewportMode = static_cast<int>(settings.viewportMode);
        st.rendering.gridOverlay = settings.gridOverlay;
        st.rendering.renderingTier = static_cast<int>(settings.renderingTier);
        st.rendering.textureBudget = settings.textureStreamingBudget;

        st.audio.masterVolume = settings.masterVolume;
        st.audio.audioOutputDevice = static_cast<int>(settings.audioOutputDevice);
        st.audio.audioEnabled = settings.audioEnabled;

        st.navigationOverlay = settings.navigationOverlay;
        st.network.maxPreviewClients = static_cast<int>(settings.networkPreviewPreferences.maxPreviewClients);
        st.network.simulatedLatencyMs = static_cast<int>(settings.networkPreviewPreferences.simulatedLatencyMilliseconds);
        st.packages.downloadThreads = settings.packages.downloadThreads;

        st.diagnostics.consoleLogLevel = static_cast<int>(settings.consoleLogLevel);
        st.diagnostics.writeLogToFile = settings.writeLogToFile;
        st.diagnostics.autoCaptureStutter = settings.autoCaptureOnStutter;
        st.diagnostics.stutterThresholdMs = settings.stutterThresholdMs;
    }

    /** @copydoc SettingsModal.h */
    void LoadSettingsForModal(SettingsState &state, EditorSettingsService &settings) {
        const EditorSettingsSnapshot snapshot = settings.Snapshot();
        state.committed = snapshot.settings;
        state.settingsRevision = snapshot.revision;
        state.configurationRevision = snapshot.configurationRevision;
        state.configuration = std::make_shared<ConfigurationSettingsSession>(settings);
        state.configurationFailure.reset();
        ApplySettingsToDraft(state, state.committed);
        state.dirty = false;
        state.initialized = true;
        state.status = SettingsModalStatus::Loaded;
        state.configurationPreviewed = false;
    }

    namespace {
        /** @brief Commits only the active project session while preserving the pending user draft. */
        bool ApplyProjectConfiguration(SettingsState &state) {
            if (!state.projectConfiguration) {
                state.configurationFailure = state.projectConfigurationError;
                state.status = SettingsModalStatus::Failure;
                return false;
            }
            const auto applied = state.projectConfiguration->Apply();
            state.configurationFailure = applied.HasError() ? std::optional<Error>{applied.ErrorValue()} : std::nullopt;
            state.status = applied.HasError() ? SettingsModalStatus::Failure : SettingsModalStatus::Applied;
            return applied.HasValue();
        }
    }  // namespace

    /** @copydoc SettingsModal.h */
    [[nodiscard]] bool ApplySettings(SettingsState &state, EditorSettingsService &settings) {
        if (static_cast<SettingsModalInternal::SettingsTab>(state.activeTab) == SettingsModalInternal::SettingsTab::ProjectConfiguration) {
            return ApplyProjectConfiguration(state);
        }
        if (state.navigationProjectDraftDirty)
            return false;
        if (const auto owner = state.prefab.owner.lock(); owner && owner->IsDirty())
            return false;
        const EditorSettings draft = CollectDraftSettings(state);
        if (state.configuration) {
            for (const auto &field : state.configuration->Fields()) {
                if (field.error) {
                    state.configurationFailure = *field.error;
                    state.status = SettingsModalStatus::Failure;
                    return false;
                }
            }
        }
        const ConfigurationDraft *configurationDraft = state.configuration ? &state.configuration->Draft() : nullptr;
        const Result<EditorSettingsSnapshot> result =
            settings.Commit(EditorSettingsDraft{.baseRevision = state.settingsRevision,
                                                .settings = draft,
                                                .baseConfigurationRevision = state.configurationRevision},
                            configurationDraft);
        if (result.HasError()) {
            const bool previewed = state.configurationPreviewed || CollectDraftSettings(state) != state.committed;
            if (result.ErrorValue().code.Value() == "configuration.draft_stale" ||
                result.ErrorValue().code.Value() == "editor.settings.draft_stale")
                LoadSettingsForModal(state, settings);
            state.configurationFailure = result.ErrorValue();
            state.status = SettingsModalStatus::Failure;
            if (previewed) {
                state.appearance.pendingThemeIndex = state.appearance.themeIndex;
                state.configurationPreviewed = true;
            }
            state.dirty = true;
            return false;
        }

        state.committed = result.Value().settings;
        state.settingsRevision = result.Value().revision;
        state.configurationRevision = result.Value().configurationRevision;
        state.configurationFailure.reset();
        if (state.configuration)
            state.configuration->Reload();
        ApplySettingsToDraft(state, state.committed);
        state.dirty = false;
        state.status = SettingsModalStatus::Applied;
        state.configurationPreviewed = false;
        return true;
    }
}  // namespace Horo::Editor
