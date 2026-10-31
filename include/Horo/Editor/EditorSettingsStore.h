#pragma once

#include "Horo/Network/NetworkProjectSettings.h"
#include "Horo/Runtime/Input.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>

namespace Horo::Editor {

    /** @brief Startup surface shown when the editor process opens without a project route. */
    enum class EditorStartupBehavior {
        WelcomeScreen,
        LastProject,
        ProjectBrowser,
    };

    /** @brief Editor viewport/rendering preview mode preference. */
    enum class EditorViewportMode {
        Shaded,
        Wireframe,
        Lit,
        Unlit,
    };

    /** @brief Editor rendering feature tier preference. */
    enum class EditorRenderingTier {
        HighEnd,
        Dx12Vulkan,
        Dx11,
        Es3,
    };

    /** @brief Console log filtering level. */
    enum class EditorConsoleLogLevel {
        Debug,
        Info,
        Warning,
        Error,
    };

    /** @brief Audio output device preference for editor preview playback. */
    enum class EditorAudioOutputDevice {
        SystemDefault,
        Headphones,
        Speakers,
    };

    /** @brief Theme preset preference exposed by the Settings modal. */
    enum class EditorThemePreset {
        HoroDark,
        Midnight,
        Light,
    };

    /** @brief User-only package acquisition preferences, independent of network project authority. */
    struct EditorPackageSettings final {
        int downloadThreads = 8; /**< Parallel template and asset package download workers. */

        bool operator==(const EditorPackageSettings &) const = default;
    };

    /** @brief User-only navigation overlay interest; never read by project policy, baking or packaged runtime. */
    struct EditorNavigationOverlayPreferences final {
        bool showAreas{};
        bool showPaths{};
        bool showAgents{};
        bool operator==(const EditorNavigationOverlayPreferences &) const = default;
    };

    /** @brief User binding overrides using the Runtime Input profile schema and validation rules. */
    struct EditorKeymapSettings final {
        Input::InputBindingProfile profile;

        /** @brief Compares profile values without introducing another binding representation. */
        bool operator==(const EditorKeymapSettings &other) const {
            return profile.schemaVersion == other.profile.schemaVersion && profile.profileId == other.profile.profileId &&
                   std::ranges::equal(profile.overrides, other.profile.overrides, [](const auto &left, const auto &right) {
                return left.action == right.action && left.bindings == right.bindings;
            });
        }
    };

    /** @brief Source-view preferences; documents and language services retain source ownership. */
    struct CodeEditorSettings final {
        bool showLineNumbers{true};
        bool showWhitespace{false};
        bool wordWrap{true};
        bool showScrollBar{true};
        int autoSaveDelaySeconds{0}; /**< Zero disables source saves; positive values save after an idle delay. */
        bool operator==(const CodeEditorSettings &) const = default;
    };

    /** @brief Typed builtin boolean setting shared by presentation and persistence. */
    struct CodeEditorBooleanSetting final {
        const char *key; /**< Stable persistence and localization identity. */
        bool CodeEditorSettings::*member;
    };

    /** @brief Typed builtin integer setting with shared validation and control bounds. */
    struct CodeEditorIntegerSetting final {
        const char *key;
        int CodeEditorSettings::*member;
        int minimum;
        int maximum;
    };

    /** @brief Returns inert builtin source toggle metadata. @return Process-lifetime descriptor span. */
    [[nodiscard]] std::span<const CodeEditorBooleanSetting> CodeEditorBooleanSettings() noexcept;
    /** @brief Returns inert builtin source numeric metadata. @return Process-lifetime descriptor span. */
    [[nodiscard]] std::span<const CodeEditorIntegerSetting> CodeEditorIntegerSettings() noexcept;

    /** @brief User-level editor settings persisted in the user configuration directory. */
    struct EditorSettings {  // NOSONAR(cpp:S1820)
        EditorStartupBehavior startupBehavior = EditorStartupBehavior::WelcomeScreen;

        int autoSaveIntervalMinutes = 5;
        bool confirmExitWithUnsavedChanges = true;
        bool restoreWorkspaceLayout = true;
        std::string defaultSceneOnProjectOpen = "Assets/Scenes/Main";
        std::string languageTag = "en-US";

        EditorThemePreset themePreset = EditorThemePreset::HoroDark;
        std::string themeId; /**< Stable installed package theme identity; empty selects themePreset. */
        std::string accentColorHex = "#04A5FC";
        int uiScalePercent = 100;
        int codeFontSizePx = 14;
        std::string uiFontFamily = "Inter";           /**< Preferred system sans family; bundled Inter remains fallback. */
        std::string codeFontFamily = "IBM Plex Mono"; /**< Preferred system monospace family; bundled font remains fallback. */

        CodeEditorSettings codeEditor;

        int orbitSensitivity = 100;
        int panSensitivity = 100;
        bool invertOrbitY = false;
        EditorKeymapSettings keymap;

        EditorViewportMode viewportMode = EditorViewportMode::Shaded;
        bool gridOverlay = true;
        EditorRenderingTier renderingTier = EditorRenderingTier::HighEnd;
        std::string textureStreamingBudget = "2048 MB";

        int masterVolume = 80;
        EditorAudioOutputDevice audioOutputDevice = EditorAudioOutputDevice::SystemDefault;
        bool audioEnabled = true;

        Network::NetworkPreviewPreferences networkPreviewPreferences{}; /**< User-only local network preview controls. */
        EditorNavigationOverlayPreferences navigationOverlay{};
        EditorPackageSettings packages{}; /**< User-only package acquisition controls. */

        EditorConsoleLogLevel consoleLogLevel = EditorConsoleLogLevel::Warning;
        bool writeLogToFile = true;
        bool autoCaptureOnStutter = false;
        float stutterThresholdMs = 33.3F;

        bool operator==(const EditorSettings &) const = default;
    };

    /** @brief Load/save document for user-level editor settings. */
    struct EditorSettingsDocument {
        EditorSettings settings{};   /**< Parsed settings with defaults for missing keys. */
        bool loadedFromDisk = false; /**< True when a settings file existed and was read. */
        bool parseError = false;     /**< True when the file was unreadable or malformed. */
        std::string error;           /**< Human-readable load/save diagnostic. */
        std::filesystem::path path;  /**< Absolute settings path used by the store. */
    };

    /** @brief Returns the user home directory used by the settings store. */
    std::filesystem::path ResolveEditorSettingsHomeDirectory();

    /**
     * @brief Returns the editor user settings path.
     *
     * The settings file is stored in `<home>/.horo/editor_settings.json`. This is a
     * user-preference configuration file, separate from project settings such as
     * `<project>/.horo/project.json` and workspace layout state.
     * @return Absolute path to the editor settings file.
     */
    std::filesystem::path ResolveEditorSettingsPath();

    /**
     * @brief Returns default editor settings after applying validation clamps.
     * @return Default user-level editor settings.
     */
    EditorSettings DefaultEditorSettings();

    /**
     * @brief Validates and clamps a settings snapshot in place.
     * @param settings Settings to sanitize.
     * @param outError Optional validation diagnostic. Empty on success.
     * @return True when no values had to be corrected, false when at least one
     *         value was invalid and was clamped or normalized.
     */
    bool ValidateEditorSettings(EditorSettings &settings, std::string *outError = nullptr);

    /**
     * @brief Loads user-level editor settings from disk.
     *
     * Missing files return defaults with `loadedFromDisk=false`. Malformed files
     * return defaults with `parseError=true`; callers may still open Settings so the
     * user can apply a clean file.
     * @return Loaded document, never throws.
     */
    EditorSettingsDocument LoadEditorSettingsDocument();

    /**
     * @brief Saves user-level editor settings to disk.
     *
     * Creates `<home>/.horo` when needed and writes a stable JSON object grouped by
     * settings domain. Secrets are not stored here.
     * @param doc In/out document to save.
     * @param outError Optional save diagnostic.
     * @return True on success, false on validation or I/O failure.
     */
    bool SaveEditorSettingsDocument(EditorSettingsDocument *doc, std::string *outError = nullptr);

}  // namespace Horo::Editor
