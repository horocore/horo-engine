#pragma once

#include "Horo/Editor/EditorSettingsStore.h"

#include <array>
#include <cstdint>
#include <string>

namespace Horo::Extensions {
    class ExtensionInventory;
    class ExtensionMarketplaceService;
}  // namespace Horo::Extensions

namespace Horo::Editor {
    class EditorSettingsService;

    struct SettingsGeneralTab {
        int startupAction = 0;
        int autoSaveInterval = 5;
        bool confirmExit = true;
        bool restoreWorkspace = true;
        std::string defaultScene = "Assets/Scenes/Main";
        std::string languageTag = "en-US";
    };

    struct SettingsAppearanceTab {
        int themeIndex = 0;
        std::string customThemePath = "~/.horo/themes/my-theme.json";
        int uiScale = 100;
        std::string editorFontSize = "14";
        std::string accentHex = "#04A5FC";
        int pendingThemeIndex = -1;
    };

    struct SettingsInputTab {
        int orbitSensitivity = 100;
        int panSensitivity = 100;
        bool invertOrbitY = false;
    };

    struct SettingsRenderingTab {
        int viewportMode = 0;
        bool gridOverlay = true;
        int renderingTier = 0;
        std::string textureBudget = "2048 MB";
        int renderBackend = 0;
    };

    struct SettingsAudioTab {
        int masterVolume = 80;
        int audioOutputDevice = 0;
        bool audioEnabled = true;
    };

    struct SettingsNetworkTab {
        int maxPreviewClients = 4;
        int simulatedLatencyMs = 0;
    };

    struct SettingsPackagesTab {
        int downloadThreads = 8;
    };

    struct SettingsDiagnosticsTab {
        int consoleLogLevel = 2;
        bool writeLogToFile = true;
        bool autoCaptureStutter = false;
        float stutterThresholdMs = 33.3F;
        bool showFps = false;
        bool anonymousTelemetry = false;
    };

    struct SettingsPluginToggles {
        bool horoMcpBridge = true;
        bool fmodIntegration = true;
        bool steamworksSdk = false;
    };

    struct SettingsMcpSettings {
        int transportMode = 0;
        int port = 8080;
        bool requireToken = true;
        bool allowRemote = false;
        int toolScope = 0;
        std::string assetRoot = "Assets/Generated";
    };

    struct SettingsFmodSettings {
        std::string studioPath = "/Applications/FMOD Studio.app";
        std::string projectFile = "Audio/HoroAudio.fspro";
        std::string bankPath = "Assets/Audio/Banks";
        bool liveUpdate = true;
        bool failOnMissing = true;
        int targetPlatform = 0;
    };

    struct SettingsSteamSettings {
        std::string sdkPath = "ThirdParty/Steamworks";
        int initMode = 0;
        bool overlay = true;
        bool achievements = true;
        bool networking = false;
    };

    struct SettingsPluginRuntime {
        std::string discoveryPaths = "{project}/plugins; ~/.horo/plugins";
        int loadOrder = 0;
        std::string devPath = "~/dev/horo-plugins";
        bool sandbox = true;
        int unsignedPolicy = 0;
        int networkPolicy = 0;
        int updateCheck = 0;
        int compatMode = 0;
    };

    /** @brief Modal-owned mutable draft for the editor settings workflow. */
    struct SettingsState {  // NOSONAR(cpp:S1820)
        using GeneralTab = SettingsGeneralTab;

        using AppearanceTab = SettingsAppearanceTab;
        using InputTab = SettingsInputTab;
        using RenderingTab = SettingsRenderingTab;
        using AudioTab = SettingsAudioTab;
        using NetworkTab = SettingsNetworkTab;
        using PackagesTab = SettingsPackagesTab;
        using DiagnosticsTab = SettingsDiagnosticsTab;
        using PluginToggles = SettingsPluginToggles;
        using McpSettings = SettingsMcpSettings;
        using FmodSettings = SettingsFmodSettings;
        using SteamSettings = SettingsSteamSettings;
        using PluginRuntime = SettingsPluginRuntime;

        bool initialized = false;
        bool wasOpen = false;
        bool dirty = false;
        EditorSettings committed{};
        std::string statusMessage;
        bool statusIsError = false;
        int activeTab = 0;
        std::uint64_t settingsRevision = 0;

        GeneralTab general{};
        AppearanceTab appearance{};
        InputTab input{};
        RenderingTab rendering{};
        AudioTab audio{};
        NetworkTab network{};
        PackagesTab packages{};
        DiagnosticsTab diagnostics{};

        int pluginSectionTab = 0;
        Extensions::ExtensionInventory *extensionInventory = nullptr;
        Extensions::ExtensionMarketplaceService *extensionMarketplace = nullptr;
        std::string selectedExtensionId;
        std::string marketplaceQuery;
        int selectedPlugin = 0;
        std::array<int, 3> pluginDetailTab{};
        std::string pluginFilter;
        std::string modalFeedback;

        PluginToggles plugins{};
        McpSettings mcp{};
        FmodSettings fmod{};
        SteamSettings steam{};
        PluginRuntime runtime{};
    };

    [[nodiscard]] EditorSettings CollectDraftSettings(const SettingsState &state);
    void ApplySettingsToDraft(SettingsState &state, const EditorSettings &settings);
    void LoadSettingsForModal(SettingsState &state, const EditorSettingsService &settings);
    [[nodiscard]] bool ApplySettings(SettingsState &state, EditorSettingsService &settings);
}  // namespace Horo::Editor
