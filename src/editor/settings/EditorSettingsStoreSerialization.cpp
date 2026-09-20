#include "EditorSettingsStoreInternal.h"
#include "Horo/Network/NetworkProjectSettings.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <ostream>
#include <string>
#include <string_view>

namespace Horo::Editor::SettingsStoreInternal {
    namespace {
        [[nodiscard]] std::string EscapeJsonString(const std::string_view value) {
            std::string out;
            out.reserve(value.size() + 8);
            for (const char c : value) {
                switch (c) {
                    case '\\':
                        out += R"(\\)";
                        break;
                    case '"':
                        out += R"(\")";
                        break;
                    case '\n':
                        out += "\\n";
                        break;
                    case '\r':
                        out += "\\r";
                        break;
                    case '\t':
                        out += "\\t";
                        break;
                    default:
                        out += c;
                        break;
                }
            }
            return out;
        }

        [[nodiscard]] const char *ToString(const EditorStartupBehavior value) {
            using enum EditorStartupBehavior;
            switch (value) {
                case LastProject:
                    return "last_project";
                case ProjectBrowser:
                    return "project_browser";
                case WelcomeScreen:
                default:
                    return "welcome_screen";
            }
        }

        [[nodiscard]] const char *ToString(const EditorThemePreset value) {
            using enum EditorThemePreset;
            switch (value) {
                case Midnight:
                    return "midnight";
                case Light:
                    return "light";
                case HoroDark:
                default:
                    return "horo_dark";
            }
        }

        [[nodiscard]] const char *ToString(const EditorViewportMode value) {
            using enum EditorViewportMode;
            switch (value) {
                case Wireframe:
                    return "wireframe";
                case Lit:
                    return "lit";
                case Unlit:
                    return "unlit";
                case Shaded:
                default:
                    return "shaded";
            }
        }

        [[nodiscard]] const char *ToString(const EditorRenderingTier value) {
            using enum EditorRenderingTier;
            switch (value) {
                case Dx12Vulkan:
                    return "dx12_vulkan";
                case Dx11:
                    return "dx11";
                case Es3:
                    return "es3";
                case HighEnd:
                default:
                    return "high_end";
            }
        }

        [[nodiscard]] const char *ToString(const EditorAudioOutputDevice value) {
            using enum EditorAudioOutputDevice;
            switch (value) {
                case Headphones:
                    return "headphones";
                case Speakers:
                    return "speakers";
                case SystemDefault:
                default:
                    return "system_default";
            }
        }

        [[nodiscard]] const char *ToString(const EditorConsoleLogLevel value) {
            using enum EditorConsoleLogLevel;
            switch (value) {
                case Debug:
                    return "debug";
                case Info:
                    return "info";
                case Error:
                    return "error";
                case Warning:
                default:
                    return "warning";
            }
        }

        [[nodiscard]] const char *BoolString(const bool value) {
            return value ? "true" : "false";
        }

        struct SanitizedSettings {
            const EditorSettings &source;
            int autoSaveIntervalMinutes;
            int uiScalePercent;
            int codeFontSizePx;
            int orbitSensitivity;
            int panSensitivity;
            int masterVolume;
            int maxPreviewClients;
            int simulatedLatencyMs;
            int packageDownloadThreads;
            float stutterThresholdMs;
        };

        [[nodiscard]] SanitizedSettings SanitizeForWrite(const EditorSettings &settings) {
            const auto bounded = [](const int value, const int minValue, const int maxValue) {
                return std::clamp(value, minValue, maxValue);
            };
            const auto boundedFloat = [](const float value, const float minValue, const float maxValue) {
                return std::isfinite(value) ? std::clamp(value, minValue, maxValue) : minValue;
            };
            return {
                settings,
                bounded(settings.autoSaveIntervalMinutes, 0, 30),
                bounded(settings.uiScalePercent, 75, 200),
                bounded(settings.codeFontSizePx, 14, 24),
                bounded(settings.orbitSensitivity, 10, 300),
                bounded(settings.panSensitivity, 10, 300),
                bounded(settings.masterVolume, 0, 100),
                static_cast<int>(std::clamp(settings.networkPreviewPreferences.maxPreviewClients,
                                            Network::NetworkPreviewPreferences::MinimumPreviewClients,
                                            Network::NetworkPreviewPreferences::MaximumPreviewClients)),
                static_cast<int>(std::min(settings.networkPreviewPreferences.simulatedLatencyMilliseconds,
                                          Network::NetworkPreviewPreferences::MaximumSimulatedLatencyMilliseconds)),
                bounded(settings.packages.downloadThreads, 1, 32),
                boundedFloat(settings.stutterThresholdMs, 1.0F, 1000.0F),
            };
        }

        void WriteEditorGroup(std::ostream &out, const SanitizedSettings &settings) {
            const EditorSettings &source = settings.source;
            out << "  \"editor\": {\n";
            out << R"(    "startupBehavior": ")" << ToString(source.startupBehavior) << "\",\n";
            out << R"(    "autoSaveIntervalMinutes": )" << settings.autoSaveIntervalMinutes << ",\n";
            out << R"(    "confirmExitWithUnsavedChanges": )" << BoolString(source.confirmExitWithUnsavedChanges) << ",\n";
            out << R"(    "restoreWorkspaceLayout": )" << BoolString(source.restoreWorkspaceLayout) << ",\n";
            out << R"(    "defaultSceneOnProjectOpen": ")" << EscapeJsonString(source.defaultSceneOnProjectOpen) << "\",\n";
            out << R"(    "languageTag": ")" << EscapeJsonString(source.languageTag) << "\",\n";
            out << "  },\n";
        }

        void WriteAppearanceGroup(std::ostream &out, const SanitizedSettings &settings) {
            const EditorSettings &source = settings.source;
            out << "  \"appearance\": {\n";
            out << R"(    "themePreset": ")" << ToString(source.themePreset) << "\",\n";
            out << R"(    "accentColorHex": ")" << EscapeJsonString(source.accentColorHex) << "\",\n";
            out << R"(    "uiScalePercent": )" << settings.uiScalePercent << ",\n";
            out << R"(    "codeFontSizePx": )" << settings.codeFontSizePx << ",\n";
            out << R"(    "uiFontFamily": ")" << EscapeJsonString(source.uiFontFamily) << "\",\n";
            out << R"(    "codeFontFamily": ")" << EscapeJsonString(source.codeFontFamily) << "\"\n";
            out << "  },\n";
        }

        void WriteInputGroup(std::ostream &out, const SanitizedSettings &settings) {
            const EditorSettings &source = settings.source;
            out << "  \"input\": {\n";
            out << R"(    "orbitSensitivity": )" << settings.orbitSensitivity << ",\n";
            out << R"(    "panSensitivity": )" << settings.panSensitivity << ",\n";
            out << R"(    "invertOrbitY": )" << BoolString(source.invertOrbitY) << "\n";
            out << "  },\n";
        }

        void WriteRenderingGroup(std::ostream &out, const SanitizedSettings &settings) {
            const EditorSettings &source = settings.source;
            out << "  \"rendering\": {\n";
            out << R"(    "viewportMode": ")" << ToString(source.viewportMode) << "\",\n";
            out << R"(    "gridOverlay": )" << BoolString(source.gridOverlay) << ",\n";
            out << R"(    "renderingTier": ")" << ToString(source.renderingTier) << "\",\n";
            out << R"(    "textureStreamingBudget": ")" << EscapeJsonString(source.textureStreamingBudget) << "\"\n";
            out << "  },\n";
        }

        void WriteAudioGroup(std::ostream &out, const SanitizedSettings &settings) {
            const EditorSettings &source = settings.source;
            out << "  \"audio\": {\n";
            out << R"(    "masterVolume": )" << settings.masterVolume << ",\n";
            out << R"(    "audioOutputDevice": ")" << ToString(source.audioOutputDevice) << "\",\n";
            out << R"(    "audioEnabled": )" << BoolString(source.audioEnabled) << "\n";
            out << "  },\n";
        }

        void WriteNetworkGroup(std::ostream &out, const SanitizedSettings &settings) {
            out << "  \"network\": {\n";
            out << R"(    "maxPreviewClients": )" << settings.maxPreviewClients << ",\n";
            out << R"(    "simulatedLatencyMs": )" << settings.simulatedLatencyMs << "\n";
            out << "  },\n";
        }

        void WritePackageGroup(std::ostream &out, const SanitizedSettings &settings) {
            out << "  \"packages\": {\n";
            out << R"(    "downloadThreads": )" << settings.packageDownloadThreads << "\n";
            out << "  },\n";
        }

        void WriteDiagnosticsGroup(std::ostream &out, const SanitizedSettings &settings) {
            const EditorSettings &source = settings.source;
            out << "  \"diagnostics\": {\n";
            out << R"(    "consoleLogLevel": ")" << ToString(source.consoleLogLevel) << "\",\n";
            out << R"(    "writeLogToFile": )" << BoolString(source.writeLogToFile) << ",\n";
            out << R"(    "autoCaptureOnStutter": )" << BoolString(source.autoCaptureOnStutter) << ",\n";
            out << std::format("    \"stutterThresholdMs\": {:.1f}\n", settings.stutterThresholdMs);
            out << "  }\n";
        }
    }  // namespace

    void WriteSettings(std::ostream &out, const EditorSettings &settings) {
        const SanitizedSettings sanitized = SanitizeForWrite(settings);
        out << "{\n";
        WriteEditorGroup(out, sanitized);
        WriteAppearanceGroup(out, sanitized);
        WriteInputGroup(out, sanitized);
        WriteRenderingGroup(out, sanitized);
        WriteAudioGroup(out, sanitized);
        WriteNetworkGroup(out, sanitized);
        WritePackageGroup(out, sanitized);
        WriteDiagnosticsGroup(out, sanitized);
        out << "}\n";
    }
}  // namespace Horo::Editor::SettingsStoreInternal
