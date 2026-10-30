#include "../extension_pages/EditorPageView.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Network/NetworkProjectSettings.h"
#include "SettingsModalInternal.h"
#include "editor/input/EditorInputActions.h"

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

    /** @copydoc DrawNavItem */
    void DrawNavItem(SettingsState &st, const NavItem &item, const EditorGuiContext &ctx) {
        const bool active = st.activeTab == static_cast<int>(item.tab);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        constexpr float rowH = 32.0F;

        ImGui::PushID(static_cast<int>(item.tab));
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

        ImGui::SetCursorScreenPos({pos.x + 12.0F, pos.y + (rowH - TextPx::Label()) * 0.5F});
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
        const std::string editor = SettingsModal::kTextScope.Text(ctx.localization, "nav.editor");
        const std::string general = SettingsModal::kTextScope.Text(ctx.localization, "nav.general");
        const std::string appearance = SettingsModal::kTextScope.Text(ctx.localization, "nav.appearance");
        const std::string input = SettingsModal::kTextScope.Text(ctx.localization, "nav.input");
        const std::string engine = SettingsModal::kTextScope.Text(ctx.localization, "nav.engine");
        const std::string rendering = SettingsModal::kTextScope.Text(ctx.localization, "nav.rendering");
        const std::string audio = SettingsModal::kTextScope.Text(ctx.localization, "nav.audio");
        const std::string network = SettingsModal::kTextScope.Text(ctx.localization, "nav.network");
        const std::string packages = SettingsModal::kTextScope.Text(ctx.localization, "nav.packages");
        const std::string updates = SettingsModal::kTextScope.Text(ctx.localization, "nav.updates");
        const std::string tools = SettingsModal::kTextScope.Text(ctx.localization, "nav.tools");
        const std::string diagnostics = SettingsModal::kTextScope.Text(ctx.localization, "nav.diagnostics");
        const std::string plugins = SettingsModal::kTextScope.Text(ctx.localization, "nav.plugins");
        DrawNavGroup(editor.c_str(), ctx);
        DrawNavItem(st, {general.c_str(), "G", General}, ctx);
        DrawNavItem(st, {appearance.c_str(), "A", Appearance}, ctx);
        DrawNavItem(st, {input.c_str(), "I", Input}, ctx);
        const auto codeEditor = SettingsModal::kTextScope.Text(ctx.localization, "nav.code_editor");
        const auto keymaps = SettingsModal::kTextScope.Text(ctx.localization, "nav.keymaps");
        DrawNavGroup(codeEditor.c_str(), ctx);
        DrawNavItem(st, {appearance.c_str(), "A", CodeEditorAppearance}, ctx);
        DrawNavItem(st, {keymaps.c_str(), "K", Keymaps}, ctx);
        DrawNavGroup(engine.c_str(), ctx);
        DrawNavItem(st, {rendering.c_str(), "R", Rendering}, ctx);
        DrawNavItem(st, {SettingsModal::kTextScope.Text(ctx.localization, "collision.title").c_str(), "C", Collision}, ctx);
        DrawNavItem(st, {audio.c_str(), "S", Audio}, ctx);
        DrawNavItem(st, {network.c_str(), "N", Network}, ctx);
        const auto navigation = SettingsModal::kTextScope.Text(ctx.localization, "nav.navigation");
        DrawNavItem(st, {navigation.c_str(), "N", Navigation}, ctx);
        DrawNavItem(st, {packages.c_str(), "K", Packages}, ctx);
        DrawNavItem(st, {updates.c_str(), "U", Updates}, ctx);
        DrawNavGroup(tools.c_str(), ctx);
        DrawNavItem(st, {diagnostics.c_str(), "D", Diagnostics}, ctx);
        DrawNavItem(st, {plugins.c_str(), "P", Plugins}, ctx);
        if (st.extensionPages) {
            const auto &pages = SettingsModal::kTextScope.Text(ctx.localization, "extension_pages.title");
            DrawNavItem(st, {pages.c_str(), "E", ExtensionPages}, ctx);
        }
    }

    void DrawGeneral(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 3> startupStr = {SettingsModal::kTextScope.Text(ctx.localization, "general.startup_welcome"),
                                                       SettingsModal::kTextScope.Text(ctx.localization, "general.startup_last"),
                                                       SettingsModal::kTextScope.Text(ctx.localization, "general.startup_browser")};
        const std::array<const char *, 3> kStartup = {startupStr[0].c_str(), startupStr[1].c_str(), startupStr[2].c_str()};
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.general");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string startupGroup = SettingsModal::kTextScope.Text(ctx.localization, "general.startup_group");
        SettingGroup(startupGroup.c_str(), ctx.theme.fonts, true);
        const std::string startupLabel = SettingsModal::kTextScope.Text(ctx.localization, "general.startup_behavior");
        const std::string startupDescription = SettingsModal::kTextScope.Text(ctx.localization, "general.startup_behavior.description");
        const std::string autosaveLabel = SettingsModal::kTextScope.Text(ctx.localization, "general.autosave_interval");
        const std::string autosaveDescription = SettingsModal::kTextScope.Text(ctx.localization, "general.autosave_interval.description");
        const std::string confirmLabel = SettingsModal::kTextScope.Text(ctx.localization, "general.confirm_exit");
        const std::string confirmDescription = SettingsModal::kTextScope.Text(ctx.localization, "general.confirm_exit.description");
        const std::string restoreLabel = SettingsModal::kTextScope.Text(ctx.localization, "general.restore_workspace");
        const std::string restoreDescription = SettingsModal::kTextScope.Text(ctx.localization, "general.restore_workspace.description");
        const std::string defaultSceneLabel = SettingsModal::kTextScope.Text(ctx.localization, "general.default_scene");
        const std::string defaultSceneDescription = SettingsModal::kTextScope.Text(ctx.localization, "general.default_scene.description");
        SettingRow(startupLabel.c_str(), startupDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kStartup]() {
            (void)ComboControl("##startup", &st.general.startupAction, kStartup.data(), kStartup.size(), ctx.theme.fonts);
        });
        SettingRow(autosaveLabel.c_str(), autosaveDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##autosave", &st.general.autoSaveInterval, 0, 30, SliderValueFormat::Minutes, ctx.theme.fonts);
        });
        SettingRow(confirmLabel.c_str(), confirmDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("confirm-exit", &st.general.confirmExit, ctx.theme.fonts);
        });
        const std::string sessionGroup = SettingsModal::kTextScope.Text(ctx.localization, "general.session_group");
        SettingGroup(sessionGroup.c_str(), ctx.theme.fonts);
        SettingRow(restoreLabel.c_str(), restoreDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("restore-workspace", &st.general.restoreWorkspace, ctx.theme.fonts);
        });
        SettingRow(defaultSceneLabel.c_str(), defaultSceneDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##default-scene", st.general.defaultScene, 64, ctx.theme.fonts);
        });
        const std::string english = SettingsModal::kTextScope.Text(ctx.localization, "language.english");
        const std::string turkish = SettingsModal::kTextScope.Text(ctx.localization, "language.turkish");
        const std::array<const char *, 2> kLanguages = {english.c_str(), turkish.c_str()};
        const std::string languageLabel = SettingsModal::kTextScope.Text(ctx.localization, "language");
        const std::string languageDescription = SettingsModal::kTextScope.Text(ctx.localization, "language.description");
        SettingRow(languageLabel.c_str(), languageDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kLanguages]() {
            int languageIndex = st.general.languageTag == "tr-TR" ? 1 : 0;
            if (ComboControl("##language", &languageIndex, kLanguages.data(), kLanguages.size(), ctx.theme.fonts)) {
                st.general.languageTag = languageIndex == 1 ? "tr-TR" : "en-US";
                st.dirty = true;
            }
        });
    }

    void DrawAppearance(SettingsState &st, const EditorGuiContext &ctx) {
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.appearance");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string themeGroup = SettingsModal::kTextScope.Text(ctx.localization, "appearance.theme_group");
        SettingGroup(themeGroup.c_str(), ctx.theme.fonts, true);
        const std::string colorThemeLabel = SettingsModal::kTextScope.Text(ctx.localization, "appearance.color_theme");
        const std::string colorThemeDescription = SettingsModal::kTextScope.Text(ctx.localization, "appearance.color_theme.description");
        const std::string customThemeLabel = SettingsModal::kTextScope.Text(ctx.localization, "appearance.custom_theme");
        const std::string customThemeDescription = SettingsModal::kTextScope.Text(ctx.localization, "appearance.custom_theme.description");
        const std::string accentLabel = SettingsModal::kTextScope.Text(ctx.localization, "appearance.accent_color");
        const std::string accentDescription = SettingsModal::kTextScope.Text(ctx.localization, "appearance.accent_color.description");
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
                st.appearance.themeId = themeList[static_cast<std::size_t>(st.appearance.themeIndex)].id;
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
    }

    /** @brief Presents source editor appearance using the same committed settings authority as the workspace. */
    void DrawCodeEditorAppearance(SettingsState &st, const EditorGuiContext &ctx) {
        const auto title = SettingsModal::kTextScope.Text(ctx.localization, "nav.code_editor");
        SectionTitle(title.c_str(), ctx.theme.fonts);
        const auto fontSizeLabel = SettingsModal::kTextScope.Text(ctx.localization, "appearance.code_font_size");
        const auto fontSizeDescription = SettingsModal::kTextScope.Text(ctx.localization, "appearance.code_font_size.description");
        const std::string typoGroup = SettingsModal::kTextScope.Text(ctx.localization, "appearance.typography_group");
        SettingGroup(typoGroup.c_str(), ctx.theme.fonts, true);
        const auto familyLabel = SettingsModal::kTextScope.Text(ctx.localization, "code_editor.font_family");
        const auto familyDescription = SettingsModal::kTextScope.Text(ctx.localization, "code_editor.font_family.description");
        SettingRow(familyLabel.c_str(), familyDescription.c_str(), ctx.theme.fonts, [&] {
            const auto &families = ctx.theme.fonts.codeFamilies;
            int selected = 0;
            for (std::size_t index = 0; index < families.size(); ++index)
                if (families[index].name == st.appearance.codeFontFamily)
                    selected = static_cast<int>(index);
            const ComboItemSource source{.label = [&](int index) {
                return families[static_cast<std::size_t>(index)].name.c_str();
            }};
            if (!families.empty() && ComboControl("##code-family", &selected, static_cast<int>(families.size()), source, ctx.theme.fonts))
                st.appearance.codeFontFamily = families[static_cast<std::size_t>(selected)].name;
        });
        SettingRow(fontSizeLabel.c_str(), fontSizeDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##font-size", st.appearance.editorFontSize, 8, ctx.theme.fonts);
        });
        const auto behavior = SettingsModal::kTextScope.Text(ctx.localization, "code_editor.behavior");
        SettingGroup(behavior.c_str(), ctx.theme.fonts);
        for (const auto &entry : CodeEditorBooleanSettings()) {
            const std::string key = std::string{"code_editor."} + entry.key;
            const auto label = SettingsModal::kTextScope.Text(ctx.localization, key);
            const auto description = SettingsModal::kTextScope.Text(ctx.localization, key + ".description");
            SettingRow(label.c_str(), description.c_str(), ctx.theme.fonts, [&] {
                static_cast<void>(ToggleControl(entry.key, &(st.appearance.codeEditor.*entry.member), ctx.theme.fonts));
            });
        }
        for (const auto &entry : CodeEditorIntegerSettings()) {
            const std::string key = std::string{"code_editor."} + entry.key;
            const auto label = SettingsModal::kTextScope.Text(ctx.localization, key);
            const auto description = SettingsModal::kTextScope.Text(ctx.localization, key + ".description");
            SettingRow(label.c_str(), description.c_str(), ctx.theme.fonts, [&] {
                SliderIntControl(entry.key, &(st.appearance.codeEditor.*entry.member), entry.minimum, entry.maximum,
                                 SliderValueFormat::Integer, ctx.theme.fonts);
            });
        }
    }

    void DrawInput(SettingsState &st, const EditorGuiContext &ctx) {
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.input");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string navGroup = SettingsModal::kTextScope.Text(ctx.localization, "input.navigation_group");
        SettingGroup(navGroup.c_str(), ctx.theme.fonts, true);
        const std::string orbitLabel = SettingsModal::kTextScope.Text(ctx.localization, "input.orbit_sensitivity");
        const std::string orbitDescription = SettingsModal::kTextScope.Text(ctx.localization, "input.orbit_sensitivity.description");
        const std::string panLabel = SettingsModal::kTextScope.Text(ctx.localization, "input.pan_sensitivity");
        const std::string panDescription = SettingsModal::kTextScope.Text(ctx.localization, "input.pan_sensitivity.description");
        const std::string invertLabel = SettingsModal::kTextScope.Text(ctx.localization, "input.invert_orbit_y");
        const std::string invertDescription = SettingsModal::kTextScope.Text(ctx.localization, "input.invert_orbit_y.description");
        SettingRow(orbitLabel.c_str(), orbitDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##orbit", &st.input.orbitSensitivity, 10, 300, SliderValueFormat::Integer, ctx.theme.fonts);
        });
        SettingRow(panLabel.c_str(), panDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            SliderIntControl("##pan", &st.input.panSensitivity, 10, 300, SliderValueFormat::Integer, ctx.theme.fonts);
        });
        SettingRow(invertLabel.c_str(), invertDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("invert-y", &st.input.invertOrbitY, ctx.theme.fonts);
        });
    }

    /** @brief Edits registered commands without creating a second shortcut store. */
    void DrawKeymaps(SettingsState &st, const EditorGuiContext &ctx) {
        const auto title = SettingsModal::kTextScope.Text(ctx.localization, "nav.keymaps");
        SectionTitle(title.c_str(), ctx.theme.fonts);
        const std::string mappingsGroup = SettingsModal::kTextScope.Text(ctx.localization, "input.mappings_group");
        const std::string mappingsDescription = SettingsModal::kTextScope.Text(ctx.localization, "input.mappings_description");
        SettingGroup(mappingsGroup.c_str(), ctx.theme.fonts);
        Ui::Hint(mappingsDescription.c_str(), ctx.theme.fonts);
        auto &profile = st.input.keymap.profile;
        std::vector<Input::ActionDescriptor> contributions;
        if (st.extensionCommands)
            for (const auto &entry : st.extensionCommands->PreparedCommands()) {
                std::vector<Input::InputBinding> defaults;
                if (const auto binding = ParseEditorShortcut(entry.command.shortcut))
                    defaults.push_back(*binding);
                contributions.push_back({Input::ActionId{entry.command.id.value}, Input::ActionValueType::Digital,
                                         Input::InputContextId{kEditorWorkspaceInputContext}, false, std::move(defaults)});
            }
        const auto actions = EditorKeymapActions(profile, contributions);
        for (const auto &action : actions) {
            if (action.valueType != Input::ActionValueType::Digital)
                continue;
            const auto &id = action.id.Value();
            ImGui::PushID(id.c_str());
            std::string label = id.starts_with("editor.") ? ctx.localization.Get("editor", "settings.keymap." + id) : id;
            if (st.extensionCommands)
                for (const auto &entry : st.extensionCommands->PreparedCommands())
                    if (entry.command.id.value == id)
                        label = st.extensionCommands->LocalizedText(entry.provider, entry.command.labelLocalizationKey,
                                                                    ctx.settings.settings.languageTag);
            auto bindings = std::span<const Input::InputBinding>{action.defaultBindings};
            for (const auto &entry : profile.overrides)
                if (entry.action == action.id)
                    bindings = entry.bindings;
            std::string chord;
            if (!bindings.empty()) {
                const auto preferred = std::ranges::find_if(bindings, [](const auto &binding) {
                    return ImGui::GetIO().ConfigMacOSXBehaviors ? binding.requiredModifiers.command : binding.requiredModifiers.control;
                });
                chord = Ui::ShortcutLabel(preferred != bindings.end() ? *preferred : bindings.front());
            }
            const bool listening = st.input.recordingAction == id;
            std::string button = chord;
            if (listening)
                button = SettingsModal::kTextScope.Text(ctx.localization, "keymap.recording");
            else if (button.empty())
                button = SettingsModal::kTextScope.Text(ctx.localization, "keymap.unassigned");
            SettingRow(label.c_str(), "", ctx.theme.fonts, [&] {
                if (Ui::Button({.label = button.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = ctx.theme.fonts.sans}))
                    st.input.recordingAction = id;
                ImGui::SameLine();
                if (Ui::Button({.label = SettingsModal::kTextScope.Text(ctx.localization, "keymap.reset").c_str(),
                                .variant = Ui::ButtonVariant::Secondary,
                                .font = ctx.theme.fonts.sans})) {
                    std::erase_if(profile.overrides, [&](const auto &entry) {
                        return entry.action == action.id;
                    });
                    st.input.recordingAction.clear();
                }
            });
            if (listening) {
                Input::InputBinding captured;
                if (Ui::CaptureShortcut(captured)) {
                    if (captured.key != Input::Key::Escape) {
                        std::erase_if(profile.overrides, [&](const auto &entry) {
                            return entry.action == action.id;
                        });
                        profile.overrides.push_back({action.id, {captured}});
                    }
                    st.input.recordingAction.clear();
                }
            }
            ImGui::PopID();
        }
        const auto report = Input::ValidateBindingProfile(actions, profile);
        if (!report.IsValid())
            Ui::ErrorText(SettingsModal::kTextScope.Text(ctx.localization, "keymap.conflict").c_str(), ctx.theme.fonts);
    }

    void DrawRendering(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 4> viewportText = {SettingsModal::kTextScope.Text(ctx.localization, "rendering.shaded"),
                                                         SettingsModal::kTextScope.Text(ctx.localization, "rendering.wireframe"),
                                                         SettingsModal::kTextScope.Text(ctx.localization, "rendering.lit"),
                                                         SettingsModal::kTextScope.Text(ctx.localization, "rendering.unlit")};
        const std::array<const char *, 4> kViewport = {viewportText[0].c_str(), viewportText[1].c_str(), viewportText[2].c_str(),
                                                       viewportText[3].c_str()};
        const std::array<std::string, 4> tierText = {SettingsModal::kTextScope.Text(ctx.localization, "rendering.high_end"),
                                                     SettingsModal::kTextScope.Text(ctx.localization, "rendering.dx12_vulkan"),
                                                     SettingsModal::kTextScope.Text(ctx.localization, "rendering.dx11"),
                                                     SettingsModal::kTextScope.Text(ctx.localization, "rendering.es3")};
        const std::array<const char *, 4> kTier = {tierText[0].c_str(), tierText[1].c_str(), tierText[2].c_str(), tierText[3].c_str()};
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.rendering");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string viewportGroup = SettingsModal::kTextScope.Text(ctx.localization, "rendering.viewport_group");
        SettingGroup(viewportGroup.c_str(), ctx.theme.fonts, true);
        const std::string viewportLabel = SettingsModal::kTextScope.Text(ctx.localization, "rendering.viewport_mode");
        const std::string viewportDescription = SettingsModal::kTextScope.Text(ctx.localization, "rendering.viewport_mode.description");
        const std::string gridLabel = SettingsModal::kTextScope.Text(ctx.localization, "rendering.grid_overlay");
        const std::string gridDescription = SettingsModal::kTextScope.Text(ctx.localization, "rendering.grid_overlay.description");
        const std::string tierLabel = SettingsModal::kTextScope.Text(ctx.localization, "rendering.tier");
        const std::string tierDescription = SettingsModal::kTextScope.Text(ctx.localization, "rendering.tier.description");
        const std::string budgetLabel = SettingsModal::kTextScope.Text(ctx.localization, "rendering.texture_budget");
        const std::string budgetDescription = SettingsModal::kTextScope.Text(ctx.localization, "rendering.texture_budget.description");
        SettingRow(viewportLabel.c_str(), viewportDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kViewport]() {
            (void)ComboControl("##viewport", &st.rendering.viewportMode, kViewport.data(), kViewport.size(), ctx.theme.fonts);
        });
        SettingRow(gridLabel.c_str(), gridDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("grid", &st.rendering.gridOverlay, ctx.theme.fonts);
        });
        const std::string qualityGroup = SettingsModal::kTextScope.Text(ctx.localization, "rendering.quality_group");
        SettingGroup(qualityGroup.c_str(), ctx.theme.fonts);
        SettingRow(tierLabel.c_str(), tierDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kTier]() {
            (void)ComboControl("##tier", &st.rendering.renderingTier, kTier.data(), kTier.size(), ctx.theme.fonts);
        });
        SettingRow(budgetLabel.c_str(), budgetDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)InputTextControl("##texture-budget", st.rendering.textureBudget, 32, ctx.theme.fonts);
        });
    }

    void DrawAudio(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 3> deviceText = {SettingsModal::kTextScope.Text(ctx.localization, "audio.system_default"),
                                                       SettingsModal::kTextScope.Text(ctx.localization, "audio.headphones"),
                                                       SettingsModal::kTextScope.Text(ctx.localization, "audio.speakers")};
        const std::array<const char *, 3> kDevices = {deviceText[0].c_str(), deviceText[1].c_str(), deviceText[2].c_str()};
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.audio");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string outputGroup = SettingsModal::kTextScope.Text(ctx.localization, "audio.output_group");
        SettingGroup(outputGroup.c_str(), ctx.theme.fonts, true);
        const std::string volumeLabel = SettingsModal::kTextScope.Text(ctx.localization, "audio.master_volume");
        const std::string volumeDescription = SettingsModal::kTextScope.Text(ctx.localization, "audio.master_volume.description");
        const std::string deviceLabel = SettingsModal::kTextScope.Text(ctx.localization, "audio.output_device");
        const std::string deviceDescription = SettingsModal::kTextScope.Text(ctx.localization, "audio.output_device.description");
        const std::string enabledLabel = SettingsModal::kTextScope.Text(ctx.localization, "audio.enable_in_editor");
        const std::string enabledDescription = SettingsModal::kTextScope.Text(ctx.localization, "audio.enable_in_editor.description");
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
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.network");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string multiplayerGroup = SettingsModal::kTextScope.Text(ctx.localization, "network.multiplayer_group");
        SettingGroup(multiplayerGroup.c_str(), ctx.theme.fonts, true);
        const std::string clientsLabel = SettingsModal::kTextScope.Text(ctx.localization, "network.max_preview_clients");
        const std::string clientsDescription = SettingsModal::kTextScope.Text(ctx.localization, "network.max_preview_clients.description");
        const std::string latencyLabel = SettingsModal::kTextScope.Text(ctx.localization, "network.simulate_latency");
        const std::string latencyDescription = SettingsModal::kTextScope.Text(ctx.localization, "network.simulate_latency.description");
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
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.packages");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string downloadGroup = SettingsModal::kTextScope.Text(ctx.localization, "packages.download_group");
        SettingGroup(downloadGroup.c_str(), ctx.theme.fonts, true);
        const std::string threadsLabel = SettingsModal::kTextScope.Text(ctx.localization, "packages.download_threads");
        const std::string threadsDescription = SettingsModal::kTextScope.Text(ctx.localization, "packages.download_threads.description");
        SettingRow(threadsLabel.c_str(), threadsDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            InputIntControl("##download-threads", &st.packages.downloadThreads, ctx.theme.fonts);
        });
    }

    void DrawDiagnostics(SettingsState &st, const EditorGuiContext &ctx) {
        const std::array<std::string, 4> logLevelText = {SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.debug"),
                                                         SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.info"),
                                                         SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.warning"),
                                                         SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.error")};
        const std::array<const char *, 4> kLogLevels = {logLevelText[0].c_str(), logLevelText[1].c_str(), logLevelText[2].c_str(),
                                                        logLevelText[3].c_str()};
        const std::string sectionTitle = SettingsModal::kTextScope.Text(ctx.localization, "nav.diagnostics");
        SectionTitle(sectionTitle.c_str(), ctx.theme.fonts);
        const std::string loggingGroup = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.logging_group");
        SettingGroup(loggingGroup.c_str(), ctx.theme.fonts, true);
        const std::string logLevelLabel = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.log_level");
        const std::string logLevelDescription = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.log_level.description");
        const std::string writeLogLabel = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.write_log");
        const std::string writeLogDescription = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.write_log.description");
        const std::string captureLabel = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.auto_capture");
        const std::string captureDescription = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.auto_capture.description");
        const std::string thresholdLabel = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.stutter_threshold");
        const std::string thresholdDescription =
            SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.stutter_threshold.description");
        SettingRow(logLevelLabel.c_str(), logLevelDescription.c_str(), ctx.theme.fonts, [&st, &ctx, kLogLevels]() {
            (void)ComboControl("##log-level", &st.diagnostics.consoleLogLevel, kLogLevels.data(), kLogLevels.size(), ctx.theme.fonts);
        });
        SettingRow(writeLogLabel.c_str(), writeLogDescription.c_str(), ctx.theme.fonts, [&st, &ctx]() {
            (void)ToggleControl("write-log", &st.diagnostics.writeLogToFile, ctx.theme.fonts);
        });
        const std::string profilerGroup = SettingsModal::kTextScope.Text(ctx.localization, "diagnostics.profiler_group");
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
            case CodeEditorAppearance:
                DrawCodeEditorAppearance(st, ctx);
                break;
            case Keymaps:
                DrawKeymaps(st, ctx);
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
            case Navigation:
                DrawNavigationSettings(st, ctx);
                break;
            case Packages:
                DrawPackages(st, ctx);
                break;
            case Updates:
                DrawUpdates(st, ctx);
                break;
            case Diagnostics:
                DrawDiagnostics(st, ctx);
                break;
            case Collision:
                DrawCollision(st, ctx);
                break;
            case ExtensionPages:
                if (st.extensionPages)
                    st.extensionPages->DrawSettings();
                break;
            case Plugins:
                DrawPlugins(st, ctx);
                break;
            default:
                break;
        }
    }

}  // namespace Horo::Editor::SettingsModalInternal
