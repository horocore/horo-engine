#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserDialogs.h"

#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/BlackboardKeyWidgets.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPane.h"

#include <algorithm>
#include <format>
#include <string>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] std::string FormatByteSize(const std::uintmax_t bytes) {
            if (bytes < 1024U)
                return std::format("{} B", bytes);
            if (bytes < 1024U * 1024U)
                return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
            return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }

        void PushModalStyle() {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.0F, 18.0F});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0F, 8.0F});
            ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Bg1());
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::BorderStrong());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
            ImGui::PushStyleColor(ImGuiCol_Separator, Theme::Border());
            ImVec4 dim = Theme::Bg0();
            dim.w = 0.72F;
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dim);
        }

        void PopModalStyle() {
            ImGui::PopStyleColor(5);
            ImGui::PopStyleVar(4);
        }

        void DrawModalHeading(const std::string &title, const EditorGuiContext &context) {
            Theme::ScopedTextStyle textStyle(context.theme.fonts.sansEmphasis, Theme::TextPx::Heading(), Theme::FontPx::SansEmphasis);
            ImGui::TextColored(Theme::Text(), "%s", title.c_str());
            ImGui::Dummy({0.0F, 2.0F});
            ImGui::Separator();
            ImGui::Dummy({0.0F, 4.0F});
        }

        [[nodiscard]] bool DrawClosableModalHeading(const std::string &title, const EditorGuiContext &context) {
            constexpr float closeSize = 22.0F;
            const float headerY = ImGui::GetCursorPosY();
            {
                Theme::ScopedTextStyle textStyle(context.theme.fonts.sansEmphasis, Theme::TextPx::Heading(), Theme::FontPx::SansEmphasis);
                ImGui::TextColored(Theme::Text(), "%s", title.c_str());
            }
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeSize);
            ImGui::SetCursorPosY(headerY);
            const bool close = Ui::IconCloseButton("##CloseAssetInfo", {closeSize, closeSize});
            ImGui::SetCursorPosY(headerY + closeSize + 5.0F);
            ImGui::Separator();
            ImGui::Dummy({0.0F, 2.0F});
            return close;
        }

        void DrawInfoRow(const char *label, const char *value, const EditorGuiContext &context) {
            ImGui::PushID(label);
            if (!ImGui::BeginTable("##AssetInfoRow", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::PopID();
                return;
            }
            const float labelWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.30F, 72.0F, 132.0F);
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            {
                Theme::ScopedTextStyle textStyle(context.theme.fonts.sans, Theme::TextPx::Label(), Theme::FontPx::Sans);
                ImGui::TextColored(Theme::Muted(), "%s", label);
            }
            ImGui::TableSetColumnIndex(1);
            {
                Theme::ScopedTextStyle textStyle(context.theme.fonts.sansCompact, Theme::TextPx::Body(), Theme::FontPx::SansCompact);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
                ImGui::PushTextWrapPos(0.0F);
                ImGui::TextWrapped("%s", value);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
            ImGui::Separator();
            ImGui::PopID();
        }

        [[nodiscard]] std::string FormatImporterInfo(const ContentBrowserEntry &entry, const std::string &none) {
            if (entry.importerContributionId.empty()) {
                return none;
            }
            std::string result = entry.importerContributionId;
            if (!entry.importerVersion.empty()) {
                result += " @ " + entry.importerVersion;
            }
            if (entry.importerChanged) {
                result += " \xE2\x86\x92 " + entry.activeImporterVersion;
            }
            return result;
        }

        [[nodiscard]] std::string FormatModuleInfo(const ContentBrowserEntry &entry, const std::string &none) {
            if (entry.importerModuleId.empty()) {
                return none;
            }
            std::string result = entry.importerModuleId;
            if (!entry.importerModuleVersion.empty()) {
                result += " @ " + entry.importerModuleVersion;
            }
            if (entry.moduleChanged) {
                result += " \xE2\x86\x92 " + entry.activeImporterModuleId + " @ " + entry.activeImporterModuleVersion;
            }
            return result;
        }

        [[nodiscard]] std::string FormatReimportState(const ContentBrowserEntry &entry, const ILocalizationService &localization) {
            std::string reimportState = AssetBrowserPane::kTextScope.Text(localization, "info.reimport_manual");
            if (entry.sourceChanged) {
                reimportState = AssetBrowserPane::kTextScope.Text(localization, "info.reimport_source_stale");
            }
            if (entry.importerChanged || entry.moduleChanged) {
                if (entry.sourceChanged) {
                    reimportState += ", ";
                } else {
                    reimportState.clear();
                }
                const char *key = entry.moduleChanged ? "info.reimport_module_changed" : "info.reimport_importer_changed";
                reimportState += AssetBrowserPane::kTextScope.Text(localization, key);
            }
            return reimportState;
        }

        [[nodiscard]] std::string FormatLastImportReasons(const ContentBrowserEntry &entry, const ILocalizationService &localization) {
            using enum Assets::AssetImportReason;
            std::string lastImport;
            for (const Assets::AssetImportReason reason : entry.lastImportReasons) {
                const char *key = nullptr;
                switch (reason) {
                    case InitialImport:
                        key = "info.import_reason_initial";
                        break;
                    case ManualReimport:
                        key = "info.import_reason_manual";
                        break;
                    case SourceChanged:
                        key = "info.reimport_source_changed";
                        break;
                    case ImporterChanged:
                        key = "info.reimport_importer_changed";
                        break;
                    case ModuleChanged:
                        key = "info.reimport_module_changed";
                        break;
                }
                if (key != nullptr) {
                    if (!lastImport.empty()) {
                        lastImport += ", ";
                    }
                    lastImport += AssetBrowserPane::kTextScope.Text(localization, key);
                }
            }
            return lastImport;
        }

        void DrawDeleteDialogEntryDetails(const ContentBrowserEntry &entry, const EditorGuiContext &context) {
            ImGui::Dummy({0.0F, 6.0F});
            ImGui::TextColored(Theme::Text(), "%s", entry.displayName.c_str());
            if (entry.kind != ContentBrowserEntryKind::Asset) {
                return;
            }
            const std::string dependencyCount = std::to_string(entry.dependencyCount);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "delete.dependencies").c_str(), dependencyCount.c_str(),
                        context);
            if (entry.dependencyCount > 0) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0F);
                ImGui::TextColored(Theme::Warn(), "%s",
                                   AssetBrowserPane::kTextScope.Text(context.localization, "delete.dependency_warning").c_str());
                ImGui::PopTextWrapPos();
            }
        }

        void DrawAssetDetailsRows(const ContentBrowserEntry &entry, const EditorGuiContext &context) {
            const std::string &none = AssetBrowserPane::kTextScope.Text(context.localization, "info.none");
            const std::string size = FormatByteSize(entry.byteSize);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.type").c_str(),
                        entry.assetType.empty() ? none.c_str() : entry.assetType.c_str(), context);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.asset_id").c_str(),
                        entry.assetId.empty() ? none.c_str() : entry.assetId.c_str(), context);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.registration").c_str(),
                        AssetBrowserPane::kTextScope.Text(context.localization, entry.registered ? "info.registered" : "info.unregistered")
                            .c_str(),
                        context);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.size").c_str(), size.c_str(), context);
            const std::string importer = FormatImporterInfo(entry, none);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.importer").c_str(), importer.c_str(), context);
            const std::string moduleInfo = FormatModuleInfo(entry, none);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.module").c_str(), moduleInfo.c_str(), context);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.source").c_str(),
                        entry.absoluteImportSourcePath.empty() ? none.c_str() : entry.absoluteImportSourcePath.c_str(), context);
            const std::string reimportState = FormatReimportState(entry, context.localization);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.reimport_status").c_str(),
                        entry.canReimport ? reimportState.c_str() : none.c_str(), context);
            const std::string lastImport = FormatLastImportReasons(entry, context.localization);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.last_import_reason").c_str(),
                        lastImport.empty() ? none.c_str() : lastImport.c_str(), context);
            DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.metadata").c_str(),
                        entry.absoluteMetadataPath.empty() ? none.c_str() : entry.absoluteMetadataPath.c_str(), context);
        }

        void DrawInfoPopup(AssetBrowserInteractionState &state, const EditorGuiContext &context) {
            constexpr const char *popupId = "##ContentBrowserInfo";
            if (state.openAssetInfo) {
                ImGui::OpenPopup(popupId);
                state.openAssetInfo = false;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float popupWidth = std::max(160.0F, std::min(560.0F, viewport->WorkSize.x - 32.0F));
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
            ImGui::SetNextWindowSizeConstraints({popupWidth, 0.0F}, {popupWidth, viewport->WorkSize.y - 48.0F});
            PushModalStyle();
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                if (DrawClosableModalHeading(AssetBrowserPane::kTextScope.Text(context.localization, "info.title"), context))
                    ImGui::CloseCurrentPopup();
                if (state.popupEntry.has_value()) {
                    const auto &entry = *state.popupEntry;
                    DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.name").c_str(), entry.displayName.c_str(),
                                context);
                    ImGui::Dummy({0.0F, 2.0F});
                    DrawInfoRow(AssetBrowserPane::kTextScope.Text(context.localization, "info.path").c_str(), entry.absolutePath.c_str(),
                                context);
                    if (entry.kind == ContentBrowserEntryKind::Asset) {
                        DrawAssetDetailsRows(entry, context);
                    }
                }
                ImGui::EndPopup();
            }
            PopModalStyle();
        }

        void DrawRenamePopup(AssetBrowserInteractionState &state, EditorWorkspaceViewCommandData &command,
                             const EditorGuiContext &context) {
            constexpr const char *popupId = "##ContentBrowserRename";
            if (state.openRename) {
                ImGui::OpenPopup(popupId);
                state.openRename = false;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float popupWidth = std::max(160.0F, std::min(460.0F, viewport->WorkSize.x - 32.0F));
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
            ImGui::SetNextWindowSizeConstraints({popupWidth, 0.0F}, {popupWidth, viewport->WorkSize.y - 48.0F});
            PushModalStyle();
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                DrawModalHeading(AssetBrowserPane::kTextScope.Text(context.localization, "rename.title"), context);
                Ui::FieldLabel(AssetBrowserPane::kTextScope.Text(context.localization, "rename.name").c_str(), context.theme.fonts);
                static_cast<void>(Ui::InputTextControl("##ContentBrowserRenameInput", state.renameBuffer.data(), state.renameBuffer.size(),
                                                       context.theme.fonts));
                ImGui::Dummy({0.0F, 12.0F});
                if (Ui::Button({
                        .label = AssetBrowserPane::kTextScope.Text(context.localization, "action.cancel").c_str(),
                        .size = {92.0F, 32.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context.theme.fonts.sans,
                    })) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (Ui::Button({
                        .label = AssetBrowserPane::kTextScope.Text(context.localization, "action.rename").c_str(),
                        .size = {92.0F, 32.0F},
                        .variant = Ui::ButtonVariant::Primary,
                        .enabled = state.renameBuffer[0] != '\0',
                        .font = context.theme.fonts.sans,
                    }) &&
                    state.popupEntry.has_value()) {
                    command = AssetBrowserInteractionSession::Rename(state.popupEntry->absolutePath, state.renameBuffer.data());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            PopModalStyle();
        }

        void DrawDeletePopup(AssetBrowserInteractionState &state, EditorWorkspaceViewCommandData &command,
                             const EditorGuiContext &context) {
            constexpr const char *popupId = "##ContentBrowserDelete";
            if (state.openDeleteConfirmation) {
                ImGui::OpenPopup(popupId);
                state.openDeleteConfirmation = false;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float popupWidth = std::max(160.0F, std::min(480.0F, viewport->WorkSize.x - 32.0F));
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
            ImGui::SetNextWindowSizeConstraints({popupWidth, 0.0F}, {popupWidth, viewport->WorkSize.y - 48.0F});
            PushModalStyle();
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                DrawModalHeading(AssetBrowserPane::kTextScope.Text(context.localization, "delete.title"), context);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0F);
                ImGui::TextColored(Theme::Muted(), "%s", AssetBrowserPane::kTextScope.Text(context.localization, "delete.message").c_str());
                ImGui::PopTextWrapPos();
                if (state.popupEntry.has_value()) {
                    DrawDeleteDialogEntryDetails(*state.popupEntry, context);
                }

                ImGui::Dummy({0.0F, 12.0F});
                if (Ui::Button({
                        .label = AssetBrowserPane::kTextScope.Text(context.localization, "action.cancel").c_str(),
                        .size = {92.0F, 32.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context.theme.fonts.sans,
                    })) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (Ui::Button({
                        .label = AssetBrowserPane::kTextScope.Text(context.localization, "action.delete").c_str(),
                        .size = {92.0F, 32.0F},
                        .variant = Ui::ButtonVariant::Primary,
                        .font = context.theme.fonts.sans,
                    }) &&
                    state.popupEntry.has_value()) {
                    command = AssetBrowserInteractionSession::Delete(state.popupEntry->absolutePath);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            PopModalStyle();
        }

        void DrawCreateFolderPopup(AssetBrowserInteractionState &state, const ContentBrowserDirectory &directory,
                                   EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            constexpr const char *popupId = "##ContentBrowserCreateFolder";
            if (state.openCreateFolder) {
                ImGui::OpenPopup(popupId);
                state.openCreateFolder = false;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float popupWidth = std::max(160.0F, std::min(460.0F, viewport->WorkSize.x - 32.0F));
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
            ImGui::SetNextWindowSizeConstraints({popupWidth, 0.0F}, {popupWidth, viewport->WorkSize.y - 48.0F});
            PushModalStyle();
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                DrawModalHeading(AssetBrowserPane::kTextScope.Text(context.localization, "create_folder.title"), context);
                Ui::FieldLabel(AssetBrowserPane::kTextScope.Text(context.localization, "create_folder.name").c_str(), context.theme.fonts);
                static_cast<void>(Ui::InputTextControl("##ContentBrowserCreateFolderInput", state.createFolderBuffer.data(),
                                                       state.createFolderBuffer.size(), context.theme.fonts));
                ImGui::Dummy({0.0F, 12.0F});
                if (Ui::Button({
                        .label = AssetBrowserPane::kTextScope.Text(context.localization, "action.cancel").c_str(),
                        .size = {92.0F, 32.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context.theme.fonts.sans,
                    })) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (Ui::Button({
                        .label = AssetBrowserPane::kTextScope.Text(context.localization, "action.create_folder").c_str(),
                        .size = {112.0F, 32.0F},
                        .variant = Ui::ButtonVariant::Primary,
                        .enabled = state.createFolderBuffer[0] != '\0',
                        .font = context.theme.fonts.sans,
                    })) {
                    command = AssetBrowserInteractionSession::CreateFolder(directory.absoluteCurrentPath, state.createFolderBuffer.data());
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            PopModalStyle();
        }

        void DrawCreateBlackboardPopup(AssetBrowserInteractionState &state, EditorWorkspaceViewCommandData &command,
                                       const EditorGuiContext &context) {
            constexpr const char *popupId = "##ContentBrowserCreateBlackboard";
            if (state.openCreateBlackboard) {
                ImGui::OpenPopup(popupId);
                state.openCreateBlackboard = false;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float width = std::max(160.0F, std::min(460.0F, viewport->WorkSize.x - 32.0F));
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
            ImGui::SetNextWindowSizeConstraints({width, 0.0F}, {width, viewport->WorkSize.y - 48.0F});
            PushModalStyle();
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                const auto text = [&](std::string_view key) -> const std::string & {
                    return context.localization.Get("editor", key);
                };
                auto &draft = state.createBlackboard;
                const auto &fonts = context.theme.fonts;
                DrawModalHeading(text("workspace.blackboard.create_title"), context);
                const auto name = [&](const char *id, std::string_view caption, std::string &value) {
                    Ui::FieldLabel(text(caption).c_str(), fonts);
                    static_cast<void>(Ui::InputTextControl(id, value, AI::BlackboardSchemaAuthoringRow::MaximumLabelBytes + 1, fonts));
                };
                name("##NewBlackboardName", "workspace.blackboard.create_schema_name", draft.request.name);
                name("##NewBlackboardKey", "workspace.blackboard.create_key_name", draft.request.keyName);
                DrawBlackboardDraftKey(context, draft.request.key, draft.numericDefault, command);
                if (draft.request.key.presence == AI::BlackboardKeyPresence::Required && !draft.request.key.defaultValue)
                    Ui::ErrorText(text("workspace.blackboard.create_required_default").c_str(), fonts);
                ImGui::Dummy({0.0F, 12.0F});
                if (Ui::Button({.label = AssetBrowserPane::kTextScope.Text(context.localization, "action.cancel").c_str(),
                                .variant = Ui::ButtonVariant::Secondary,
                                .font = fonts.sans}))
                    ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (Ui::Button({.label = text("workspace.blackboard.create_confirm").c_str(),
                                .variant = Ui::ButtonVariant::Primary,
                                .enabled = AssetBrowserInteractionSession::CanConfirmCreateBlackboard(draft),
                                .font = fonts.sans})) {
                    if (auto confirmed = AssetBrowserInteractionSession::ConfirmCreateBlackboard(draft)) {
                        command = std::move(*confirmed);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
            PopModalStyle();
        }

        void DrawCreateQueryPopup(AssetBrowserInteractionState &state, EditorWorkspaceViewCommandData &command,
                                  const EditorGuiContext &context) {
            constexpr const char *popupId = "##ContentBrowserCreateEnvironmentQuery";
            if (state.openCreateQuery) {
                ImGui::OpenPopup(popupId);
                state.openCreateQuery = false;
            }
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float width = std::max(160.0F, std::min(460.0F, viewport->WorkSize.x - 32.0F));
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
            ImGui::SetNextWindowSizeConstraints({width, 0.0F}, {width, viewport->WorkSize.y - 48.0F});
            PushModalStyle();
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                const auto text = [&](std::string_view key) -> const std::string & {
                    return context.localization.Get("editor", key);
                };
                const auto &fonts = context.theme.fonts;
                auto &draft = state.createQuery;
                DrawModalHeading(text("workspace.environment_query.create_title"), context);
                Ui::FieldLabel(text("workspace.environment_query.create_name").c_str(), fonts);
                static_cast<void>(Ui::InputTextControl("##NewQueryName", draft.name.data(), draft.name.size(), fonts));
                const auto integer = [&](const char *identity, std::string_view label, int &value) {
                    Ui::FieldLabel(text(label).c_str(), fonts);
                    Ui::InputIntControl(identity, &value, fonts);
                };
                integer("##NewQueryStep", "workspace.environment_query.create_step", draft.stepMillimeters);
                integer("##NewQueryOuter", "workspace.environment_query.create_outer", draft.outerSteps);
                static_cast<void>(Ui::CheckboxControl(text("workspace.environment_query.create_annulus").c_str(), &draft.annulus, fonts));
                if (draft.annulus)
                    integer("##NewQueryInner", "workspace.environment_query.create_inner", draft.innerSteps);
                integer("##NewQueryMaximum", "workspace.environment_query.create_limit", draft.maximumResults);
                ImGui::Dummy({0.0F, 12.0F});
                if (Ui::Button({.label = AssetBrowserPane::kTextScope.Text(context.localization, "action.cancel").c_str(),
                                .variant = Ui::ButtonVariant::Secondary,
                                .font = fonts.sans}))
                    ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (Ui::Button({.label = text("workspace.environment_query.create_confirm").c_str(),
                                .variant = Ui::ButtonVariant::Primary,
                                .enabled = AssetBrowserInteractionSession::CanConfirmCreateQuery(draft),
                                .font = fonts.sans})) {
                    if (auto confirmed = AssetBrowserInteractionSession::ConfirmCreateQuery(draft)) {
                        command = std::move(*confirmed);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
            PopModalStyle();
        }
    }  // namespace

    /** @copydoc DrawAssetBrowserDialogs */
    void DrawAssetBrowserDialogs(AssetBrowserInteractionState &state, const ContentBrowserDirectory &directory,
                                 EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        DrawInfoPopup(state, context);
        DrawRenamePopup(state, command, context);
        DrawDeletePopup(state, command, context);
        DrawCreateFolderPopup(state, directory, command, context);
        DrawCreateBlackboardPopup(state, command, context);
        DrawCreateQueryPopup(state, command, context);
    }
}  // namespace Horo::Editor
