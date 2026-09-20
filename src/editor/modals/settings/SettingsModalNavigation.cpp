#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Network/NetworkProjectSettings.h"
#include "SettingsModalInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <imgui.h>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace Horo::Editor::SettingsModalInternal {
    using namespace Theme;
    using namespace Ui;
    using Theme::ScopedTextStyle;

    void DrawNavGroup(const char *label, const EditorGuiContext &ctx) {
        ImGui::Dummy({0.0F, 5.0F});
        ScopedTextStyle ts(ctx.theme.fonts.sansEmphasis, TextPx::Label(), FontPx::SansEmphasis);
        ImGui::PushStyleColor(ImGuiCol_Text, Dim());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0F);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    }

    void DrawNavItem(SettingsState &st, const NavItem &item, const EditorGuiContext &ctx) {
        const bool active = st.activeTab == static_cast<int>(item.tab);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        constexpr float rowH = 38.0F;

        ImGui::PushID(item.label);
        ImGui::InvisibleButton("nav", {rowW, rowH});
        if (ImGui::IsItemClicked()) {
            if (st.activeTab != static_cast<int>(item.tab)) {
                LOG_DEBUG("editor.settings", "Settings tab changed to '%s'.", item.label);
            }
            st.activeTab = static_cast<int>(item.tab);
        }
        const bool hovered = ImGui::IsItemHovered();

        auto *dl = ImGui::GetWindowDrawList();
        if (active || hovered) {
            const auto accentGlow = ImVec4{Accent().x, Accent().y, Accent().z, 0.14F};
            dl->AddRectFilled(pos, {pos.x + rowW, pos.y + rowH}, U32(active ? accentGlow : Hover()), Layout::Radius);
        }
        if (active) {
            dl->AddRectFilled(pos, {pos.x + 2.0F, pos.y + rowH}, U32(Accent()), 1.0F);
        }

        ImGui::SetCursorScreenPos({pos.x + 12.0F, pos.y + 10.0F});
        {
            ScopedTextStyle ts(ctx.theme.fonts.sansEmphasis, TextPx::Label(), FontPx::SansEmphasis);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? Accent() : Muted());
            ImGui::TextUnformatted(item.icon);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0.0F, 10.0F);
        {
            ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Body(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? Text() : Muted());
            ImGui::TextUnformatted(item.label);
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorScreenPos({pos.x, pos.y + rowH + 1.0F});
        ImGui::PopID();
    }

    void DrawNavigationContent(SettingsState &st, const EditorGuiContext &ctx) {
        using enum SettingsTab;
        const std::string editor = ctx.localization.Get("editor", "settings.nav.editor");
        const std::string general = ctx.localization.Get("editor", "settings.nav.general");
        const std::string appearance = ctx.localization.Get("editor", "settings.nav.appearance");
        const std::string input = ctx.localization.Get("editor", "settings.nav.input");
        const std::string engine = ctx.localization.Get("editor", "settings.nav.engine");
        const std::string rendering = ctx.localization.Get("editor", "settings.nav.rendering");
        const std::string audio = ctx.localization.Get("editor", "settings.nav.audio");
        const std::string network = ctx.localization.Get("editor", "settings.nav.network");
        const std::string packages = ctx.localization.Get("editor", "settings.nav.packages");
        const std::string tools = ctx.localization.Get("editor", "settings.nav.tools");
        const std::string diagnostics = ctx.localization.Get("editor", "settings.nav.diagnostics");
        const std::string plugins = ctx.localization.Get("editor", "settings.nav.plugins");
        DrawNavGroup(editor.c_str(), ctx);
        DrawNavItem(st, {general.c_str(), "G", General}, ctx);
        DrawNavItem(st, {appearance.c_str(), "A", Appearance}, ctx);
        DrawNavItem(st, {input.c_str(), "I", Input}, ctx);
        DrawNavGroup(engine.c_str(), ctx);
        DrawNavItem(st, {rendering.c_str(), "R", Rendering}, ctx);
        DrawNavItem(st, {audio.c_str(), "S", Audio}, ctx);
        DrawNavItem(st, {network.c_str(), "N", Network}, ctx);
        DrawNavItem(st, {packages.c_str(), "K", Packages}, ctx);
        DrawNavGroup(tools.c_str(), ctx);
        DrawNavItem(st, {diagnostics.c_str(), "D", Diagnostics}, ctx);
        DrawNavItem(st, {plugins.c_str(), "P", Plugins}, ctx);
    }

    void DrawGeneral(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 3> startupStr = {ctx.localization.Get("editor", "settings.general.startup_welcome"),
                                                       ctx.localization.Get("editor", "settings.general.startup_last"),
                                                       ctx.localization.Get("editor", "settings.general.startup_browser")};
        const std::array<const char *, 3> kStartup = {startupStr[0].c_str(), startupStr[1].c_str(), startupStr[2].c_str()};
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.general");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string startupGroup = ctx.localization.Get("editor", "settings.general.startup_group");
        SettingGroup(startupGroup.c_str(), ctx.theme.fonts, true);
        const std::string startupLabel = ctx.localization.Get("editor", "settings.general.startup_behavior");
        const std::string startupDescription = ctx.localization.Get("editor", "settings.general.startup_behavior.description");
        const std::string autosaveLabel = ctx.localization.Get("editor", "settings.general.autosave_interval");
        const std::string autosaveDescription = ctx.localization.Get("editor", "settings.general.autosave_interval.description");
        const std::string confirmLabel = ctx.localization.Get("editor", "settings.general.confirm_exit");
        const std::string confirmDescription = ctx.localization.Get("editor", "settings.general.confirm_exit.description");
        const std::string restoreLabel = ctx.localization.Get("editor", "settings.general.restore_workspace");
        const std::string restoreDescription = ctx.localization.Get("editor", "settings.general.restore_workspace.description");
        const std::string defaultSceneLabel = ctx.localization.Get("editor", "settings.general.default_scene");
        const std::string defaultSceneDescription = ctx.localization.Get("editor", "settings.general.default_scene.description");
        SettingRow(startupLabel.c_str(), startupDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kStartup]() {
            (void)ComboControl("##startup", &st.general.startupAction, kStartup.data(), kStartup.size(), ctx.theme.fonts);
        });
        SettingRow(autosaveLabel.c_str(), autosaveDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##autosave", &st.general.autoSaveInterval, 0, 30, SliderValueFormat::Minutes, ctx.theme.fonts);
        });
        SettingRow(confirmLabel.c_str(), confirmDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("confirm-exit", &st.general.confirmExit, ctx.theme.fonts);
        });
        const std::string sessionGroup = ctx.localization.Get("editor", "settings.general.session_group");
        SettingGroup(sessionGroup.c_str(), ctx.theme.fonts);
        SettingRow(restoreLabel.c_str(), restoreDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("restore-workspace", &st.general.restoreWorkspace, ctx.theme.fonts);
        });
        SettingRow(defaultSceneLabel.c_str(), defaultSceneDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##default-scene", st.general.defaultScene, 64, ctx.theme.fonts);
        });
        const std::string english = ctx.localization.Get("editor", "settings.language.english");
        const std::string turkish = ctx.localization.Get("editor", "settings.language.turkish");
        const std::array<const char *, 2> kLanguages = {english.c_str(), turkish.c_str()};
        const std::string languageLabel = ctx.localization.Get("editor", "settings.language");
        const std::string languageDescription = ctx.localization.Get("editor", "settings.language.description");
        SettingRow(languageLabel.c_str(), languageDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kLanguages]() {
            int languageIndex = st.general.languageTag == "tr-TR" ? 1 : 0;
            if (ComboControl("##language", &languageIndex, kLanguages.data(), kLanguages.size(), ctx.theme.fonts)) {
                st.general.languageTag = languageIndex == 1 ? "tr-TR" : "en-US";
                st.dirty = true;
            }
        });
    }

    void DrawAppearance(SettingsState &st, const EditorGuiContext &ctx) {
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.appearance");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string themeGroup = ctx.localization.Get("editor", "settings.appearance.theme_group");
        SettingGroup(themeGroup.c_str(), ctx.theme.fonts, true);
        const std::string colorThemeLabel = ctx.localization.Get("editor", "settings.appearance.color_theme");
        const std::string colorThemeDescription = ctx.localization.Get("editor", "settings.appearance.color_theme.description");
        const std::string customThemeLabel = ctx.localization.Get("editor", "settings.appearance.custom_theme");
        const std::string customThemeDescription = ctx.localization.Get("editor", "settings.appearance.custom_theme.description");
        const std::string accentLabel = ctx.localization.Get("editor", "settings.appearance.accent_color");
        const std::string accentDescription = ctx.localization.Get("editor", "settings.appearance.accent_color.description");
        const std::string fontSizeLabel = ctx.localization.Get("editor", "settings.appearance.code_font_size");
        const std::string fontSizeDescription = ctx.localization.Get("editor", "settings.appearance.code_font_size.description");
        SettingRow(colorThemeLabel.c_str(), colorThemeDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            const auto &themeList = GetThemeList();
            static std::vector<const char *> s_names;
            s_names.clear();
            for (const auto &t : themeList)
                s_names.push_back(t.name.c_str());

            const auto count = static_cast<int>(s_names.size());
            if (st.appearance.themeIndex >= count)
                st.appearance.themeIndex = 0;

            const int prev = st.appearance.themeIndex;
            (void)ComboControl("##theme", &st.appearance.themeIndex, s_names.data(), count, ctx.theme.fonts);
            if (st.appearance.themeIndex != prev) {
                // Defer: apply at start of next frame to avoid mid-frame style glitches
                st.appearance.pendingThemeIndex = st.appearance.themeIndex;
                st.dirty = true;
            }
        });
        SettingRow(customThemeLabel.c_str(), customThemeDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##custom-theme", st.appearance.customThemePath, 128, ctx.theme.fonts);
        });
        SettingRow(accentLabel.c_str(), accentDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ColorHexControl("accent-color", st.appearance.accentHex, 16, ctx.theme.fonts);
        });
        const std::string typoGroup = ctx.localization.Get("editor", "settings.appearance.typography_group");
        SettingGroup(typoGroup.c_str(), ctx.theme.fonts);
        SettingRow(fontSizeLabel.c_str(), fontSizeDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##font-size", st.appearance.editorFontSize, 8, ctx.theme.fonts);
        });
    }

    void DrawInput(SettingsState &st, const EditorGuiContext &ctx) {
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.input");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string navGroup = ctx.localization.Get("editor", "settings.input.navigation_group");
        SettingGroup(navGroup.c_str(), ctx.theme.fonts, true);
        const std::string orbitLabel = ctx.localization.Get("editor", "settings.input.orbit_sensitivity");
        const std::string orbitDescription = ctx.localization.Get("editor", "settings.input.orbit_sensitivity.description");
        const std::string panLabel = ctx.localization.Get("editor", "settings.input.pan_sensitivity");
        const std::string panDescription = ctx.localization.Get("editor", "settings.input.pan_sensitivity.description");
        const std::string invertLabel = ctx.localization.Get("editor", "settings.input.invert_orbit_y");
        const std::string invertDescription = ctx.localization.Get("editor", "settings.input.invert_orbit_y.description");
        SettingRow(orbitLabel.c_str(), orbitDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##orbit", &st.input.orbitSensitivity, 10, 300, SliderValueFormat::Integer, ctx.theme.fonts);
        });
        SettingRow(panLabel.c_str(), panDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##pan", &st.input.panSensitivity, 10, 300, SliderValueFormat::Integer, ctx.theme.fonts);
        });
        SettingRow(invertLabel.c_str(), invertDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("invert-y", &st.input.invertOrbitY, ctx.theme.fonts);
        });

        const std::string mappingsGroup = ctx.localization.Get("editor", "settings.input.mappings_group");
        const std::string mappingsLabel = ctx.localization.Get("editor", "settings.input.mappings_label");
        const std::string mappingsDescription = ctx.localization.Get("editor", "settings.input.mappings_description");
        SettingGroup(mappingsGroup.c_str(), ctx.theme.fonts);
        SettingRow(mappingsLabel.c_str(), mappingsDescription.c_str(), ctx.theme.fonts, []() {
            // Intentionally empty: the mappings overview is read-only for now.
        });
    }

    void DrawRendering(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 4> viewportText = {ctx.localization.Get("editor", "settings.rendering.shaded"),
                                                         ctx.localization.Get("editor", "settings.rendering.wireframe"),
                                                         ctx.localization.Get("editor", "settings.rendering.lit"),
                                                         ctx.localization.Get("editor", "settings.rendering.unlit")};
        const std::array<const char *, 4> kViewport = {viewportText[0].c_str(), viewportText[1].c_str(), viewportText[2].c_str(),
                                                       viewportText[3].c_str()};
        const std::array<std::string, 4> tierText = {ctx.localization.Get("editor", "settings.rendering.high_end"),
                                                     ctx.localization.Get("editor", "settings.rendering.dx12_vulkan"),
                                                     ctx.localization.Get("editor", "settings.rendering.dx11"),
                                                     ctx.localization.Get("editor", "settings.rendering.es3")};
        const std::array<const char *, 4> kTier = {tierText[0].c_str(), tierText[1].c_str(), tierText[2].c_str(), tierText[3].c_str()};
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.rendering");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string viewportGroup = ctx.localization.Get("editor", "settings.rendering.viewport_group");
        SettingGroup(viewportGroup.c_str(), ctx.theme.fonts, true);
        const std::string viewportLabel = ctx.localization.Get("editor", "settings.rendering.viewport_mode");
        const std::string viewportDescription = ctx.localization.Get("editor", "settings.rendering.viewport_mode.description");
        const std::string gridLabel = ctx.localization.Get("editor", "settings.rendering.grid_overlay");
        const std::string gridDescription = ctx.localization.Get("editor", "settings.rendering.grid_overlay.description");
        const std::string tierLabel = ctx.localization.Get("editor", "settings.rendering.tier");
        const std::string tierDescription = ctx.localization.Get("editor", "settings.rendering.tier.description");
        const std::string budgetLabel = ctx.localization.Get("editor", "settings.rendering.texture_budget");
        const std::string budgetDescription = ctx.localization.Get("editor", "settings.rendering.texture_budget.description");
        SettingRow(viewportLabel.c_str(), viewportDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kViewport]() {
            (void)ComboControl("##viewport", &st.rendering.viewportMode, kViewport.data(), kViewport.size(), ctx.theme.fonts);
        });
        SettingRow(gridLabel.c_str(), gridDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("grid", &st.rendering.gridOverlay, ctx.theme.fonts);
        });
        const std::string qualityGroup = ctx.localization.Get("editor", "settings.rendering.quality_group");
        SettingGroup(qualityGroup.c_str(), ctx.theme.fonts);
        SettingRow(tierLabel.c_str(), tierDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kTier]() {
            (void)ComboControl("##tier", &st.rendering.renderingTier, kTier.data(), kTier.size(), ctx.theme.fonts);
        });
        SettingRow(budgetLabel.c_str(), budgetDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##texture-budget", st.rendering.textureBudget, 32, ctx.theme.fonts);
        });
    }

    void DrawAudio(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 3> deviceText = {ctx.localization.Get("editor", "settings.audio.system_default"),
                                                       ctx.localization.Get("editor", "settings.audio.headphones"),
                                                       ctx.localization.Get("editor", "settings.audio.speakers")};
        const std::array<const char *, 3> kDevices = {deviceText[0].c_str(), deviceText[1].c_str(), deviceText[2].c_str()};
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.audio");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string outputGroup = ctx.localization.Get("editor", "settings.audio.output_group");
        SettingGroup(outputGroup.c_str(), ctx.theme.fonts, true);
        const std::string volumeLabel = ctx.localization.Get("editor", "settings.audio.master_volume");
        const std::string volumeDescription = ctx.localization.Get("editor", "settings.audio.master_volume.description");
        const std::string deviceLabel = ctx.localization.Get("editor", "settings.audio.output_device");
        const std::string deviceDescription = ctx.localization.Get("editor", "settings.audio.output_device.description");
        const std::string enabledLabel = ctx.localization.Get("editor", "settings.audio.enable_in_editor");
        const std::string enabledDescription = ctx.localization.Get("editor", "settings.audio.enable_in_editor.description");
        SettingRow(volumeLabel.c_str(), volumeDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##volume", &st.audio.masterVolume, 0, 100, SliderValueFormat::Integer, ctx.theme.fonts);
        });
        SettingRow(deviceLabel.c_str(), deviceDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kDevices]() {
            (void)ComboControl("##audio-device", &st.audio.audioOutputDevice, kDevices.data(), kDevices.size(), ctx.theme.fonts);
        });
        SettingRow(enabledLabel.c_str(), enabledDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("audio-enabled", &st.audio.audioEnabled, ctx.theme.fonts);
        });
    }

    void DrawNetwork(SettingsState &st, const EditorGuiContext &ctx) {
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.network");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string multiplayerGroup = ctx.localization.Get("editor", "settings.network.multiplayer_group");
        SettingGroup(multiplayerGroup.c_str(), ctx.theme.fonts, true);
        const std::string clientsLabel = ctx.localization.Get("editor", "settings.network.max_preview_clients");
        const std::string clientsDescription = ctx.localization.Get("editor", "settings.network.max_preview_clients.description");
        const std::string latencyLabel = ctx.localization.Get("editor", "settings.network.simulate_latency");
        const std::string latencyDescription = ctx.localization.Get("editor", "settings.network.simulate_latency.description");
        SettingRow(clientsLabel.c_str(), clientsDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            InputIntControl("##max-clients", &st.network.maxPreviewClients, ctx.theme.fonts);
        });
        SettingRow(latencyLabel.c_str(), latencyDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##latency", &st.network.simulatedLatencyMs, 0,
                             static_cast<int>(Network::NetworkPreviewPreferences::MaximumSimulatedLatencyMilliseconds),
                             SliderValueFormat::Milliseconds, ctx.theme.fonts);
        });
    }

    void DrawPackages(SettingsState &st, const EditorGuiContext &ctx) {
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.packages");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string downloadGroup = ctx.localization.Get("editor", "settings.packages.download_group");
        SettingGroup(downloadGroup.c_str(), ctx.theme.fonts, true);
        const std::string threadsLabel = ctx.localization.Get("editor", "settings.packages.download_threads");
        const std::string threadsDescription = ctx.localization.Get("editor", "settings.packages.download_threads.description");
        SettingRow(threadsLabel.c_str(), threadsDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            InputIntControl("##download-threads", &st.packages.downloadThreads, ctx.theme.fonts);
        });
    }

    void DrawDiagnostics(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 4> logLevelText = {ctx.localization.Get("editor", "settings.diagnostics.debug"),
                                                         ctx.localization.Get("editor", "settings.diagnostics.info"),
                                                         ctx.localization.Get("editor", "settings.diagnostics.warning"),
                                                         ctx.localization.Get("editor", "settings.diagnostics.error")};
        const std::array<const char *, 4> kLogLevels = {logLevelText[0].c_str(), logLevelText[1].c_str(), logLevelText[2].c_str(),
                                                        logLevelText[3].c_str()};
        const std::string sectionTitle = ctx.localization.Get("editor", "settings.nav.diagnostics");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string loggingGroup = ctx.localization.Get("editor", "settings.diagnostics.logging_group");
        SettingGroup(loggingGroup.c_str(), ctx.theme.fonts, true);
        const std::string logLevelLabel = ctx.localization.Get("editor", "settings.diagnostics.log_level");
        const std::string logLevelDescription = ctx.localization.Get("editor", "settings.diagnostics.log_level.description");
        const std::string writeLogLabel = ctx.localization.Get("editor", "settings.diagnostics.write_log");
        const std::string writeLogDescription = ctx.localization.Get("editor", "settings.diagnostics.write_log.description");
        const std::string captureLabel = ctx.localization.Get("editor", "settings.diagnostics.auto_capture");
        const std::string captureDescription = ctx.localization.Get("editor", "settings.diagnostics.auto_capture.description");
        const std::string thresholdLabel = ctx.localization.Get("editor", "settings.diagnostics.stutter_threshold");
        const std::string thresholdDescription = ctx.localization.Get("editor", "settings.diagnostics.stutter_threshold.description");
        SettingRow(logLevelLabel.c_str(), logLevelDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kLogLevels]() {
            (void)ComboControl("##log-level", &st.diagnostics.consoleLogLevel, kLogLevels.data(), kLogLevels.size(), ctx.theme.fonts);
        });
        SettingRow(writeLogLabel.c_str(), writeLogDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("write-log", &st.diagnostics.writeLogToFile, ctx.theme.fonts);
        });
        const std::string profilerGroup = ctx.localization.Get("editor", "settings.diagnostics.profiler_group");
        SettingGroup(profilerGroup.c_str(), ctx.theme.fonts);
        SettingRow(captureLabel.c_str(), captureDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("capture-stutter", &st.diagnostics.autoCaptureStutter, ctx.theme.fonts);
        });
        SettingRow(thresholdLabel.c_str(), thresholdDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            InputFloatControl("##stutter", &st.diagnostics.stutterThresholdMs, ctx.theme.fonts);
        });
    }

    void DrawContent(SettingsState &st, const EditorGuiContext &ctx) {
        switch (static_cast<SettingsTab>(st.activeTab)) {
            using enum SettingsTab;
            case General:
                DrawGeneral(st, ctx);
                break;
            case Appearance:
                DrawAppearance(st, ctx);
                break;
            case Input:
                DrawInput(st, ctx);
                break;
            case Rendering:
                DrawRendering(st, ctx);
                break;
            case Audio:
                DrawAudio(st, ctx);
                break;
            case Network:
                DrawNetwork(st, ctx);
                break;
            case Packages:
                DrawPackages(st, ctx);
                break;
            case Diagnostics:
                DrawDiagnostics(st, ctx);
                break;
            case Plugins:
                DrawPlugins(st, ctx);
                break;
            default:
                break;
        }
    }

}  // namespace Horo::Editor::SettingsModalInternal
