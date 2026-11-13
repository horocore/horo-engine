#include "Horo/Editor/EditorSettingsStore.h"

#include "EditorSettingsStoreInternal.h"
#include "Horo/Editor/Localization/LocalizationTypes.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Platform/ConfigurationFileStore.h"
#include "JsonUtils.h"
#include "editor/input/EditorInputActions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

namespace Horo::Editor {
    /** @copydoc CodeEditorBooleanSettings */
    std::span<const CodeEditorBooleanSetting> CodeEditorBooleanSettings() noexcept {
        static constexpr CodeEditorBooleanSetting settings[]{
            {"codeShowLineNumbers", &CodeEditorSettings::showLineNumbers},
            {"codeShowWhitespace", &CodeEditorSettings::showWhitespace},
            {"codeWordWrap", &CodeEditorSettings::wordWrap},
            {"codeShowScrollBar", &CodeEditorSettings::showScrollBar},
        };
        return settings;
    }

    /** @copydoc CodeEditorIntegerSettings */
    std::span<const CodeEditorIntegerSetting> CodeEditorIntegerSettings() noexcept {
        static constexpr CodeEditorIntegerSetting settings[]{
            {"codeAutoSaveDelaySeconds", &CodeEditorSettings::autoSaveDelaySeconds, 0, 300},
        };
        return settings;
    }

    namespace {
        [[nodiscard]] std::string GetEnvVar(const char *name) {
            if (!name || !*name) {
                return {};
            }
#if defined(_WIN32)
            size_t len = 0;
            if (getenv_s(&len, nullptr, 0, name) != 0 || len <= 1) {
                return {};
            }
            std::string value(len, '\0');
            if (getenv_s(&len, value.data(), value.size(), name) != 0 || len <= 1) {
                return {};
            }
            value.resize(len - 1);
            return value;
#else
            const char *value = std::getenv(name);
            return value ? std::string(value) : std::string();
#endif
        }

        [[nodiscard]] std::string ReadWholeFile(const std::filesystem::path &path, std::string *outError) {
            NativeDurableFileSystem files;
            ConfigurationFileStore store{files};
            ConfigurationLimits limits;
            limits.maximumDocumentBytes = 2 * 1024 * 1024;
            auto read = store.Read(path, limits);
            if (read.HasValue())
                return std::move(read).Value();
            if (outError)
                *outError = read.ErrorValue().message;
            return {};
        }

        /** @brief Imports the retained legacy profile only while the canonical Settings field is absent. */
        void ImportLegacyKeymap(EditorSettingsDocument &document) {
            const auto path = ResolveEditorSettingsHomeDirectory() / ".horo" / "input" / "editor.json";
            if (std::error_code error; std::filesystem::exists(path, error) && !error) {
                const auto profile = Input::LoadBindingProfile(path);
                if (profile.HasValue())
                    document.settings.keymap.profile = profile.Value();
                else
                    document.error = profile.ErrorValue().message;
            }
        }

        /** @brief Reads the canonical typed profile without normalizing duplicate or malformed JSON into valid settings. */
        bool ReadKeymap(const std::string &text, EditorSettingsDocument &document, nlohmann::json &parsed) {
            if (text.size() > 2 * 1024 * 1024) {
                document.error = "Editor settings exceed the supported size.";
                return false;
            }
            Foundation::JsonParseGuard guard{ConfigurationLimits{}.maximumNestingDepth};
            if (!guard.Validate(text.begin(), text.end())) {
                document.error = "Editor settings contain malformed JSON, duplicate keys, or excessive nesting.";
                return false;
            }
            parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                document.error = "Editor settings contain malformed JSON or duplicate keys.";
                return false;
            }
            if (const auto version = parsed.find("schemaVersion");
                version != parsed.end() && (!version->is_number_integer() || version->get<std::int64_t>() != 1)) {
                document.error = "Editor settings schema version is unsupported.";
                return false;
            }
            if (const auto configuration = parsed.find("configuration"); configuration != parsed.end()) {
                ConfigurationSchema schema;
                static_cast<void>(schema.Seal());
                const auto json = configuration->dump();
                const auto validated = ConfigurationResolver::ParseDocument(schema, json, "editor-user");
                if (validated.HasError()) {
                    document.error = "Editor user configuration is invalid.";
                    return false;
                }
                document.configurationJson = json;
            }
            const auto input = parsed.find("input");
            if (input == parsed.end() || (input->is_object() && !input->contains("bindingProfile"))) {
                ImportLegacyKeymap(document);
                return true;
            }
            if (!input->is_object() || !(*input)["bindingProfile"].is_object()) {
                document.error = "Editor keymap must contain a binding profile object.";
                return false;
            }
            const auto profile = Input::ParseBindingProfile((*input)["bindingProfile"].dump());
            if (profile.HasError()) {
                document.error = profile.ErrorValue().message;
                return false;
            }
            document.settings.keymap.profile = profile.Value();
            return true;
        }

        /** @brief Typed view of the bounded legacy document, excluding the separate shared configuration subtree. */
        struct SettingsJsonInput {
            const nlohmann::json &root;
            bool invalidValue = false;
        };

        /** @brief Preserves flat legacy fields while rejecting ambiguous duplicate representations. */
        const nlohmann::json *FindSettingMember(SettingsJsonInput &input, const char *key) {
            const auto direct = input.root.find(key);
            const nlohmann::json *found = direct == input.root.end() ? nullptr : &*direct;
            constexpr std::array groups{"editor", "appearance", "codeEditor", "input",      "rendering",
                                        "audio",  "network",    "packages",   "diagnostics"};
            for (const char *group : groups) {
                const auto object = input.root.find(group);
                if (object == input.root.end())
                    continue;
                if (!object->is_object()) {
                    input.invalidValue = true;
                    continue;
                }
                const auto value = object->find(key);
                if (value == object->end())
                    continue;
                if (found != nullptr) {
                    input.invalidValue = true;
                    return nullptr;
                }
                found = &*value;
            }
            return found;
        }

        /** @brief Reads a bounded decoded string with no regular-expression recursion. */
        std::optional<std::string> FindStringValue(SettingsJsonInput &input, const char *key) {
            const auto *value = FindSettingMember(input, key);
            if (value == nullptr)
                return std::nullopt;
            if (value->is_string()) {
                const auto &text = value->get_ref<const std::string &>();
                if (text.size() <= ConfigurationLimits{}.maximumStringValueBytes && text.find('\0') == std::string::npos)
                    return text;
            }
            input.invalidValue = true;
            return std::nullopt;
        }

        /** @brief Reads only JSON booleans and retains a typed-shape rejection. */
        std::optional<bool> FindBoolValue(SettingsJsonInput &input, const char *key) {
            const auto *value = FindSettingMember(input, key);
            if (value == nullptr)
                return std::nullopt;
            if (value->is_boolean())
                return value->get<bool>();
            input.invalidValue = true;
            return std::nullopt;
        }

        /** @brief Reads integers without signed/unsigned wrap or floating-point truncation. */
        std::optional<int> FindIntValue(SettingsJsonInput &input, const char *key) {
            const auto *value = FindSettingMember(input, key);
            if (value == nullptr)
                return std::nullopt;
            if (value->is_number_unsigned()) {
                const auto number = value->get<std::uint64_t>();
                if (number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                    return static_cast<int>(number);
            } else if (value->is_number_integer()) {
                const auto number = value->get<std::int64_t>();
                if (number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max())
                    return static_cast<int>(number);
            }
            input.invalidValue = true;
            return std::nullopt;
        }

        /** @brief Reads finite representable numeric preferences independently of process locale. */
        std::optional<float> FindFloatValue(SettingsJsonInput &input, const char *key) {
            const auto *value = FindSettingMember(input, key);
            if (value == nullptr)
                return std::nullopt;
            if (value->is_number()) {
                const auto number = value->get<double>();
                if (std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max())
                    return static_cast<float>(number);
            }
            input.invalidValue = true;
            return std::nullopt;
        }

        [[nodiscard]] bool IsHexColor(std::string_view value) {
            if (value.size() != 7 || value[0] != '#') {
                return false;
            }
            return std::all_of(value.begin() + 1, value.end(), [](const char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            });
        }

        [[nodiscard]] EditorStartupBehavior ParseStartup(const std::string_view value) {
            using enum EditorStartupBehavior;
            if (value == "last_project")
                return LastProject;
            if (value == "project_browser")
                return ProjectBrowser;
            return WelcomeScreen;
        }

        [[nodiscard]] EditorThemePreset ParseThemePreset(std::string_view value) {
            using enum EditorThemePreset;
            if (value == "midnight")
                return Midnight;
            if (value == "light")
                return Light;
            return HoroDark;
        }

        [[nodiscard]] EditorViewportMode ParseViewportMode(std::string_view value) {
            using enum EditorViewportMode;
            if (value == "wireframe")
                return Wireframe;
            if (value == "lit")
                return Lit;
            if (value == "unlit")
                return Unlit;
            return Shaded;
        }

        [[nodiscard]] EditorRenderingTier ParseRenderingTier(std::string_view value) {
            using enum EditorRenderingTier;
            if (value == "dx12_vulkan")
                return Dx12Vulkan;
            if (value == "dx11")
                return Dx11;
            if (value == "es3")
                return Es3;
            return HighEnd;
        }

        [[nodiscard]] EditorAudioOutputDevice ParseAudioDevice(std::string_view value) {
            using enum EditorAudioOutputDevice;
            if (value == "headphones")
                return Headphones;
            if (value == "speakers")
                return Speakers;
            return SystemDefault;
        }

        [[nodiscard]] EditorConsoleLogLevel ParseLogLevel(std::string_view value) {
            using enum EditorConsoleLogLevel;
            if (value == "debug")
                return Debug;
            if (value == "info")
                return Info;
            if (value == "error")
                return Error;
            return Warning;
        }

        void ApplyEditorGroup(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "startupBehavior"); v.has_value())
                s.startupBehavior = ParseStartup(*v);
            if (auto v = FindIntValue(json, "autoSaveIntervalMinutes"); v.has_value())
                s.autoSaveIntervalMinutes = *v;
            if (auto v = FindBoolValue(json, "confirmExitWithUnsavedChanges"); v.has_value())
                s.confirmExitWithUnsavedChanges = *v;
            if (auto v = FindBoolValue(json, "restoreWorkspaceLayout"); v.has_value())
                s.restoreWorkspaceLayout = *v;
            if (auto v = FindStringValue(json, "defaultSceneOnProjectOpen"); v.has_value())
                s.defaultSceneOnProjectOpen = *v;
            if (auto v = FindStringValue(json, "languageTag"); v.has_value())
                s.languageTag = *v;
        }

        void ApplyAppearanceGroup(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "themePreset"); v.has_value())
                s.themePreset = ParseThemePreset(*v);
            if (auto v = FindStringValue(json, "themeId"); v.has_value())
                s.themeId = *v;
            if (auto v = FindStringValue(json, "accentColorHex"); v.has_value())
                s.accentColorHex = *v;
            if (auto v = FindIntValue(json, "uiScalePercent"); v.has_value())
                s.uiScalePercent = *v;
            for (const auto &entry : CodeEditorBooleanSettings())
                if (const auto value = FindBoolValue(json, entry.key); value.has_value())
                    s.codeEditor.*entry.member = *value;
            for (const auto &entry : CodeEditorIntegerSettings())
                if (const auto value = FindIntValue(json, entry.key); value.has_value())
                    s.codeEditor.*entry.member = *value;
            if (auto v = FindIntValue(json, "codeFontSizePx"); v.has_value())
                s.codeFontSizePx = *v;
            if (auto v = FindStringValue(json, "uiFontFamily"); v.has_value())
                s.uiFontFamily = *v;
            if (auto v = FindStringValue(json, "codeFontFamily"); v.has_value())
                s.codeFontFamily = *v;
        }

        void ApplyInputGroup(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindIntValue(json, "orbitSensitivity"); v.has_value())
                s.orbitSensitivity = *v;
            if (auto v = FindIntValue(json, "panSensitivity"); v.has_value())
                s.panSensitivity = *v;
            if (auto v = FindBoolValue(json, "invertOrbitY"); v.has_value())
                s.invertOrbitY = *v;
        }

        void ApplyRenderingGroup(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "viewportMode"); v.has_value())
                s.viewportMode = ParseViewportMode(*v);
            if (auto v = FindBoolValue(json, "navigationShowAreas"); v.has_value())
                s.navigationOverlay.showAreas = *v;
            if (auto v = FindBoolValue(json, "navigationShowPaths"); v.has_value())
                s.navigationOverlay.showPaths = *v;
            if (auto v = FindBoolValue(json, "navigationShowAgents"); v.has_value())
                s.navigationOverlay.showAgents = *v;
            if (auto v = FindBoolValue(json, "gridOverlay"); v.has_value())
                s.gridOverlay = *v;
            if (auto v = FindStringValue(json, "renderingTier"); v.has_value())
                s.renderingTier = ParseRenderingTier(*v);
            if (auto v = FindStringValue(json, "textureStreamingBudget"); v.has_value())
                s.textureStreamingBudget = *v;
        }

        void ApplyAudioNetworkGroups(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindIntValue(json, "masterVolume"); v.has_value())
                s.masterVolume = *v;
            if (auto v = FindStringValue(json, "audioOutputDevice"); v.has_value())
                s.audioOutputDevice = ParseAudioDevice(*v);
            if (auto v = FindBoolValue(json, "audioEnabled"); v.has_value())
                s.audioEnabled = *v;
            if (auto v = FindIntValue(json, "maxPreviewClients"); v.has_value())
                s.networkPreviewPreferences.maxPreviewClients = static_cast<std::uint32_t>(std::max(*v, 0));
            if (auto v = FindIntValue(json, "simulatedLatencyMs"); v.has_value())
                s.networkPreviewPreferences.simulatedLatencyMilliseconds = static_cast<std::uint32_t>(std::max(*v, 0));
        }

        void ApplyPackageGroup(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindIntValue(json, "downloadThreads"); v.has_value()) {
                s.packages.downloadThreads = *v;
                return;
            }
            // Preserve the pre-1172 key during load; WriteSettings emits the canonical packages group.
            if (auto v = FindIntValue(json, "packageDownloadThreads"); v.has_value())
                s.packages.downloadThreads = *v;
        }

        void ApplyDiagnosticsPluginsGroups(SettingsJsonInput &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "consoleLogLevel"); v.has_value())
                s.consoleLogLevel = ParseLogLevel(*v);
            if (auto v = FindBoolValue(json, "writeLogToFile"); v.has_value())
                s.writeLogToFile = *v;
            if (auto v = FindBoolValue(json, "autoCaptureOnStutter"); v.has_value())
                s.autoCaptureOnStutter = *v;
            if (auto v = FindFloatValue(json, "stutterThresholdMs"); v.has_value())
                s.stutterThresholdMs = *v;
        }

        void ApplyJsonValues(SettingsJsonInput &json, EditorSettings &s) {
            ApplyEditorGroup(json, s);
            ApplyAppearanceGroup(json, s);
            ApplyInputGroup(json, s);
            ApplyRenderingGroup(json, s);
            ApplyAudioNetworkGroups(json, s);
            ApplyPackageGroup(json, s);
            ApplyDiagnosticsPluginsGroups(json, s);
        }

    }  // namespace

    /** @copydoc ResolveEditorSettingsHomeDirectory */
    std::filesystem::path ResolveEditorSettingsHomeDirectory() {
#if defined(_WIN32)
        if (const std::string home = GetEnvVar("USERPROFILE"); !home.empty()) {
            return std::filesystem::path(home);
        }
        const std::string drive = GetEnvVar("HOMEDRIVE");
        const std::string homePath = GetEnvVar("HOMEPATH");
        if (!drive.empty() && !homePath.empty()) {
            return std::filesystem::path(drive + homePath);
        }
#else
        if (const std::string home = GetEnvVar("HOME"); !home.empty()) {
            return std::filesystem::path(home);
        }
#endif
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }

    /** @copydoc ResolveEditorSettingsPath */
    std::filesystem::path ResolveEditorSettingsPath() {
        return ResolveEditorSettingsHomeDirectory() / ".horo" / "editor_settings.json";
    }

    /** @copydoc DefaultEditorSettings */
    EditorSettings DefaultEditorSettings() {
        EditorSettings out{};
        (void)ValidateEditorSettings(out, nullptr);
        return out;
    }

    namespace {
        template <typename MarkInvalid> void ValidateCoreEditorSettings(EditorSettings &settings, MarkInvalid &markInvalid) {
            const auto clampInt = [&markInvalid](int &value, const int minValue, const int maxValue, const char *message) {
                if (value < minValue || value > maxValue) {
                    markInvalid(message);
                    value = std::clamp(value, minValue, maxValue);
                }
            };
            clampInt(settings.autoSaveIntervalMinutes, 0, 30, "Auto-save interval must be between 0 and 30 minutes.");
            if (!LocaleTag::Parse(settings.languageTag).has_value()) {
                markInvalid("Language must be a valid BCP 47 locale tag.");
                settings.languageTag = "en-US";
            }
            clampInt(settings.uiScalePercent, 75, 200, "UI scale must be between 75 and 200 percent.");
            for (const auto &entry : CodeEditorIntegerSettings())
                clampInt(settings.codeEditor.*entry.member, entry.minimum, entry.maximum, "Code editor preference is outside its bounds.");
            if (settings.codeFontFamily.empty() || settings.codeFontFamily.size() > 128) {
                markInvalid("Code font family must contain between 1 and 128 bytes.");
                settings.codeFontFamily = "IBM Plex Mono";
            }
            clampInt(settings.codeFontSizePx, 14, 24, "Code font size must be between 14 and 24 px.");
            clampInt(settings.orbitSensitivity, 10, 300, "Orbit sensitivity must be between 10 and 300.");
            clampInt(settings.panSensitivity, 10, 300, "Pan sensitivity must be between 10 and 300.");
            clampInt(settings.masterVolume, 0, 100, "Master volume must be between 0 and 100.");
        }

        template <typename MarkInvalid> void ValidateNetworkAndPackages(EditorSettings &settings, MarkInvalid &markInvalid) {
            const auto clampUnsigned = [&markInvalid](std::uint32_t &value, const std::uint32_t minValue, const std::uint32_t maxValue,
                                                      const char *message) {
                if (value < minValue || value > maxValue) {
                    markInvalid(message);
                    value = std::clamp(value, minValue, maxValue);
                }
            };
            clampUnsigned(settings.networkPreviewPreferences.maxPreviewClients, Network::NetworkPreviewPreferences::MinimumPreviewClients,
                          Network::NetworkPreviewPreferences::MaximumPreviewClients, "Max preview clients must be between 1 and 16.");
            clampUnsigned(settings.networkPreviewPreferences.simulatedLatencyMilliseconds, 0,
                          Network::NetworkPreviewPreferences::MaximumSimulatedLatencyMilliseconds,
                          "Simulated latency must be between 0 and 500 ms.");
            if (settings.packages.downloadThreads < 1 || settings.packages.downloadThreads > 32) {
                markInvalid("Package download threads must be between 1 and 32.");
                settings.packages.downloadThreads = std::clamp(settings.packages.downloadThreads, 1, 32);
            }
        }

        template <typename MarkInvalid> void ValidateStutterThreshold(EditorSettings &settings, MarkInvalid &markInvalid) {
            if (!std::isfinite(settings.stutterThresholdMs) || settings.stutterThresholdMs < 1.0F ||
                settings.stutterThresholdMs > 1000.0F) {
                markInvalid("Stutter threshold must be between 1 and 1000 ms.");
                settings.stutterThresholdMs = std::isfinite(settings.stutterThresholdMs)
                                                  ? std::clamp(settings.stutterThresholdMs, 1.0F, 1000.0F)
                                                  : EditorSettings{}.stutterThresholdMs;
            }
        }

        template <typename MarkInvalid> void ValidateAppearanceAndPaths(EditorSettings &settings, MarkInvalid &markInvalid) {
            if (!IsHexColor(settings.accentColorHex)) {
                markInvalid("Accent color must be a #RRGGBB hex color.");
                settings.accentColorHex = "#04A5FC";
            }
            if (settings.defaultSceneOnProjectOpen.empty()) {
                markInvalid("Default scene cannot be empty.");
                settings.defaultSceneOnProjectOpen = "Assets/Scenes/Main";
            }
            if (settings.textureStreamingBudget.empty()) {
                markInvalid("Texture streaming budget cannot be empty.");
                settings.textureStreamingBudget = "2048 MB";
            }
        }
    }  // namespace

    /** @copydoc ValidateEditorSettings */
    bool ValidateEditorSettings(EditorSettings &settings, std::string *outError) {
        if (outError) {
            outError->clear();
        }
        bool valid = true;
        auto markInvalid = [&valid, outError](const char *message) {
            valid = false;
            if (outError && outError->empty()) {
                *outError = message;
            }
        };

        const EditorSettings defaults;
        constexpr std::array textFields{&EditorSettings::defaultSceneOnProjectOpen,
                                        &EditorSettings::languageTag,
                                        &EditorSettings::themeId,
                                        &EditorSettings::accentColorHex,
                                        &EditorSettings::uiFontFamily,
                                        &EditorSettings::codeFontFamily,
                                        &EditorSettings::textureStreamingBudget};
        for (const auto field : textFields) {
            auto &text = settings.*field;
            if (text.size() > ConfigurationLimits{}.maximumStringValueBytes || text.find('\0') != std::string::npos ||
                !IsValidUtf8ScalarSequence(text)) {
                markInvalid("Editor settings text must be bounded UTF-8 without NUL characters.");
                text = defaults.*field;
            }
        }

        const auto bindings = Input::ValidateBindingProfile(EditorKeymapActions(settings.keymap.profile), settings.keymap.profile);
        if (!bindings.IsValid()) {
            markInvalid("Editor keymap contains an invalid or conflicting shortcut.");
            settings.keymap = {};
        }
        if (settings.themeId.size() > 256 ||
            settings.themeId.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos) {
            markInvalid("Package theme identity is invalid.");
            settings.themeId.clear();
        }
        ValidateCoreEditorSettings(settings, markInvalid);
        ValidateNetworkAndPackages(settings, markInvalid);
        ValidateStutterThreshold(settings, markInvalid);
        ValidateAppearanceAndPaths(settings, markInvalid);
        return valid;
    }

    /** @copydoc LoadEditorSettingsDocument */
    EditorSettingsDocument LoadEditorSettingsDocument() {
        EditorSettingsDocument out;
        out.path = ResolveEditorSettingsPath();
        out.settings = DefaultEditorSettings();

        if (std::error_code ec; !std::filesystem::exists(out.path, ec)) {
            LOG_DEBUG("editor.settings", "editor_settings.json not found — using defaults.");
            ImportLegacyKeymap(out);
            std::string validationError;
            if (!ValidateEditorSettings(out.settings, &validationError))
                out.error = std::move(validationError);
            return out;
        }

        out.loadedFromDisk = true;
        std::string readError;
        const std::string json = ReadWholeFile(out.path, &readError);
        if (!readError.empty()) {
            LOG_ERROR("editor.settings", "Failed to read editor_settings.json: %s", readError.c_str());
            out.parseError = true;
            out.error = readError;
            return out;
        }

        nlohmann::json parsed;
        if (!ReadKeymap(json, out, parsed)) {
            out.parseError = true;
            return out;
        }
        SettingsJsonInput input{parsed};
        ApplyJsonValues(input, out.settings);
        if (input.invalidValue) {
            out.parseError = true;
            out.error = "Editor settings contain an invalid type, encoding, size, or ambiguous field.";
            out.settings = DefaultEditorSettings();
            return out;
        }
        if (std::string validationError; !ValidateEditorSettings(out.settings, &validationError)) {
            LOG_WARN("editor.settings", "editor_settings.json loaded but failed validation: %s", validationError.c_str());
            out.error = validationError;
        } else {
            LOG_DEBUG("editor.settings", "editor_settings.json loaded successfully.");
        }
        return out;
    }

    /** @copydoc SaveEditorSettingsDocument */
    bool SaveEditorSettingsDocument(EditorSettingsDocument *doc, std::string *outError, const ConfigurationSnapshot *configuration) {
        if (outError) {
            outError->clear();
        }
        if (!doc) {
            if (outError) {
                *outError = "Editor settings document is null.";
            }
            return false;
        }

        doc->path = ResolveEditorSettingsPath();
        if (std::string validationError; !ValidateEditorSettings(doc->settings, &validationError)) {
            if (outError) {
                *outError = validationError;
            }
            return false;
        }

        const std::filesystem::path dir = doc->path.parent_path();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (outError) {
                *outError = ec.message();
            }
            return false;
        }

        if (configuration)
            doc->configurationJson = configuration->SourceToJson(ConfigurationSource::User);
        else if (!doc->configurationJson.empty()) {
            if (outError)
                *outError = "A validated configuration snapshot is required to save shared settings.";
            return false;
        }
        std::ostringstream out;
        SettingsStoreInternal::WriteSettings(out, doc->settings, doc->configurationJson);
        const auto text = out.str();
        if (text.size() > 2 * 1024 * 1024) {
            if (outError)
                *outError = "Editor settings exceed the supported size.";
            return false;
        }
        NativeDurableFileSystem files;
        auto lockPath = doc->path;
        lockPath += ".lock";
        auto lock = files.TryAcquireExclusive(lockPath, "horo.editor.settings");
        if (lock.HasError()) {
            if (outError)
                *outError = lock.ErrorValue().message;
            return false;
        }
        auto prepared = doc->path;
        prepared += ".tmp";
        const auto removed = files.RemoveDurable(prepared);
        if (removed.HasError()) {
            if (outError)
                *outError = removed.ErrorValue().message;
            return false;
        }
        bool created = false;
        const auto written = files.WritePrivateDurable(prepared, std::as_bytes(std::span{text.data(), text.size()}), created);
        if (written.HasError()) {
            if (created)
                static_cast<void>(files.RemoveDurable(prepared));
            if (outError)
                *outError = written.ErrorValue().message;
            return false;
        }
        AtomicFileReplacementReceipt receipt;
        const auto published = files.AtomicReplaceTracked(prepared, doc->path, receipt);
        if (published.HasError() && !receipt.WasCommitted()) {
            static_cast<void>(files.RemoveDurable(prepared));
            if (outError)
                *outError = published.ErrorValue().message;
            return false;
        }
        if (published.HasError())
            LOG_WARN("editor.settings", "Settings replacement committed with a durability diagnostic: code=%s.",
                     published.ErrorValue().code.Value().c_str());

        doc->loadedFromDisk = true;
        doc->parseError = false;
        doc->error = published.HasError() ? published.ErrorValue().message : std::string{};
        LOG_DEBUG("editor.settings", "editor_settings.json published atomically.");
        return true;
    }
}  // namespace Horo::Editor
