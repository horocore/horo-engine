#include "SettingsModalInternal.h"
#include "editor/update/UpdateExperienceSession.h"

#include <array>
#include <format>
#include <imgui.h>

namespace Horo::Editor::SettingsModalInternal {
    namespace {
        [[nodiscard]] const char *PhaseKey(const EditorUpdatePhase phase) {
            using enum EditorUpdatePhase;
            switch (phase) {
                case Idle:
                    return "settings.updates.phase.idle";
                case Checking:
                    return "settings.updates.phase.checking";
                case UpToDate:
                    return "settings.updates.phase.up_to_date";
                case Available:
                    return "settings.updates.phase.available";
                case Downloading:
                    return "settings.updates.phase.downloading";
                case Verifying:
                    return "settings.updates.phase.verifying";
                case Staged:
                    return "settings.updates.phase.staged";
                case RestartRequired:
                    return "settings.updates.phase.restart_required";
                case Activating:
                    return "settings.updates.phase.activating";
                case RollbackPending:
                    return "settings.updates.phase.rollback_pending";
                case Active:
                    return "settings.updates.phase.active";
                case Failed:
                    return "settings.updates.phase.failed";
                case RolledBack:
                    return "settings.updates.phase.rolled_back";
            }
            return "settings.updates.phase.failed";
        }

        [[nodiscard]] bool ActionButton(const EditorGuiContext &ctx, const char *key, const bool enabled,
                                        const Ui::ButtonVariant variant = Ui::ButtonVariant::Secondary) {
            const std::string label = ctx.localization.Get("editor", key);
            return Ui::Button({.label = label.c_str(), .variant = variant, .enabled = enabled, .font = ctx.theme.fonts.sans});
        }

        void DrawNotes(const EditorUpdateSnapshot &snapshot, const EditorGuiContext &ctx) {
            const std::string notesTitle = ctx.localization.Get("editor", "settings.updates.release_notes");
            Ui::SectionTitle(notesTitle.c_str(), ctx.theme.fonts);
            if (!snapshot.offer) {
                const std::string empty = ctx.localization.Get("editor", "settings.updates.no_offer");
                ImGui::TextWrapped("%s", empty.c_str());
                return;
            }
            ImGui::TextUnformatted(snapshot.offer->version.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", snapshot.offer->releaseNotes.c_str());
            if (snapshot.offer->compatibilityImpacts.empty())
                return;
            ImGui::Spacing();
            const std::string impactTitle = ctx.localization.Get("editor", "settings.updates.compatibility");
            ImGui::TextUnformatted(impactTitle.c_str());
            for (const std::string &impact : snapshot.offer->compatibilityImpacts) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", impact.c_str());
            }
        }

        void DrawConfirmation(SettingsState &state, UpdateExperienceSession &session, const EditorGuiContext &ctx) {
            if (state.pendingUpdateConfirmation != 0)
                ImGui::OpenPopup("##update-confirmation");
            if (!ImGui::BeginPopupModal("##update-confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;
            const std::string prompt =
                ctx.localization.Get("editor", state.pendingUpdateConfirmation == 1 ? "settings.updates.confirm.restart"
                                                                                    : "settings.updates.confirm.rollback");
            ImGui::TextWrapped("%s", prompt.c_str());
            ImGui::Spacing();
            if (const bool cancel = ActionButton(ctx, "settings.updates.cancel", true); cancel || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.pendingUpdateConfirmation = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ActionButton(ctx, state.pendingUpdateConfirmation == 1 ? "settings.updates.restart" : "settings.updates.rollback", true,
                             Ui::ButtonVariant::Primary)) {
                if (state.pendingUpdateConfirmation == 1)
                    static_cast<void>(session.RestartNow(true));
                else
                    static_cast<void>(session.Rollback(true));
                state.pendingUpdateConfirmation = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }  // namespace

    namespace {
        void DrawChannelConfirmation(SettingsState &state, UpdateExperienceSession &session, const EditorGuiContext &ctx) {
            if (state.pendingUpdateChannel >= 0)
                ImGui::OpenPopup("##update-channel-confirmation");
            if (!ImGui::BeginPopupModal("##update-channel-confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                return;
            const std::string prompt = ctx.localization.Get("editor", "settings.updates.confirm.channel");
            ImGui::TextWrapped("%s", prompt.c_str());
            if (const bool cancel = ActionButton(ctx, "settings.updates.cancel", true); cancel || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.pendingUpdateChannel = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ActionButton(ctx, "settings.updates.confirm.action", true, Ui::ButtonVariant::Primary)) {
                static_cast<void>(session.SetChannel({static_cast<EditorUpdateChannelKind>(state.pendingUpdateChannel), {}}, true));
                state.pendingUpdateChannel = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        void DrawChannelControl(SettingsState &state, UpdateExperienceSession &session, const EditorGuiContext &ctx) {
            const auto &snapshot = session.Snapshot();
            const std::array<std::string, 5> channelNames{ctx.localization.Get("editor", "settings.updates.channel.stable"),
                                                          ctx.localization.Get("editor", "settings.updates.channel.preview"),
                                                          ctx.localization.Get("editor", "settings.updates.channel.nightly"),
                                                          ctx.localization.Get("editor", "settings.updates.channel.enterprise"),
                                                          ctx.localization.Get("editor", "settings.updates.channel.offline")};
            const std::array<const char *, 5> channelLabels{channelNames[0].c_str(), channelNames[1].c_str(), channelNames[2].c_str(),
                                                            channelNames[3].c_str(), channelNames[4].c_str()};
            const std::string channelLabel = ctx.localization.Get("editor", "settings.updates.channel");
            const std::string channelDescription = ctx.localization.Get("editor", "settings.updates.channel.description");
            Ui::SettingRow(channelLabel.c_str(), channelDescription.c_str(), ctx.theme.fonts,
                           [&snapshot, &channelLabels, &ctx, &state, &session]() {
                auto selected = static_cast<int>(snapshot.channel.kind);
                if (Ui::ComboControl("##update-channel", &selected, channelLabels.data(), channelLabels.size(), ctx.theme.fonts)) {
                    const auto channel = static_cast<EditorUpdateChannelKind>(selected);
                    if (channel == EditorUpdateChannelKind::Preview || channel == EditorUpdateChannelKind::Nightly)
                        state.pendingUpdateChannel = selected;
                    else
                        static_cast<void>(session.SetChannel({channel, {}}));
                }
            });
            DrawChannelConfirmation(state, session, ctx);
        }

        void DrawPolicy(SettingsState &state, UpdateExperienceSession &session, const EditorGuiContext &ctx) {
            const auto &snapshot = session.Snapshot();
            DrawChannelControl(state, session, ctx);
            bool automatic = snapshot.automaticDownload;
            const std::string automaticLabel = ctx.localization.Get("editor", "settings.updates.automatic_download");
            const std::string automaticDescription = ctx.localization.Get("editor", "settings.updates.automatic_download.description");
            Ui::SettingRow(automaticLabel.c_str(), automaticDescription.c_str(), ctx.theme.fonts, [&automatic, &ctx, &session]() {
                if (Ui::ToggleControl("update-automatic-download", &automatic, ctx.theme.fonts))
                    session.SetAutomaticDownload(automatic);
            });
            bool installOnExit = snapshot.installOnExit;
            const std::string exitLabel = ctx.localization.Get("editor", "settings.updates.install_on_exit");
            const std::string exitDescription = ctx.localization.Get("editor", "settings.updates.install_on_exit.description");
            Ui::SettingRow(exitLabel.c_str(), exitDescription.c_str(), ctx.theme.fonts, [&installOnExit, &ctx, &session]() {
                if (Ui::ToggleControl("update-install-on-exit", &installOnExit, ctx.theme.fonts))
                    session.SetInstallOnExit(installOnExit);
            });
        }

        void DrawProgress(const EditorUpdateSnapshot &snapshot, const EditorGuiContext &ctx) {
            ImGui::Spacing();
            const std::string phase = ctx.localization.Get("editor", PhaseKey(snapshot.phase));
            ImGui::TextUnformatted(phase.c_str());
            if (snapshot.totalBytes > 0U) {
                const float fraction = static_cast<float>(snapshot.transferredBytes) / static_cast<float>(snapshot.totalBytes);
                ImGui::ProgressBar(fraction, {-1.0F, 0.0F}, std::format("{} / {}", snapshot.transferredBytes, snapshot.totalBytes).c_str());
            }
            if (!snapshot.diagnostic.empty()) {
                const std::string diagnostics = ctx.localization.Get("editor", "settings.updates.diagnostics");
                ImGui::TextUnformatted(diagnostics.c_str());
                ImGui::TextWrapped("%s", snapshot.diagnostic.c_str());
            }
        }

        void DrawActions(SettingsState &state, UpdateExperienceSession &session, const EditorGuiContext &ctx) {
            using enum EditorUpdatePhase;
            const auto &snapshot = session.Snapshot();
            if (const bool operationActive = snapshot.phase == Checking || snapshot.phase == Downloading || snapshot.phase == Verifying ||
                                             snapshot.phase == Activating;
                ActionButton(ctx, "settings.updates.check", !operationActive))
                static_cast<void>(session.CheckNow());
            if (ActionButton(ctx, "settings.updates.download", snapshot.phase == Available))
                static_cast<void>(session.Download());
            if (ActionButton(ctx, "settings.updates.cancel", snapshot.canCancel))
                static_cast<void>(session.Cancel());
            if (ActionButton(ctx, "settings.updates.restart", snapshot.phase == RestartRequired))
                state.pendingUpdateConfirmation = 1;
            if (ActionButton(ctx, "settings.updates.rollback", snapshot.phase == Active || snapshot.phase == Failed))
                state.pendingUpdateConfirmation = 2;
            DrawConfirmation(state, session, ctx);
        }
    }  // namespace

    void DrawUpdates(SettingsState &state, const EditorGuiContext &ctx) {
        const std::string title = ctx.localization.Get("editor", "settings.nav.updates");
        Ui::SectionTitle(title.c_str(), ctx.theme.fonts);
        if (!ctx.updates) {
            const std::string unavailable = ctx.localization.Get("editor", "settings.updates.unconfigured");
            ImGui::TextWrapped("%s", unavailable.c_str());
            return;
        }
        auto &session = *ctx.updates;
        DrawPolicy(state, session, ctx);
        DrawProgress(session.Snapshot(), ctx);
        DrawActions(state, session, ctx);
        ImGui::Spacing();
        DrawNotes(session.Snapshot(), ctx);
    }
}  // namespace Horo::Editor::SettingsModalInternal
