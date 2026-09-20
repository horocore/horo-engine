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

    template <typename DrawControl>
    void PluginSettingRow(const char *label, const char *description, const EditorGuiContext &ctx, DrawControl drawControl) {
        const float rowW = ImGui::GetContentRegionAvail().x;
        const float startY = ImGui::GetCursorScreenPos().y;

        {
            ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Label(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Text());
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
        }

        if (description != nullptr && description[0] != '\0') {
            ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowW);
            ImGui::TextWrapped("%s", description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::Dummy({0.0F, 6.0F});
        drawControl();
        ImGui::Dummy({0.0F, 8.0F});

        const ImVec2 sep = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine({sep.x, sep.y}, {sep.x + rowW, sep.y}, U32(Border()), 1.0F);
        ImGui::Dummy({0.0F, 10.0F});

        if (ImGui::GetCursorScreenPos().y - startY < 58.0F)
            ImGui::Dummy({0.0F, 58.0F - (ImGui::GetCursorScreenPos().y - startY)});
    }

    void DrawToggleState(const char *id, bool *value, const EditorGuiContext &ctx) {
        (void)ToggleControl(id, value, ctx.theme.fonts, false);
        ImGui::SameLine(0.0F, 8.0F);
        ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
        ImGui::PushStyleColor(ImGuiCol_Text, *value ? Text() : Muted());
        const std::string enabledText = ctx.localization.Get("editor", "settings.plugins.status.enabled");
        const std::string disabledText = ctx.localization.Get("editor", "settings.plugins.status.disabled");
        ImGui::TextUnformatted(*value ? enabledText.c_str() : disabledText.c_str());
        ImGui::PopStyleColor();
    }

    void DrawPluginDetailPanelPrimaryAction(SettingsState &st) {
        switch (st.selectedPlugin) {
            case 0:
                st.modalFeedback = "Opening Horo MCP Bridge logs...";
                break;
            case 1:
                st.modalFeedback = "FMOD integration validated successfully.";
                break;
            case 2:
                st.plugins.steamworksSdk = true;
                break;
            default:
                break;
        }
    }

    void DrawPluginDetailPanelSecondaryAction(SettingsState &st) {
        switch (st.selectedPlugin) {
            case 0:
                st.plugins.horoMcpBridge = false;
                break;
            case 1:
                st.plugins.fmodIntegration = false;
                break;
            case 2:
                st.modalFeedback = "Opening Steamworks SDK documentation...";
                break;
            default:
                break;
        }
    }

    void DrawPluginDetailHeaderActions(SettingsState &st, const PluginDetailHeaderSpec &hdr, const ImVec2 position, const float headerW,
                                       const float headerH) {
        if (headerW < 520.0F)
            return;

        constexpr float actionW = 88.0F;
        constexpr float actionGap = 6.0F;
        ImGui::SetCursorScreenPos({position.x + headerW - actionW * 2.0F - actionGap - 14.0F, position.y + headerH - 36.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{8.0F, 4.0F});
        ImGui::PushStyleColor(ImGuiCol_Button, Bg1());
        if (ImGui::Button(hdr.action1, {actionW, 28.0F}))
            DrawPluginDetailPanelPrimaryAction(st);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0F, actionGap);
        const bool danger = (st.selectedPlugin == 0 || st.selectedPlugin == 1);
        if (danger)
            ImGui::PushStyleColor(ImGuiCol_Text, Err());
        if (ImGui::Button(hdr.action2, {actionW, 28.0F}))
            DrawPluginDetailPanelSecondaryAction(st);
        if (danger)
            ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void DrawPluginDetailHeaderCard(SettingsState &st, const EditorGuiContext &ctx, const PluginDetailHeaderSpec &hdr) {
        const float headerW = ImGui::GetContentRegionAvail().x;
        constexpr float headerH = 126.0F;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, {p.x + headerW, p.y + headerH}, U32(Bg3()), Layout::Radius);
        dl->AddRect(p, {p.x + headerW, p.y + headerH}, U32(Border()), Layout::Radius);

        const bool selectedEnabled = !(st.selectedPlugin == 2 && !st.plugins.steamworksSdk);
        const ImVec2 dotCenter{p.x + 18.0F, p.y + 20.0F};
        dl->AddCircleFilled(dotCenter, 4.5F, selectedEnabled ? U32(Ok()) : U32(Dim()));
        if (selectedEnabled)
            dl->AddCircleFilled(dotCenter, 7.0F, ImColor{Ok().x, Ok().y, Ok().z, 0.13F});

        ImGui::SetCursorScreenPos({p.x + 34.0F, p.y + 12.0F});
        {
            ScopedTextStyle ts(ctx.theme.fonts.sansCompact, TextPx::Label(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Text());
            ImGui::TextUnformatted(hdr.name);
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos({p.x + 20.0F, p.y + 40.0F});
        {
            ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, Muted());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + headerW - 44.0F);
            ImGui::TextWrapped("%s", hdr.desc);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos({p.x + 20.0F, p.y + 94.0F});
        Badge({.label = hdr.scopeBadge, .tone = BadgeTone::Accent}, ctx.theme.fonts);
        Badge({.label = hdr.signedBadge, .tone = hdr.signedTone}, ctx.theme.fonts);
        {
            ScopedTextStyle ts(ctx.theme.fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Dim());
            ImGui::TextUnformatted(hdr.restartBadge);
            ImGui::PopStyleColor();
        }

        DrawPluginDetailHeaderActions(st, hdr, p, headerW, headerH);

        ImGui::SetCursorScreenPos({p.x, p.y + headerH + 12.0F});
    }

    void DrawPluginDetailTabs(int &activeTab, const EditorGuiContext &ctx) {
        static constexpr std::array kDetailTabs = {"Settings", "Permissions", "Diagnostics", "Manifest"};
        const float tabAvail = ImGui::GetContentRegionAvail().x;
        const float tabGap = 4.0F;
        const float tabW = (tabAvail - tabGap * 3.0F) / 4.0F;
        constexpr float tabH = 34.0F;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.0F, 0.0F});
        for (int i = 0; i < 4; ++i) {
            if (i > 0)
                ImGui::SameLine(0.0F, tabGap);
            const bool active = activeTab == i;
            ImGui::PushID(i + 200);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Layout::Radius);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4{Accent().x, Accent().y, Accent().z, 0.09F} : Bg2());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hover());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{Accent().x, Accent().y, Accent().z, 0.16F});
            ImGui::PushStyleColor(ImGuiCol_Text, active ? Accent() : Muted());
            {
                ScopedTextStyle ts(ctx.theme.fonts.sans, TextPx::Caption(), FontPx::Sans);
                if (ImGui::Button(kDetailTabs[i], {tabW, tabH}))
                    activeTab = i;
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::PopID();

            if (active)
                DrawActiveTabIndicator();
        }
        ImGui::PopStyleVar();
    }

    void DrawMcpDetailContent(SettingsState &st, const EditorGuiContext &ctx, int activeTab);
    void DrawFmodDetailContent(SettingsState &st, const EditorGuiContext &ctx, int activeTab);
    void DrawSteamDetailContent(SettingsState &st, const EditorGuiContext &ctx, int activeTab);

    void DrawPluginDetailContentForSelection(SettingsState &st, const EditorGuiContext &ctx, const int activeTab) {
        switch (st.selectedPlugin) {
            case 0:
                DrawMcpDetailContent(st, ctx, activeTab);
                break;
            case 1:
                DrawFmodDetailContent(st, ctx, activeTab);
                break;
            case 2:
                DrawSteamDetailContent(st, ctx, activeTab);
                break;
            default:
                break;
        }
    }

    void DrawEmptyPluginDetailPanel(const float w, const bool embedded) {
        if (!embedded) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
            ImGui::BeginChild("PluginDetail", {w, 0.0F}, true, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Dim());
        ImGui::TextUnformatted("Select a plugin from the list.");
        ImGui::PopStyleColor();
        if (!embedded) {
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }

    void DrawPluginDetailPanelBody(SettingsState &st, const EditorGuiContext &ctx, const float w, const bool embedded,
                                   const PluginDetailHeaderSpec &hdr, int &activeTab) {
        static int s_lastSelectedPlugin = -1;
        static int s_lastPluginTab = -1;
        if (!embedded) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
            ImGui::BeginChild("PluginDetail", {w, 0.0F}, true,
                              ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        }

        DrawPluginDetailHeaderCard(st, ctx, hdr);
        DrawPluginDetailTabs(activeTab, ctx);
        ImGui::Dummy({0.0F, 10.0F});
        if (const bool contentChanged = s_lastSelectedPlugin != st.selectedPlugin || s_lastPluginTab != activeTab;
            contentChanged && !embedded)
            ImGui::SetScrollHereY(0.0F);
        DrawPluginDetailContentForSelection(st, ctx, activeTab);
        s_lastSelectedPlugin = st.selectedPlugin;
        s_lastPluginTab = activeTab;

        if (!embedded) {
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }

    void DrawPluginDetailPanel(SettingsState &st, const EditorGuiContext &ctx, float w, bool embedded) {  // NOSONAR(cpp:S1144)
        if (st.selectedPlugin < 0 || st.selectedPlugin > 2) {
            DrawEmptyPluginDetailPanel(w, embedded);
            return;
        }

        const std::string mcpDDesc = ctx.localization.Get("editor", "settings.plugins.mcp.detail_desc");
        const std::string mcpScope = ctx.localization.Get("editor", "settings.plugins.scope.editor");
        const std::string mcpSigned = ctx.localization.Get("editor", "settings.plugins.signed.signed");
        const std::string mcpRestart = ctx.localization.Get("editor", "settings.plugins.restart.not_required");
        const std::string openLogs = ctx.localization.Get("editor", "settings.plugins.action.open_logs");
        const std::string disableStr = ctx.localization.Get("editor", "settings.plugins.action.disable");
        const std::string fmodDDesc = ctx.localization.Get("editor", "settings.plugins.fmod.detail_desc");
        const std::string fmodScope = ctx.localization.Get("editor", "settings.plugins.scope.project");
        const std::string fmodSigned = ctx.localization.Get("editor", "settings.plugins.signed.vendor");
        const std::string fmodRestart = ctx.localization.Get("editor", "settings.plugins.restart.needs_sdk");
        const std::string validateStr = ctx.localization.Get("editor", "settings.plugins.action.validate");
        const std::string steamDDesc = ctx.localization.Get("editor", "settings.plugins.steam.detail_desc");
        const std::string steamScope = ctx.localization.Get("editor", "settings.plugins.scope.platform");
        const std::string steamSigned = ctx.localization.Get("editor", "settings.plugins.signed.disabled");
        const std::string steamRestart = ctx.localization.Get("editor", "settings.plugins.restart.on_enable");
        const std::string enableStr = ctx.localization.Get("editor", "settings.plugins.action.enable");
        const std::string openDocs = ctx.localization.Get("editor", "settings.plugins.action.open_docs");
        const std::array<PluginDetailHeaderSpec, 3> kDetailHeaders = {{
            {"Horo MCP Bridge", mcpDDesc.c_str(), mcpScope.c_str(), mcpSigned.c_str(), BadgeTone::Success, mcpRestart.c_str(),
             openLogs.c_str(), disableStr.c_str()},
            {"Vendor FMOD Integration", fmodDDesc.c_str(), fmodScope.c_str(), fmodSigned.c_str(), BadgeTone::Success, fmodRestart.c_str(),
             validateStr.c_str(), disableStr.c_str()},
            {"Steamworks SDK", steamDDesc.c_str(), steamScope.c_str(), steamSigned.c_str(), BadgeTone::Warning, steamRestart.c_str(),
             enableStr.c_str(), openDocs.c_str()},
        }};
        const auto &hdr = kDetailHeaders[st.selectedPlugin];
        int &activeTab = st.pluginDetailTab[st.selectedPlugin];
        if (activeTab < 0 || activeTab > 3)
            activeTab = 0;
        DrawPluginDetailPanelBody(st, ctx, w, embedded, hdr, activeTab);
    }

    void DrawMcpSettings(SettingsState &st, const EditorGuiContext &ctx) {
        SettingGroup("CONNECTION", ctx.theme.fonts, true);
        PluginSettingRow("Transport Mode", "Use stdio for local tools; HTTP is useful for explicit local integrations.", ctx,
                         [&st, &ctx]() {
            static constexpr std::array kModes = {"Local HTTP", "stdio", "Named Pipe"};
            (void)ComboControl("##transport", &st.mcp.transportMode, kModes.data(), 3, ctx.theme.fonts);
        });
        PluginSettingRow("MCP Port", "Bound to localhost unless remote access is enabled.", ctx, [&st, &ctx]() {
            InputIntControl("##mcp-port", &st.mcp.port, ctx.theme.fonts);
        });
        PluginSettingRow("Require Session Token", "Reject tool calls unless they include the generated editor session token.", ctx,
                         [&st, &ctx]() {
            DrawToggleState("##token", &st.mcp.requireToken, ctx);
        });
        PluginSettingRow("Allow Remote Connections", "Off by default to avoid accidental LAN exposure.", ctx, [&st, &ctx]() {
            DrawToggleState("##remote", &st.mcp.allowRemote, ctx);
        });
        SettingGroup("TOOL SCOPE", ctx.theme.fonts);
        PluginSettingRow("Allowed Tool Groups", "Restrict what external tools can invoke.", ctx, [&st, &ctx]() {
            static constexpr std::array kScopes = {"Read + Safe Mutations", "Read Only", "Full Project Access", "Custom Policy..."};
            (void)ComboControl("##scope", &st.mcp.toolScope, kScopes.data(), 4, ctx.theme.fonts);
        });
        PluginSettingRow("Asset Write Root", "All generated assets must stay under this folder.", ctx, [&st, &ctx]() {
            (void)InputTextControl("##root", st.mcp.assetRoot, 64, ctx.theme.fonts);
        });
    }

    void DrawMcpDetailContent(SettingsState &st, const EditorGuiContext &ctx, const int activeTab) {
        using enum Horo::Editor::Ui::BadgeTone;
        switch (activeTab) {
            case 0:
                DrawMcpSettings(st, ctx);
                break;

            case 1: {
                static const std::array<PermissionRowSpec, 3> kPerms = {{
                    {"✓", "Read project metadata", "Read project name, scene list, package graph, and editor state.", "Allowed", Success},
                    {"✓", "Write generated assets", "Create files only under Assets/Generated unless policy is elevated.", "Scoped",
                     Success},
                    {"!", "Execute build commands", "Requires interactive confirmation before running build or release tasks.", "Confirm",
                     Warning},
                }};
                DrawPermissionRows(kPerms, ctx);
            } break;

            case 2: {
                static const std::array<DiagnosticMetricSpec, 3> kMetrics = {{
                    {"STATUS", "Running", "sandboxed", Ok()},
                    {"LAST CALL", "2m ago", "tool request", Text()},
                    {"ERRORS", "0", "last 24h", Ok()},
                }};
                DrawDiagnosticMetrics(kMetrics, ctx);
                const std::array kActivity = {"14:22  project.read completed in 18ms",
                                              "14:20  assets.write.scoped created /Assets/Generated/mesh.json",
                                              "14:16  command.run requested confirmation"};
                DrawDiagnosticActivity(kActivity, ctx);
            } break;

            case 3:
                DrawManifestBlock("plugins/mcp-bridge/plugin.yaml",
                                  "id: horo.mcp.bridge\n"
                                  "version: 0.4.0\n"
                                  "entry: plugins/mcp-bridge/bin/horo-mcp\n"
                                  "scope: editor\n"
                                  "permissions:\n"
                                  "  - project.read\n"
                                  "  - assets.write.scoped\n"
                                  "  - commands.run.confirmed",
                                  ctx);
                break;
            default:
                break;
        }
    }

    void DrawFmodSettings(SettingsState &st, const EditorGuiContext &ctx) {
        SettingGroup("AUTHORING", ctx.theme.fonts, true);
        PluginSettingRow("FMOD Studio Path", "Used to open projects and compile banks from the editor.", ctx, [&st, &ctx]() {
            (void)InputTextControl("##fmod-path", st.fmod.studioPath, 128, ctx.theme.fonts);
        });
        PluginSettingRow("FMOD Project File", "Relative to project root.", ctx, [&st, &ctx]() {
            (void)InputTextControl("##fmod-proj", st.fmod.projectFile, 64, ctx.theme.fonts);
        });
        PluginSettingRow("Bank Output Path", "Compiled banks copied into the runtime asset tree.", ctx, [&st, &ctx]() {
            (void)InputTextControl("##fmod-bank", st.fmod.bankPath, 64, ctx.theme.fonts);
        });
        SettingGroup("RUNTIME & BUILD", ctx.theme.fonts);
        PluginSettingRow("Live Update", "Reload event metadata and banks without restarting the editor.", ctx, [&st, &ctx]() {
            DrawToggleState("##fmod-live", &st.fmod.liveUpdate, ctx);
        });
        PluginSettingRow("Fail Build On Missing Banks", "Prevents shipping builds with unresolved audio events.", ctx, [&st, &ctx]() {
            DrawToggleState("##fmod-fail", &st.fmod.failOnMissing, ctx);
        });
        PluginSettingRow("Target Platform", "Bank platform used for editor preview.", ctx, [&st, &ctx]() {
            static constexpr std::array kPlatforms = {"Desktop", "Windows", "macOS", "Linux", "Console"};
            (void)ComboControl("##fmod-plat", &st.fmod.targetPlatform, kPlatforms.data(), 5, ctx.theme.fonts);
        });
    }

    void DrawFmodDetailContent(SettingsState &st, const EditorGuiContext &ctx, const int activeTab) {
        switch (activeTab) {
            case 0:
                DrawFmodSettings(st, ctx);
                break;

            case 1: {
                static const std::array<PermissionRowSpec, 2> kPerms = {{
                    {"✓", "Read and write audio banks", "Limited to configured FMOD project and bank output paths.", "Scoped",
                     BadgeTone::Success},
                    {"!", "Launch external FMOD Studio process", "Requires a configured executable path and user initiated action.",
                     "User action", BadgeTone::Warning},
                }};
                DrawPermissionRows(kPerms, ctx);
            } break;

            case 2: {
                static const std::array<DiagnosticMetricSpec, 3> kMetrics = {{
                    {"BANKS", "14", "loaded", Text()},
                    {"UNRESOLVED", "2", "events", Warn()},
                    {"LIVE UPDATE", "On", "connected", Ok()},
                }};
                DrawDiagnosticMetrics(kMetrics, ctx);
                const std::array kActivity = {"13:58  bank import finished with 2 unresolved event refs",
                                              "13:44  live update connection established", "13:31  Desktop bank validation completed"};
                DrawDiagnosticActivity(kActivity, ctx);
            } break;

            case 3:
                DrawManifestBlock("plugins/fmod/plugin.yaml",
                                  "id: vendor.fmod\n"
                                  "version: 2.02.20\n"
                                  "entry: plugins/fmod/horo-fmod.plugin\n"
                                  "scope: project\n"
                                  "permissions:\n"
                                  "  - audio.bank.readwrite\n"
                                  "  - process.launch.user_action\n"
                                  "  - build.validation",
                                  ctx);
                break;
            default:
                break;
        }
    }

    void DrawSteamSettings(SettingsState &st, const EditorGuiContext &ctx) {
        SettingGroup("STEAM APP", ctx.theme.fonts, true);
        PluginSettingRow("App ID", "Use 480 for local Spacewar-style testing only.", ctx, [&ctx]() {
            static int steamAppId = 480;
            InputIntControl("##steam-appid", &steamAppId, ctx.theme.fonts);
        });
        PluginSettingRow("SDK Path", "Path to the local Steamworks SDK root.", ctx, [&st, &ctx]() {
            (void)InputTextControl("##steam-sdk", st.steam.sdkPath, 64, ctx.theme.fonts);
        });
        PluginSettingRow("Initialize On", "Controls when Steam API is started during editor workflows.", ctx, [&st, &ctx]() {
            static constexpr std::array kModes = {"Play Mode Only", "Editor Launch", "Build Runtime Only"};
            (void)ComboControl("##steam-init", &st.steam.initMode, kModes.data(), 3, ctx.theme.fonts);
        });
        SettingGroup("FEATURES", ctx.theme.fonts);
        PluginSettingRow("Overlay", "Enable Steam overlay while testing from Play Mode.", ctx, [&st, &ctx]() {
            DrawToggleState("##steam-overlay", &st.steam.overlay, ctx);
        });
        PluginSettingRow("Achievements", "Expose achievement authoring and validation panels.", ctx, [&st, &ctx]() {
            DrawToggleState("##steam-ach", &st.steam.achievements, ctx);
        });
        PluginSettingRow("Networking Sockets", "Enable Steam networking transport for multiplayer preview.", ctx, [&st, &ctx]() {
            DrawToggleState("##steam-net", &st.steam.networking, ctx);
        });
    }

    void DrawSteamDetailContent(SettingsState &st, const EditorGuiContext &ctx, const int activeTab) {
        switch (activeTab) {
            case 0:
                DrawSteamSettings(st, ctx);
                break;

            case 1: {
                static const std::array<PermissionRowSpec, 2> kPerms = {{
                    {"✓", "Read platform config", "Reads App ID, achievements config, and build target metadata.", "Allowed",
                     BadgeTone::Success},
                    {"!", "Network access", "Only enabled when Steam networking transport is selected.", "Conditional", BadgeTone::Warning},
                }};
                DrawPermissionRows(kPerms, ctx);
            } break;

            case 2: {
                static const std::array<DiagnosticMetricSpec, 3> kMetrics = {{
                    {"STATUS", "Disabled", "not loaded", Dim()},
                    {"SDK", "Missing", "path required", Warn()},
                    {"OVERLAY", "Ready", "waiting", Ok()},
                }};
                DrawDiagnosticMetrics(kMetrics, ctx);
                const std::array kActivity = {"12:45  skipped init because Steamworks SDK is disabled", "12:44  overlay check passed",
                                              "12:42  missing SDK path warning emitted"};
                DrawDiagnosticActivity(kActivity, ctx);
            } break;

            case 3:
                DrawManifestBlock("plugins/steamworks/plugin.yaml",
                                  "id: vendor.steamworks\n"
                                  "version: 1.59\n"
                                  "entry: plugins/steamworks/horo-steam.plugin\n"
                                  "scope: project\n"
                                  "permissions:\n"
                                  "  - platform.config.read\n"
                                  "  - network.conditional\n"
                                  "  - achievements.write",
                                  ctx);
                break;
            default:
                break;
        }
    }

}  // namespace Horo::Editor::SettingsModalInternal
