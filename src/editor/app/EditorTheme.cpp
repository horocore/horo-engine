#include "Horo/Editor/EditorTheme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::Editor::Theme {
    namespace {
        using namespace DesignSystem;

        void ApplyHoroDark(ImGuiStyle &style);  // forward — defined below
        void ApplyMidnight(ImGuiStyle &style);  // forward — defined below
        void ApplyLight(ImGuiStyle &style);     // forward — defined below

        // ── Active design tokens (runtime-switchable) ──────────────────
        DesignTokens &ActiveTokens() {
            static DesignTokens tokens = DefaultDesignTokens();
            return tokens;
        }

        DesignTokens &UnscaledTokens() {
            static DesignTokens tokens = DefaultDesignTokens();
            return tokens;
        }

        void ResolveActiveTokens() {
            ActiveTokens() = UnscaledTokens();
        }

        void InstallUnscaledTokens(DesignTokens tokens) {
            UnscaledTokens() = std::move(tokens);
            ResolveActiveTokens();
        }

        /** @brief Light theme design tokens — surface colors inverted, accents unchanged. */
        void ApplyLightDesignTokens() {
            DesignTokens tokens = DefaultDesignTokens();
            tokens.colors = ColorTokens{
                ImVec4{0.941F, 0.937F, 0.929F, 1.0F},   // surfaceRoot
                ImVec4{0.961F, 0.957F, 0.953F, 1.0F},   // surfaceWindow
                ImVec4{0.929F, 0.925F, 0.918F, 1.0F},   // surfacePanel
                ImVec4{0.902F, 0.898F, 0.890F, 1.0F},   // surfaceRaised
                ImVec4{0.867F, 0.863F, 0.855F, 1.0F},   // surfaceHover
                ImVec4{0.784F, 0.780F, 0.773F, 1.0F},   // border
                ImVec4{0.659F, 0.655F, 0.647F, 1.0F},   // borderStrong
                ImVec4{0.125F, 0.129F, 0.137F, 1.0F},   // textPrimary
                ImVec4{0.400F, 0.396F, 0.388F, 1.0F},   // textMuted
                ImVec4{0.529F, 0.525F, 0.518F, 1.0F},   // textDim
                ImVec4{0.016F, 0.647F, 0.988F, 1.0F},   // actionPrimary
                ImVec4{0.180F, 0.706F, 0.992F, 1.0F},   // actionPrimaryHover
                ImVec4{0.000F, 0.500F, 0.820F, 1.0F},   // actionPrimaryActive
                ImVec4{0.016F, 0.647F, 0.988F, 0.15F},  // actionPrimarySoft
                ImVec4{0.373F, 0.722F, 0.541F, 1.0F},   // statusOk
                ImVec4{0.910F, 0.639F, 0.239F, 1.0F},   // statusWarn
                ImVec4{0.831F, 0.322F, 0.290F, 1.0F},   // statusError
                ImVec4{0.071F, 0.082F, 0.102F, 1.0F},   // textOnActionPrimary
            };
            InstallUnscaledTokens(std::move(tokens));
        }

        /** @brief Midnight theme design tokens — dark surface with purple accents. */
        void ApplyMidnightDesignTokens() {
            DesignTokens tokens = DefaultDesignTokens();
            tokens.colors = ColorTokens{
                ImVec4{0.027F, 0.039F, 0.063F, 1.0F},   // surfaceRoot
                ImVec4{0.047F, 0.063F, 0.094F, 1.0F},   // surfaceWindow
                ImVec4{0.059F, 0.078F, 0.110F, 1.0F},   // surfacePanel
                ImVec4{0.086F, 0.106F, 0.149F, 1.0F},   // surfaceRaised
                ImVec4{0.106F, 0.125F, 0.173F, 1.0F},   // surfaceHover
                ImVec4{0.149F, 0.169F, 0.216F, 1.0F},   // border
                ImVec4{0.212F, 0.231F, 0.278F, 1.0F},   // borderStrong
                ImVec4{0.867F, 0.855F, 0.898F, 1.0F},   // textPrimary
                ImVec4{0.600F, 0.580F, 0.640F, 1.0F},   // textMuted
                ImVec4{0.361F, 0.349F, 0.400F, 1.0F},   // textDim
                ImVec4{0.447F, 0.282F, 0.847F, 1.0F},   // actionPrimary
                ImVec4{0.545F, 0.400F, 0.902F, 1.0F},   // actionPrimaryHover
                ImVec4{0.369F, 0.220F, 0.749F, 1.0F},   // actionPrimaryActive
                ImVec4{0.447F, 0.282F, 0.847F, 0.15F},  // actionPrimarySoft
                ImVec4{0.373F, 0.722F, 0.541F, 1.0F},   // statusOk
                ImVec4{0.910F, 0.639F, 0.239F, 1.0F},   // statusWarn
                ImVec4{0.831F, 0.322F, 0.290F, 1.0F},   // statusError
                ImVec4{0.020F, 0.075F, 0.110F, 1.0F},   // textOnActionPrimary
            };
            InstallUnscaledTokens(std::move(tokens));
        }

        // ── Built-in theme definitions ───────────────────────────────────
        void ApplyColorsToStyle(const std::unordered_map<std::string, ImVec4, ThemeStringHash, std::equal_to<>> &colors,
                                ImGuiStyle &style) {
            ImVec4 *const c = style.Colors;
            auto get = [&](const char *key, const ImVec4 fallback) {
                const auto it = colors.find(key);
                return it != colors.end() ? it->second : fallback;
            };

            c[ImGuiCol_WindowBg] = get("WindowBg", ImVec4{0.039F, 0.047F, 0.059F, 1.0F});
            c[ImGuiCol_ChildBg] = get("ChildBg", ImVec4{0.071F, 0.082F, 0.102F, 1.0F});
            c[ImGuiCol_PopupBg] = get("PopupBg", ImVec4{0.071F, 0.082F, 0.102F, 1.0F});
            c[ImGuiCol_FrameBg] = get("FrameBg", ImVec4{0.122F, 0.141F, 0.169F, 1.0F});
            c[ImGuiCol_FrameBgHovered] = get("FrameBgHovered", ImVec4{0.137F, 0.157F, 0.188F, 1.0F});
            c[ImGuiCol_FrameBgActive] = get("FrameBgActive", ImVec4{0.137F, 0.157F, 0.188F, 1.0F});
            c[ImGuiCol_Button] = get("Button", ImVec4{0.122F, 0.141F, 0.169F, 1.0F});
            c[ImGuiCol_ButtonHovered] = get("ButtonHovered", ImVec4{0.016F, 0.647F, 0.988F, 0.14F});
            c[ImGuiCol_ButtonActive] = get("ButtonActive", ImVec4{0.016F, 0.647F, 0.988F, 0.24F});
            c[ImGuiCol_Text] = get("Text", ImVec4{0.910F, 0.894F, 0.851F, 1.0F});
            c[ImGuiCol_TextDisabled] = get("TextDisabled", ImVec4{0.369F, 0.357F, 0.329F, 1.0F});
            c[ImGuiCol_Border] = get("Border", ImVec4{0.165F, 0.184F, 0.216F, 1.0F});
            c[ImGuiCol_BorderShadow] = get("BorderShadow", ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
            c[ImGuiCol_ScrollbarBg] = get("ScrollbarBg", ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
            c[ImGuiCol_ScrollbarGrab] = get("ScrollbarGrab", ImVec4{0.227F, 0.251F, 0.286F, 1.0F});
            c[ImGuiCol_ScrollbarGrabHovered] = get("ScrollbarGrabHovered", ImVec4{0.369F, 0.357F, 0.329F, 1.0F});
            c[ImGuiCol_ScrollbarGrabActive] = get("ScrollbarGrabActive", ImVec4{0.910F, 0.894F, 0.851F, 1.0F});
            c[ImGuiCol_CheckMark] = get("CheckMark", ImVec4{0.016F, 0.647F, 0.988F, 1.0F});
            c[ImGuiCol_SliderGrab] = get("SliderGrab", ImVec4{0.016F, 0.647F, 0.988F, 1.0F});
            c[ImGuiCol_SliderGrabActive] = get("SliderGrabActive", ImVec4{0.149F, 0.714F, 0.992F, 1.0F});
            c[ImGuiCol_Header] = get("Header", ImVec4{0.016F, 0.647F, 0.988F, 0.14F});
            c[ImGuiCol_HeaderHovered] = get("HeaderHovered", ImVec4{0.137F, 0.157F, 0.188F, 1.0F});
            c[ImGuiCol_HeaderActive] = get("HeaderActive", ImVec4{0.137F, 0.157F, 0.188F, 1.0F});
            c[ImGuiCol_ResizeGrip] = get("ResizeGrip", ImVec4{0.016F, 0.647F, 0.988F, 0.25F});
            c[ImGuiCol_ResizeGripHovered] = get("ResizeGripHovered", ImVec4{0.016F, 0.647F, 0.988F, 0.67F});
            c[ImGuiCol_ResizeGripActive] = get("ResizeGripActive", ImVec4{0.016F, 0.647F, 0.988F, 1.0F});
            c[ImGuiCol_PlotLines] = get("PlotLines", ImVec4{0.016F, 0.647F, 0.988F, 1.0F});
            c[ImGuiCol_PlotHistogram] = get("PlotHistogram", ImVec4{0.016F, 0.647F, 0.988F, 1.0F});
            c[ImGuiCol_TableBorderStrong] = get("TableBorderStrong", ImVec4{0.165F, 0.184F, 0.216F, 1.0F});
            c[ImGuiCol_TableBorderLight] = get("TableBorderLight", ImVec4{0.165F, 0.184F, 0.216F, 0.5F});
        }

        std::vector<ThemeEntry> &ThemeList() {
            static std::vector<ThemeEntry> themes;
            return themes;
        }

        int &ActiveThemeIndex() {
            static int index = 0;
            return index;
        }

        [[nodiscard]] bool ParseHexColor(const std::string &hex, ImVec4 &color) {
            if (hex.size() != 7U || hex[0] != '#')
                return false;
            const auto hexDigit = [](const char value) {
                if (value >= '0' && value <= '9')
                    return value - '0';
                if (value >= 'a' && value <= 'f')
                    return value - 'a' + 10;
                if (value >= 'A' && value <= 'F')
                    return value - 'A' + 10;
                return -1;
            };

            const std::array digits{hexDigit(hex[1]), hexDigit(hex[2]), hexDigit(hex[3]),
                                    hexDigit(hex[4]), hexDigit(hex[5]), hexDigit(hex[6])};
            if (std::ranges::any_of(digits, [](const int value) {
                return value < 0;
            }))
                return false;
            const auto red = digits[0] * 16 + digits[1];
            const auto green = digits[2] * 16 + digits[3];
            const auto blue = digits[4] * 16 + digits[5];
            color = ImVec4{static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F, static_cast<float>(blue) / 255.0F, 1.0F};
            return true;
        }

        void OverrideFloat(const nlohmann::json &object, const char *key, float &target) {
            const auto found = object.find(key);
            if (found != object.end() && found->is_number()) {
                const float value = found->get<float>();
                if (std::isfinite(value) && value >= 0.0F)
                    target = value;
            }
        }

        void OverrideComponentMetrics(const nlohmann::json &object, ComponentSizeMetrics &metrics) {
            OverrideFloat(object, "fontSize", metrics.fontSize);
            OverrideFloat(object, "paddingX", metrics.paddingX);
            OverrideFloat(object, "paddingY", metrics.paddingY);
            OverrideFloat(object, "minimumHeight", metrics.minimumHeight);
            OverrideFloat(object, "iconSize", metrics.iconSize);
        }

        /** @brief Reads finite bounded host motion settings from the selected user theme. */
        void ReadMotionOverrides(const nlohmann::json &root, MotionTokens &motion) {
            const auto found = root.find("motion");
            if (found == root.end() || !found->is_object())
                return;
            const auto duration = [&](const char *name, float &value) {
                const auto field = found->find(name);
                if (field != found->end() && field->is_number()) {
                    const float candidate = field->get<float>();
                    if (std::isfinite(candidate) && candidate >= 0 && candidate <= 2)
                        value = candidate;
                }
            };
            duration("fast", motion.fast);
            duration("normal", motion.normal);
            duration("slow", motion.slow);
            duration("hover", motion.hover);
            duration("press", motion.press);
            duration("focus", motion.focus);
            duration("modal", motion.modal);
            if (const auto field = found->find("reduceMotion"); field != found->end() && field->is_boolean())
                motion.reduceMotion = field->get<bool>();
        }

        void ReadDesignTokenOverrides(const nlohmann::json &root, DesignTokens &tokens) {
            const auto tokenIt = root.find("tokens");
            if (tokenIt == root.end() || !tokenIt->is_object())
                return;
            const nlohmann::json &tokenJson = *tokenIt;

            if (const auto sizesIt = tokenJson.find("componentSizes"); sizesIt != tokenJson.end() && sizesIt->is_object()) {
                constexpr std::array<const char *, 5> names{"xs", "s", "m", "l", "xl"};
                for (std::size_t index = 0; index < names.size(); ++index) {
                    const auto metricsIt = sizesIt->find(names[index]);
                    if (metricsIt != sizesIt->end() && metricsIt->is_object())
                        OverrideComponentMetrics(*metricsIt, tokens.components.sizes[index]);
                }
            }

            if (const auto spacingIt = tokenJson.find("styleSpacing"); spacingIt != tokenJson.end() && spacingIt->is_object()) {
                constexpr std::array<const char *, 5> names{"xs", "s", "m", "l", "xl"};
                for (std::size_t index = 0; index < names.size(); ++index)
                    OverrideFloat(*spacingIt, names[index], tokens.components.spacing[index + 1U]);
            }
            if (const auto spacingIt = tokenJson.find("layoutSpacing"); spacingIt != tokenJson.end() && spacingIt->is_object()) {
                OverrideFloat(*spacingIt, "propertyRowGap", tokens.spacing.propertyRowGap);
            }

            if (const auto typographyIt = tokenJson.find("typography"); typographyIt != tokenJson.end() && typographyIt->is_object()) {
                OverrideFloat(*typographyIt, "sansBase", tokens.typography.sansBase);
                OverrideFloat(*typographyIt, "sansCompactBase", tokens.typography.sansCompactBase);
                OverrideFloat(*typographyIt, "sansEmphasisBase", tokens.typography.sansEmphasisBase);
                OverrideFloat(*typographyIt, "caption", tokens.typography.caption);
                OverrideFloat(*typographyIt, "label", tokens.typography.label);
                OverrideFloat(*typographyIt, "body", tokens.typography.body);
                OverrideFloat(*typographyIt, "cardTitle", tokens.typography.cardTitle);
                OverrideFloat(*typographyIt, "title", tokens.typography.title);
                OverrideFloat(*typographyIt, "heading", tokens.typography.heading);
                OverrideFloat(*typographyIt, "display", tokens.typography.display);
            }
            if (const auto radiiIt = tokenJson.find("radii"); radiiIt != tokenJson.end() && radiiIt->is_object()) {
                OverrideFloat(*radiiIt, "control", tokens.radii.control);
                OverrideFloat(*radiiIt, "card", tokens.radii.card);
                OverrideFloat(*radiiIt, "modal", tokens.radii.modal);
            }
            ReadMotionOverrides(tokenJson, tokens.motion);
            NormalizeDesignTokens(tokens);
        }

        [[nodiscard]] bool ReadJsonColor(const nlohmann::json &value, ImVec4 &color) {
            if (value.is_string())
                return ParseHexColor(value.get<std::string>(), color);
            if (!value.is_array() || value.size() < 3U || value.size() > 4U)
                return false;
            for (const auto &channel : value) {
                if (!channel.is_number())
                    return false;
            }
            color = ImVec4{value[0].get<float>(), value[1].get<float>(), value[2].get<float>(),
                           value.size() == 4U ? value[3].get<float>() : 1.0F};
            return true;
        }

        void ReadThemeColors(const nlohmann::json &root,
                             std::unordered_map<std::string, ImVec4, ThemeStringHash, std::equal_to<>> &colors) {
            const auto colorsIt = root.find("colors");
            if (colorsIt == root.end() || !colorsIt->is_object())
                return;
            for (const auto &[key, value] : colorsIt->items()) {
                ImVec4 color{};
                if (ReadJsonColor(value, color))
                    colors[key] = color;
            }
        }

        /** @brief Returns the themes directory path. */
        std::string GetThemesDir() {
            const char *home = std::getenv("HOME");
            if (home == nullptr)
                home = std::getenv("USERPROFILE");
            if (home == nullptr)
                return ".horo/themes";

            std::string dir{home};
            dir += "/.horo/themes";
            return dir;
        }

        void AppendCustomThemes(const char *additionalPath) {
            std::vector<std::filesystem::path> scanPaths{GetThemesDir()};
            if (additionalPath != nullptr && additionalPath[0] != '\0')
                scanPaths.emplace_back(additionalPath);

            for (const auto &dir : scanPaths) {
                std::error_code ec;
                if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
                    continue;
                for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (ec)
                        break;
                    if (!entry.is_regular_file() || entry.path().extension() != ".json")
                        continue;
                    ThemeEntry theme;
                    if (LoadThemeFromJson(entry.path().string().c_str(), theme))
                        ThemeList().push_back(std::move(theme));
                }
            }
        }
    }  // namespace

    const std::vector<ThemeEntry> &GetThemeList() {
        return ThemeList();
    }

    const DesignTokens &GetActiveTokens() {
        return ActiveTokens();
    }

    void RefreshThemeList(const char *additionalPath) {
        ThemeList().clear();

        // Built-in themes
        {
            ThemeEntry e;
            e.name = "Horo Dark";
            e.isBuiltIn = true;
            ThemeList().push_back(std::move(e));
        }
        {
            ThemeEntry e;
            e.name = "Midnight";
            e.isBuiltIn = true;
            e.colors["WindowBg"] = ImVec4{0.027F, 0.039F, 0.063F, 1.0F};
            e.colors["ChildBg"] = ImVec4{0.047F, 0.063F, 0.094F, 1.0F};
            e.colors["PopupBg"] = ImVec4{0.047F, 0.063F, 0.094F, 1.0F};
            e.colors["FrameBg"] = ImVec4{0.086F, 0.106F, 0.149F, 1.0F};
            e.colors["FrameBgHovered"] = ImVec4{0.106F, 0.125F, 0.173F, 1.0F};
            e.colors["FrameBgActive"] = ImVec4{0.106F, 0.125F, 0.173F, 1.0F};
            e.colors["Button"] = ImVec4{0.086F, 0.106F, 0.149F, 1.0F};
            e.colors["ButtonHovered"] = ImVec4{0.447F, 0.282F, 0.847F, 0.18F};
            e.colors["ButtonActive"] = ImVec4{0.447F, 0.282F, 0.847F, 0.28F};
            e.colors["Text"] = ImVec4{0.867F, 0.855F, 0.898F, 1.0F};
            e.colors["TextDisabled"] = ImVec4{0.361F, 0.349F, 0.400F, 1.0F};
            e.colors["Border"] = ImVec4{0.149F, 0.169F, 0.216F, 1.0F};
            e.colors["ScrollbarGrab"] = ImVec4{0.212F, 0.231F, 0.278F, 1.0F};
            e.colors["ScrollbarGrabHovered"] = ImVec4{0.361F, 0.349F, 0.400F, 1.0F};
            e.colors["ScrollbarGrabActive"] = ImVec4{0.867F, 0.855F, 0.898F, 1.0F};
            e.colors["CheckMark"] = ImVec4{0.447F, 0.282F, 0.847F, 1.0F};
            e.colors["SliderGrab"] = ImVec4{0.447F, 0.282F, 0.847F, 1.0F};
            e.colors["SliderGrabActive"] = ImVec4{0.545F, 0.400F, 0.902F, 1.0F};
            e.colors["Header"] = ImVec4{0.447F, 0.282F, 0.847F, 0.18F};
            e.colors["HeaderHovered"] = ImVec4{0.106F, 0.125F, 0.173F, 1.0F};
            e.colors["HeaderActive"] = ImVec4{0.447F, 0.282F, 0.847F, 0.25F};
            e.colors["ResizeGrip"] = ImVec4{0.447F, 0.282F, 0.847F, 0.25F};
            e.colors["ResizeGripHovered"] = ImVec4{0.447F, 0.282F, 0.847F, 0.67F};
            e.colors["ResizeGripActive"] = ImVec4{0.447F, 0.282F, 0.847F, 1.0F};
            e.colors["PlotLines"] = ImVec4{0.447F, 0.282F, 0.847F, 1.0F};
            e.colors["PlotHistogram"] = ImVec4{0.447F, 0.282F, 0.847F, 1.0F};
            e.colors["TableBorderStrong"] = ImVec4{0.149F, 0.169F, 0.216F, 1.0F};
            e.colors["TableBorderLight"] = ImVec4{0.149F, 0.169F, 0.216F, 0.5F};
            ThemeList().push_back(std::move(e));
        }
        {
            ThemeEntry e;
            e.name = "Light";
            e.isBuiltIn = true;
            e.colors["WindowBg"] = ImVec4{0.941F, 0.937F, 0.929F, 1.0F};
            e.colors["ChildBg"] = ImVec4{0.961F, 0.957F, 0.953F, 1.0F};
            e.colors["PopupBg"] = ImVec4{0.961F, 0.957F, 0.953F, 1.0F};
            e.colors["FrameBg"] = ImVec4{0.902F, 0.898F, 0.890F, 1.0F};
            e.colors["FrameBgHovered"] = ImVec4{0.867F, 0.863F, 0.855F, 1.0F};
            e.colors["FrameBgActive"] = ImVec4{0.867F, 0.863F, 0.855F, 1.0F};
            e.colors["Button"] = ImVec4{0.902F, 0.898F, 0.890F, 1.0F};
            e.colors["ButtonHovered"] = ImVec4{0.016F, 0.647F, 0.988F, 0.12F};
            e.colors["ButtonActive"] = ImVec4{0.016F, 0.647F, 0.988F, 0.22F};
            e.colors["Text"] = ImVec4{0.125F, 0.129F, 0.137F, 1.0F};
            e.colors["TextDisabled"] = ImVec4{0.529F, 0.525F, 0.518F, 1.0F};
            e.colors["Border"] = ImVec4{0.784F, 0.780F, 0.773F, 1.0F};
            e.colors["ScrollbarGrab"] = ImVec4{0.659F, 0.655F, 0.647F, 1.0F};
            e.colors["ScrollbarGrabHovered"] = ImVec4{0.529F, 0.525F, 0.518F, 1.0F};
            e.colors["ScrollbarGrabActive"] = ImVec4{0.125F, 0.129F, 0.137F, 1.0F};
            e.colors["CheckMark"] = ImVec4{0.016F, 0.647F, 0.988F, 1.0F};
            e.colors["SliderGrab"] = ImVec4{0.016F, 0.647F, 0.988F, 1.0F};
            e.colors["SliderGrabActive"] = ImVec4{0.149F, 0.714F, 0.992F, 1.0F};
            e.colors["Header"] = ImVec4{0.016F, 0.647F, 0.988F, 0.12F};
            e.colors["HeaderHovered"] = ImVec4{0.867F, 0.863F, 0.855F, 1.0F};
            e.colors["HeaderActive"] = ImVec4{0.016F, 0.647F, 0.988F, 0.18F};
            e.colors["ResizeGrip"] = ImVec4{0.016F, 0.647F, 0.988F, 0.25F};
            e.colors["ResizeGripHovered"] = ImVec4{0.016F, 0.647F, 0.988F, 0.67F};
            e.colors["ResizeGripActive"] = ImVec4{0.016F, 0.647F, 0.988F, 1.0F};
            e.colors["PlotLines"] = ImVec4{0.016F, 0.647F, 0.988F, 1.0F};
            e.colors["PlotHistogram"] = ImVec4{0.016F, 0.647F, 0.988F, 1.0F};
            e.colors["TableBorderStrong"] = ImVec4{0.784F, 0.780F, 0.773F, 1.0F};
            e.colors["TableBorderLight"] = ImVec4{0.784F, 0.780F, 0.773F, 0.5F};
            ThemeList().push_back(std::move(e));
        }

        AppendCustomThemes(additionalPath);
    }

    bool LoadThemeFromJson(const char *path, ThemeEntry &outEntry) {
        std::ifstream file(path);  // NOSONAR(cpp:S2083)
        if (!file.is_open())
            return false;

        std::ostringstream oss;
        oss << file.rdbuf();
        try {
            const nlohmann::json document = nlohmann::json::parse(oss.str(), nullptr, false, true);
            if (document.is_discarded() || !document.is_object())
                return false;

            ThemeEntry loaded;
            const auto nameIt = document.find("name");
            loaded.name =
                nameIt != document.end() && nameIt->is_string() ? nameIt->get<std::string>() : std::filesystem::path(path).stem().string();
            loaded.sourcePath = path;
            loaded.isBuiltIn = false;
            loaded.designTokens = DefaultDesignTokens();
            ReadThemeColors(document, loaded.colors);
            ReadDesignTokenOverrides(document, loaded.designTokens);
            if (const auto tokensIt = document.find("tokens");
                loaded.colors.empty() && (tokensIt == document.end() || !tokensIt->is_object()))
                return false;
            outEntry = std::move(loaded);
            return true;
        } catch (const nlohmann::json::exception &) {
            return false;
        }
    }

    void SetUiScalePercent(const int percent) {
        // UI scaling is intentionally disabled until font, layout, and viewport
        // scaling can be resolved through one coherent DPI-aware policy.
        static_cast<void>(percent);
        ResolveActiveTokens();
    }

    void SelectThemeByIndex(const int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= ThemeList().size())
            return;

        ActiveThemeIndex() = index;
        const auto &entry = ThemeList()[index];

        // Always apply ImGui style colors when the entry carries custom values.
        if (!entry.colors.empty())
            ApplyColorsToStyle(entry.colors, ImGui::GetStyle());

        // Select the DesignTokens based on the built-in preset index.
        // Built-in entries (0=HoroDark, 1=Midnight, 2=Light) may or may
        // not carry a colors map, but the token set is always known.
        if (entry.isBuiltIn) {
            if (index == 0) {
                // Horo Dark — apply style fallback if no colors in entry
                if (entry.colors.empty())
                    ApplyHoroDark(ImGui::GetStyle());
                InstallUnscaledTokens(DefaultDesignTokens());
            } else if (index == 1) {
                // Midnight — dark surface + purple accent
                if (entry.colors.empty())
                    ApplyMidnight(ImGui::GetStyle());
                ApplyMidnightDesignTokens();
            } else if (index == 2) {
                // Light — inverted surface + blue accent
                if (entry.colors.empty())
                    ApplyLight(ImGui::GetStyle());
                ApplyLightDesignTokens();
            } else {
                // Future built-in: fall back to Horo Dark tokens
                InstallUnscaledTokens(DefaultDesignTokens());
            }
        } else {
            // Custom JSON theme: ImGui style already applied above;
            // derive tokens from ImGui style colors.
            DesignTokens tokens = entry.designTokens;
            const ImVec4 *const c = ImGui::GetStyle().Colors;
            tokens.colors.surfaceRoot = c[ImGuiCol_WindowBg];
            tokens.colors.surfaceWindow = c[ImGuiCol_ChildBg];
            tokens.colors.surfacePanel = c[ImGuiCol_PopupBg];
            tokens.colors.surfaceRaised = c[ImGuiCol_FrameBg];
            tokens.colors.surfaceHover = c[ImGuiCol_FrameBgHovered];
            tokens.colors.border = c[ImGuiCol_Border];
            tokens.colors.borderStrong = c[ImGuiCol_TableBorderStrong];
            tokens.colors.textPrimary = c[ImGuiCol_Text];
            tokens.colors.textMuted = c[ImGuiCol_TextDisabled];
            tokens.colors.textDim = c[ImGuiCol_TextDisabled];
            tokens.colors.actionPrimary = c[ImGuiCol_CheckMark];
            tokens.colors.actionPrimaryHover = c[ImGuiCol_SliderGrabActive];
            tokens.colors.actionPrimaryActive = c[ImGuiCol_SliderGrabActive];
            tokens.colors.actionPrimarySoft = c[ImGuiCol_Header];
            tokens.colors.textOnActionPrimary = c[ImGuiCol_WindowBg];
            InstallUnscaledTokens(std::move(tokens));
        }
    }

    int GetActiveThemeIndex() {
        return ActiveThemeIndex();
    }

    // ── Legacy preset API (for backward compat) ──────────────────────────

    namespace {
        void ApplyHoroDark(ImGuiStyle &style) {
            auto *c = style.Colors;
            c[ImGuiCol_WindowBg] = {0.039F, 0.047F, 0.059F, 1.0F};
            c[ImGuiCol_ChildBg] = {0.071F, 0.082F, 0.102F, 1.0F};
            c[ImGuiCol_PopupBg] = {0.071F, 0.082F, 0.102F, 1.0F};
            c[ImGuiCol_FrameBg] = {0.122F, 0.141F, 0.169F, 1.0F};
            c[ImGuiCol_FrameBgHovered] = {0.137F, 0.157F, 0.188F, 1.0F};
            c[ImGuiCol_FrameBgActive] = {0.137F, 0.157F, 0.188F, 1.0F};
            c[ImGuiCol_Button] = {0.122F, 0.141F, 0.169F, 1.0F};
            c[ImGuiCol_ButtonHovered] = {0.016F, 0.647F, 0.988F, 0.14F};
            c[ImGuiCol_ButtonActive] = {0.016F, 0.647F, 0.988F, 0.24F};
            c[ImGuiCol_Text] = {0.910F, 0.894F, 0.851F, 1.0F};
            c[ImGuiCol_TextDisabled] = {0.369F, 0.357F, 0.329F, 1.0F};
            c[ImGuiCol_Border] = {0.165F, 0.184F, 0.216F, 1.0F};
            c[ImGuiCol_BorderShadow] = {0.0F, 0.0F, 0.0F, 0.0F};
            c[ImGuiCol_ScrollbarBg] = {0.0F, 0.0F, 0.0F, 0.0F};
            c[ImGuiCol_ScrollbarGrab] = {0.227F, 0.251F, 0.286F, 1.0F};
            c[ImGuiCol_ScrollbarGrabHovered] = {0.369F, 0.357F, 0.329F, 1.0F};
            c[ImGuiCol_ScrollbarGrabActive] = {0.910F, 0.894F, 0.851F, 1.0F};
            c[ImGuiCol_CheckMark] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_SliderGrab] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_SliderGrabActive] = {0.149F, 0.714F, 0.992F, 1.0F};
            c[ImGuiCol_Header] = {0.016F, 0.647F, 0.988F, 0.14F};
            c[ImGuiCol_HeaderHovered] = {0.137F, 0.157F, 0.188F, 1.0F};
            c[ImGuiCol_HeaderActive] = {0.137F, 0.157F, 0.188F, 1.0F};
            c[ImGuiCol_ResizeGrip] = {0.016F, 0.647F, 0.988F, 0.25F};
            c[ImGuiCol_ResizeGripHovered] = {0.016F, 0.647F, 0.988F, 0.67F};
            c[ImGuiCol_ResizeGripActive] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_PlotLines] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_PlotHistogram] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_TableBorderStrong] = {0.165F, 0.184F, 0.216F, 1.0F};
            c[ImGuiCol_TableBorderLight] = {0.165F, 0.184F, 0.216F, 0.5F};
        }

        void ApplyMidnight(ImGuiStyle &style) {
            auto *c = style.Colors;
            c[ImGuiCol_WindowBg] = {0.027F, 0.039F, 0.063F, 1.0F};
            c[ImGuiCol_ChildBg] = {0.047F, 0.063F, 0.094F, 1.0F};
            c[ImGuiCol_PopupBg] = {0.047F, 0.063F, 0.094F, 1.0F};
            c[ImGuiCol_FrameBg] = {0.086F, 0.106F, 0.149F, 1.0F};
            c[ImGuiCol_FrameBgHovered] = {0.106F, 0.125F, 0.173F, 1.0F};
            c[ImGuiCol_FrameBgActive] = {0.106F, 0.125F, 0.173F, 1.0F};
            c[ImGuiCol_Button] = {0.086F, 0.106F, 0.149F, 1.0F};
            c[ImGuiCol_ButtonHovered] = {0.447F, 0.282F, 0.847F, 0.18F};
            c[ImGuiCol_ButtonActive] = {0.447F, 0.282F, 0.847F, 0.28F};
            c[ImGuiCol_Text] = {0.867F, 0.855F, 0.898F, 1.0F};
            c[ImGuiCol_TextDisabled] = {0.361F, 0.349F, 0.400F, 1.0F};
            c[ImGuiCol_Border] = {0.149F, 0.169F, 0.216F, 1.0F};
            c[ImGuiCol_BorderShadow] = {0.0F, 0.0F, 0.0F, 0.0F};
            c[ImGuiCol_ScrollbarBg] = {0.0F, 0.0F, 0.0F, 0.0F};
            c[ImGuiCol_ScrollbarGrab] = {0.212F, 0.231F, 0.278F, 1.0F};
            c[ImGuiCol_ScrollbarGrabHovered] = {0.361F, 0.349F, 0.400F, 1.0F};
            c[ImGuiCol_ScrollbarGrabActive] = {0.867F, 0.855F, 0.898F, 1.0F};
            c[ImGuiCol_CheckMark] = {0.447F, 0.282F, 0.847F, 1.0F};
            c[ImGuiCol_SliderGrab] = {0.447F, 0.282F, 0.847F, 1.0F};
            c[ImGuiCol_SliderGrabActive] = {0.545F, 0.400F, 0.902F, 1.0F};
            c[ImGuiCol_Header] = {0.447F, 0.282F, 0.847F, 0.18F};
            c[ImGuiCol_HeaderHovered] = {0.106F, 0.125F, 0.173F, 1.0F};
            c[ImGuiCol_HeaderActive] = {0.106F, 0.125F, 0.173F, 1.0F};
            c[ImGuiCol_ResizeGrip] = {0.447F, 0.282F, 0.847F, 0.25F};
            c[ImGuiCol_ResizeGripHovered] = {0.447F, 0.282F, 0.847F, 0.67F};
            c[ImGuiCol_ResizeGripActive] = {0.447F, 0.282F, 0.847F, 1.0F};
            c[ImGuiCol_PlotLines] = {0.447F, 0.282F, 0.847F, 1.0F};
            c[ImGuiCol_PlotHistogram] = {0.447F, 0.282F, 0.847F, 1.0F};
            c[ImGuiCol_TableBorderStrong] = {0.149F, 0.169F, 0.216F, 1.0F};
            c[ImGuiCol_TableBorderLight] = {0.149F, 0.169F, 0.216F, 0.5F};
        }

        void ApplyLight(ImGuiStyle &style) {
            auto *c = style.Colors;
            c[ImGuiCol_WindowBg] = {0.941F, 0.937F, 0.929F, 1.0F};
            c[ImGuiCol_ChildBg] = {0.961F, 0.957F, 0.953F, 1.0F};
            c[ImGuiCol_PopupBg] = {0.961F, 0.957F, 0.953F, 1.0F};
            c[ImGuiCol_FrameBg] = {0.902F, 0.898F, 0.890F, 1.0F};
            c[ImGuiCol_FrameBgHovered] = {0.867F, 0.863F, 0.855F, 1.0F};
            c[ImGuiCol_FrameBgActive] = {0.867F, 0.863F, 0.855F, 1.0F};
            c[ImGuiCol_Button] = {0.902F, 0.898F, 0.890F, 1.0F};
            c[ImGuiCol_ButtonHovered] = {0.016F, 0.647F, 0.988F, 0.12F};
            c[ImGuiCol_ButtonActive] = {0.016F, 0.647F, 0.988F, 0.22F};
            c[ImGuiCol_Text] = {0.125F, 0.129F, 0.137F, 1.0F};
            c[ImGuiCol_TextDisabled] = {0.529F, 0.525F, 0.518F, 1.0F};
            c[ImGuiCol_Border] = {0.784F, 0.780F, 0.773F, 1.0F};
            c[ImGuiCol_BorderShadow] = {0.0F, 0.0F, 0.0F, 0.0F};
            c[ImGuiCol_ScrollbarBg] = {0.0F, 0.0F, 0.0F, 0.0F};
            c[ImGuiCol_ScrollbarGrab] = {0.659F, 0.655F, 0.647F, 1.0F};
            c[ImGuiCol_ScrollbarGrabHovered] = {0.529F, 0.525F, 0.518F, 1.0F};
            c[ImGuiCol_ScrollbarGrabActive] = {0.125F, 0.129F, 0.137F, 1.0F};
            c[ImGuiCol_CheckMark] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_SliderGrab] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_SliderGrabActive] = {0.149F, 0.714F, 0.992F, 1.0F};
            c[ImGuiCol_Header] = {0.016F, 0.647F, 0.988F, 0.12F};
            c[ImGuiCol_HeaderHovered] = {0.867F, 0.863F, 0.855F, 1.0F};
            c[ImGuiCol_HeaderActive] = {0.867F, 0.863F, 0.855F, 1.0F};
            c[ImGuiCol_ResizeGrip] = {0.016F, 0.647F, 0.988F, 0.25F};
            c[ImGuiCol_ResizeGripHovered] = {0.016F, 0.647F, 0.988F, 0.67F};
            c[ImGuiCol_ResizeGripActive] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_PlotLines] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_PlotHistogram] = {0.016F, 0.647F, 0.988F, 1.0F};
            c[ImGuiCol_TableBorderStrong] = {0.784F, 0.780F, 0.773F, 1.0F};
            c[ImGuiCol_TableBorderLight] = {0.784F, 0.780F, 0.773F, 0.5F};
        }

        Preset &CurrentPreset() {
            static Preset preset = Preset::HoroDark;
            return preset;
        }
    }  // namespace

    void Apply(ImGuiStyle &style) {
        ImGui::StyleColorsDark();
        style.WindowRounding = 0;
        style.FrameRounding = Layout::Radius;
        style.ChildRounding = Layout::Radius;
        style.PopupRounding = Layout::Radius;
        style.ScrollbarRounding = Layout::Radius;
        style.GrabRounding = Layout::Radius;
        style.WindowBorderSize = 0;
        style.FrameBorderSize = 1;
        style.ChildBorderSize = 1;
        style.WindowPadding = ImVec2{0, 0};
        style.FramePadding = ImVec2{10, 7};
        style.ItemSpacing = ImVec2{8, 8};
        style.ItemInnerSpacing = ImVec2{8, 4};
        style.ScrollbarSize = 10.0F;

        RefreshThemeList(std::getenv("HORO_THEME_OVERRIDE"));

        // If env var points to a specific JSON file, load it directly
        const char *overridePath = std::getenv("HORO_THEME_OVERRIDE");
        bool selectedOverride = false;
        if (overridePath && overridePath[0] != '\0') {
            std::error_code ec;
            if (std::filesystem::is_regular_file(overridePath, ec)) {
                ThemeEntry theme;
                if (LoadThemeFromJson(overridePath, theme)) {
                    ThemeList().push_back(std::move(theme));
                    SelectThemeByIndex(static_cast<int>(GetThemeList().size()) - 1);
                    selectedOverride = true;
                }
            }
        }
        if (!selectedOverride)
            SelectThemeByIndex(0);
    }

    void SetThemePreset(const Preset preset) {
        CurrentPreset() = preset;
    }

    Preset GetThemePreset() {
        return CurrentPreset();
    }

    void ApplyCurrentTheme() {
        switch (CurrentPreset()) {
            case Preset::Midnight:
                ApplyMidnight(ImGui::GetStyle());
                ApplyMidnightDesignTokens();
                break;
            case Preset::Light:
                ApplyLight(ImGui::GetStyle());
                ApplyLightDesignTokens();
                break;
            default:
                ApplyHoroDark(ImGui::GetStyle());
                InstallUnscaledTokens(DefaultDesignTokens());
                break;
        }
    }
}  // namespace Horo::Editor::Theme
