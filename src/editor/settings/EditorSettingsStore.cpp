#include "Horo/Editor/EditorSettingsStore.h"

#include "EditorSettingsStoreInternal.h"
#include "Horo/Editor/Localization/LocalizationTypes.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <system_error>

namespace Horo::Editor {
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
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                if (outError) {
                    *outError = "Failed to open editor settings file.";
                }
                return {};
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        [[nodiscard]] std::string UnescapeJsonString(std::string_view value) {
            std::string out;
            out.reserve(value.size());
            bool escaping = false;
            for (const char c : value) {
                if (escaping) {
                    switch (c) {
                        case 'n':
                            out += '\n';
                            break;
                        case 'r':
                            out += '\r';
                            break;
                        case 't':
                            out += '\t';
                            break;
                        default:
                            out += c;
                            break;
                    }
                    escaping = false;
                    continue;
                }
                if (c == '\\') {
                    escaping = true;
                    continue;
                }
                out += c;
            }
            return out;
        }

        [[nodiscard]] std::optional<std::string> FindStringValue(const std::string &json, const char *key) {
            const std::regex re(std::string{R"(")"} + key + R"re("\s*:\s*"((?:\\.|[^"])*)")re");
            std::smatch match;
            if (!std::regex_search(json, match, re) || match.size() < 2) {
                return std::nullopt;
            }
            return UnescapeJsonString(match[1].str());
        }

        [[nodiscard]] std::optional<bool> FindBoolValue(const std::string &json, const char *key) {
            const std::regex re(std::string{"\""} + key + R"("\s*:\s*(true|false))");
            std::smatch match;
            if (!std::regex_search(json, match, re) || match.size() < 2) {
                return std::nullopt;
            }
            return match[1].str() == "true";
        }

        [[nodiscard]] std::optional<int> FindIntValue(const std::string &json, const char *key) {
            const std::regex re(std::string{"\""} + key + R"("\s*:\s*(-?\d+))");
            std::smatch match;
            if (!std::regex_search(json, match, re) || match.size() < 2) {
                return std::nullopt;
            }
            int value = 0;
            const std::string text = match[1].str();
            const auto *begin = text.data();
            if (const auto *end = begin + text.size(); std::from_chars(begin, end, value).ec != std::errc{}) {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] std::optional<float> FindFloatValue(const std::string &json, const char *key) {
            const std::regex re(std::string{"\""} + key + R"("\s*:\s*(-?\d+(?:\.\d+)?))");
            std::smatch match;
            if (!std::regex_search(json, match, re) || match.size() < 2) {
                return std::nullopt;
            }
            try {
                return std::stof(match[1].str());
            } catch (const std::invalid_argument &) {
                return std::nullopt;
            } catch (const std::out_of_range &) {
                return std::nullopt;
            }
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

        void ApplyEditorGroup(const std::string &json, EditorSettings &s) {
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

        void ApplyAppearanceGroup(const std::string &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "themePreset"); v.has_value())
                s.themePreset = ParseThemePreset(*v);
            if (auto v = FindStringValue(json, "accentColorHex"); v.has_value())
                s.accentColorHex = *v;
            if (auto v = FindIntValue(json, "uiScalePercent"); v.has_value())
                s.uiScalePercent = *v;
            if (auto v = FindIntValue(json, "codeFontSizePx"); v.has_value())
                s.codeFontSizePx = *v;
            if (auto v = FindStringValue(json, "uiFontFamily"); v.has_value())
                s.uiFontFamily = *v;
            if (auto v = FindStringValue(json, "codeFontFamily"); v.has_value())
                s.codeFontFamily = *v;
        }

        void ApplyInputGroup(const std::string &json, EditorSettings &s) {
            if (auto v = FindIntValue(json, "orbitSensitivity"); v.has_value())
                s.orbitSensitivity = *v;
            if (auto v = FindIntValue(json, "panSensitivity"); v.has_value())
                s.panSensitivity = *v;
            if (auto v = FindBoolValue(json, "invertOrbitY"); v.has_value())
                s.invertOrbitY = *v;
        }

        void ApplyRenderingGroup(const std::string &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "viewportMode"); v.has_value())
                s.viewportMode = ParseViewportMode(*v);
            if (auto v = FindBoolValue(json, "gridOverlay"); v.has_value())
                s.gridOverlay = *v;
            if (auto v = FindStringValue(json, "renderingTier"); v.has_value())
                s.renderingTier = ParseRenderingTier(*v);
            if (auto v = FindStringValue(json, "textureStreamingBudget"); v.has_value())
                s.textureStreamingBudget = *v;
        }

        void ApplyAudioNetworkGroups(const std::string &json, EditorSettings &s) {
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

        void ApplyPackageGroup(const std::string &json, EditorSettings &s) {
            if (auto v = FindIntValue(json, "downloadThreads"); v.has_value()) {
                s.packages.downloadThreads = *v;
                return;
            }
            // Preserve the pre-1172 key during load; WriteSettings emits the canonical packages group.
            if (auto v = FindIntValue(json, "packageDownloadThreads"); v.has_value())
                s.packages.downloadThreads = *v;
        }

        void ApplyDiagnosticsPluginsGroups(const std::string &json, EditorSettings &s) {
            if (auto v = FindStringValue(json, "consoleLogLevel"); v.has_value())
                s.consoleLogLevel = ParseLogLevel(*v);
            if (auto v = FindBoolValue(json, "writeLogToFile"); v.has_value())
                s.writeLogToFile = *v;
            if (auto v = FindBoolValue(json, "autoCaptureOnStutter"); v.has_value())
                s.autoCaptureOnStutter = *v;
            if (auto v = FindFloatValue(json, "stutterThresholdMs"); v.has_value())
                s.stutterThresholdMs = *v;
        }

        void ApplyJsonValues(const std::string &json, EditorSettings &s) {
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
            LOG_DEBUG("editor.settings", "editor_settings.json not found at '%s' — using defaults.", out.path.string().c_str());
            return out;
        }

        out.loadedFromDisk = true;
        std::string readError;
        const std::string json = ReadWholeFile(out.path, &readError);
        if (!readError.empty()) {
            LOG_ERROR("editor.settings", "Failed to read editor_settings.json at '%s': %s", out.path.string().c_str(), readError.c_str());
            out.parseError = true;
            out.error = readError;
            return out;
        }

        if (json.find('{') == std::string::npos || json.find('}') == std::string::npos) {
            LOG_ERROR("editor.settings", "editor_settings.json at '%s' is not a valid JSON object.", out.path.string().c_str());
            out.parseError = true;
            out.error = "Editor settings file must contain a JSON object.";
            return out;
        }

        ApplyJsonValues(json, out.settings);
        if (std::string validationError; !ValidateEditorSettings(out.settings, &validationError)) {
            LOG_WARN("editor.settings", "editor_settings.json loaded but failed validation: %s", validationError.c_str());
            out.error = validationError;
        } else {
            LOG_DEBUG("editor.settings", "editor_settings.json loaded successfully from '%s'.", out.path.string().c_str());
        }
        return out;
    }

    /** @copydoc SaveEditorSettingsDocument */
    bool SaveEditorSettingsDocument(EditorSettingsDocument *doc, std::string *outError) {
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

        std::ofstream out(doc->path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (outError) {
                *outError = "Failed to open editor settings file for writing.";
            }
            return false;
        }

        const auto &s = doc->settings;
        SettingsStoreInternal::WriteSettings(out, s);

        if (!out.good()) {
            LOG_ERROR("editor.settings", "I/O error while writing editor_settings.json to '%s'.", doc->path.string().c_str());
            if (outError) {
                *outError = "Failed while writing editor settings file.";
            }
            return false;
        }

        doc->loadedFromDisk = true;
        doc->parseError = false;
        doc->error.clear();
        LOG_DEBUG("editor.settings", "editor_settings.json saved to '%s'.", doc->path.string().c_str());
        return true;
    }
}  // namespace Horo::Editor
