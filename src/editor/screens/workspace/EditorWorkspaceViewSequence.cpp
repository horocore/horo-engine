#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/modals/settings/NavigationSettingsDraft.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace Horo::Editor {
    namespace {
        /** @brief Emits stable-neighbor reorder requests for the active document tab. */
        void DrawDocumentReorderActions(const ILocalizationService &localization, const std::span<const WorkspaceDocumentTab> tabs,
                                        const std::size_t index, EditorWorkspaceViewCommandData &outCommand) {
            const auto instance = tabs[index].identity.instance;
            ImGui::SameLine();
            if (Ui::Button({.label = localization.Get("editor", "workspace.documents.move_left").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = index > 0})) {
                outCommand.command = EditorWorkspaceViewCommand::MoveWorkspaceDocument;
                outCommand.documentInstance = instance;
                outCommand.documentMoveBefore = tabs[index - 1].identity.instance;
            }
            ImGui::SameLine();
            if (Ui::Button({.label = localization.Get("editor", "workspace.documents.move_right").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = index + 1 < tabs.size()})) {
                outCommand.command = EditorWorkspaceViewCommand::MoveWorkspaceDocument;
                outCommand.documentInstance = instance;
                outCommand.documentMoveBefore = index + 2 < tabs.size() ? std::optional{tabs[index + 2].identity.instance} : std::nullopt;
            }
        }
    }  // namespace

    /** @copydoc EditorWorkspaceView::DocumentTabHeight */
    float EditorWorkspaceView::DocumentTabHeight(const WorkspaceDockArea area) const {
        if (area != WorkspaceDockArea::Document)
            return 0.0F;
        const auto &tokens = Theme::GetActiveTokens();
        return DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium).minimumHeight + tokens.spacing.propertyRowGap * 2.0F +
               ImGui::GetStyle().ScrollbarSize;
    }

    /** @copydoc EditorWorkspaceView::DrawDockContent */
    void EditorWorkspaceView::DrawDockContent(const WorkspaceDockArea area, const std::shared_ptr<IWorkspacePanel> &panel,
                                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const {
        if (area == WorkspaceDockArea::Document &&
            (DrawUiCanvasDocument(viewModel, outCommand) || DrawSourceDocument(viewModel, outCommand) ||
             DrawNavigationDocument(viewModel) || DrawPCGDocument(viewModel, outCommand) || DrawSequenceDocument(viewModel, outCommand))) {
            if (panel)
                panel->OnPresentationHidden(outCommand);
            return;
        }
        if (panel)
            panel->DrawPanel(ImGui::GetWindowPos(), ImGui::GetWindowSize(), viewModel, outCommand, m_context);
    }

    /** @copydoc EditorWorkspaceView::DrawWorkspaceDocumentTabs */
    void EditorWorkspaceView::DrawWorkspaceDocumentTabs(const EditorWorkspaceViewModel &viewModel,
                                                        EditorWorkspaceViewCommandData &outCommand) {
        const auto tabs = viewModel.workspacePanelHost.DocumentTabs();
        const auto active = viewModel.workspacePanelHost.ActiveDocument();
        const auto &tokens = Theme::GetActiveTokens();
        for (std::size_t index = 0; index < tabs.size(); ++index) {
            const auto &tab = tabs[index];
            const auto instance = tab.identity.instance;
            const std::string id = std::to_string(instance.Value());
            ImGui::PushID(id.c_str());
            const std::string &path = tab.identity.key.source.Value();
            const auto separator = path.rfind('/');
            const std::string title =
                path.substr(separator == std::string::npos ? 0 : separator + 1) + (tab.dirty ? " *" : "") + "###document";
            if (const float width = std::min(ImGui::CalcTextSize(title.c_str(), nullptr, true).x + tokens.spacing.propertyRowGap * 4.0F,
                                             240.0F * tokens.sizes.uiScale);
                Ui::Button({.label = title.c_str(),
                            .size = {width, 0},
                            .variant = active == instance ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                            .font = m_context.theme.fonts.sans})) {
                outCommand.command = EditorWorkspaceViewCommand::FocusWorkspaceDocument;
                outCommand.documentInstance = instance;
            }
            if (ImGui::IsItemHovered())
                Ui::ShowTooltip(path.c_str(), &m_context.theme.fonts);
            if (active == instance && active != m_lastPresentedDocument)
                ImGui::SetScrollHereX(0.5F);
            ImGui::SameLine();
            if (const std::string close = m_context.localization.Get("editor", "workspace.documents.close") + "###close";
                Ui::Button({.label = close.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = m_context.theme.fonts.sans})) {
                outCommand.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument;
                outCommand.documentInstance = instance;
            }
            if (active == instance) {
                DrawDocumentReorderActions(m_context.localization, tabs, index, outCommand);
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        m_lastPresentedDocument = active;
    }

    /** @copydoc EditorWorkspaceView::ResetSourceSession */
    void EditorWorkspaceView::ResetSourceSession() noexcept {
        m_uiCanvasGesture.reset();
        m_sourceSession.Reset();
    }

    /** @copydoc EditorWorkspaceView::DrawSourceDocument */
    bool EditorWorkspaceView::DrawSourceDocument(const EditorWorkspaceViewModel &viewModel,
                                                 EditorWorkspaceViewCommandData &outCommand) const {
        const auto active = viewModel.workspacePanelHost.ActiveDocument();
        const auto tabs = viewModel.workspacePanelHost.DocumentTabs();
        m_sourceSession.Retain(tabs);
        const auto tab = std::ranges::find_if(tabs, [active](const WorkspaceDocumentTab &candidate) {
            return candidate.identity.instance == active;
        });
        if (tab == tabs.end() || tab->identity.key.kind != DocumentKind::Source) {
            m_sourceSession.ClearFocus();
            return false;
        }
        const auto index = static_cast<std::size_t>(std::count_if(tabs.begin(), tab, [](const WorkspaceDocumentTab &candidate) {
            return candidate.identity.key.kind == DocumentKind::Source;
        }));
        if (index >= viewModel.sourceDocuments.size() || viewModel.sourceDocuments[index].Revision() == 0) {
            Ui::ErrorText(m_context.localization.Get("editor", "workspace.source_open.unavailable").c_str(), m_context.theme.fonts);
            return true;
        }
        const auto &source = viewModel.sourceDocuments[index];
        if (Ui::Button({.label = m_context.localization.Get("editor", "workspace.documents.save").c_str(),
                        .enabled = tab->dirty,
                        .font = m_context.theme.fonts.sans})) {
            outCommand.command = EditorWorkspaceViewCommand::SaveSourceDocument;
            outCommand.sourceSave = SourceSaveRequest{tab->identity.instance, source.Revision(), {}};
        }
        const auto history = index < viewModel.sourceHistory.size() ? viewModel.sourceHistory[index] : SourceHistoryState{};
        DrawSourceEditor(tab->identity.instance, source, history, viewModel, outCommand);
        return true;
    }

    /** @copydoc EditorWorkspaceView::DrawSourceEditor */
    void EditorWorkspaceView::DrawSourceEditor(const DocumentInstanceId instance, const SourceDocumentSnapshot &source,
                                               const SourceHistoryState &history, const EditorWorkspaceViewModel &viewModel,
                                               EditorWorkspaceViewCommandData &outCommand) const {
        auto *presentation = m_sourceSession.Acquire(instance);
        if (!presentation) {
            Ui::ErrorText(m_context.localization.Get("editor", "workspace.source_open.unavailable").c_str(), m_context.theme.fonts);
            return;
        }
        auto &widget = presentation->widget;
        presentation->Publish(source);
        if (viewModel.sourceEditingResult && viewModel.sourceEditingResult->HasValue())
            static_cast<void>(widget.Restore(viewModel.sourceEditingResult->Value()));
        auto toolIntent = presentation->tools.Draw(widget, source, history, viewModel.sourceEditingResult, m_context);
        const bool focus = m_sourceSession.NeedsFocus(instance) || (!toolIntent && presentation->tools.TakeFocusRequest());
        const auto available = ImGui::GetContentRegionAvail();
        auto intent =
            widget.Draw({available.x, available.y, Theme::GetActiveTokens().sizes.uiScale, focus, !toolIntent}, &m_context.theme.fonts);
        outCommand.sourceTextInputOwned = widget.OwnsInput() || presentation->tools.OwnsInput();
        m_sourceSession.Presented(instance);
        if (toolIntent)
            intent = std::move(toolIntent);
        if (intent) {
            outCommand.command = EditorWorkspaceViewCommand::SourceDocumentInteraction;
            outCommand.sourceInteraction = std::move(intent);
        }
    }

    /** @copydoc EditorWorkspaceView::DrawSequenceDocument */
    bool EditorWorkspaceView::DrawSequenceDocument(const EditorWorkspaceViewModel &viewModel,
                                                   EditorWorkspaceViewCommandData &outCommand) const {
        const auto found = std::ranges::find_if(viewModel.sequenceDocuments, [&](const auto &session) {
            return session.source->identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (found == viewModel.sequenceDocuments.end())
            return false;
        const auto text = [&](const char *key) {
            return m_context.localization.Get("editor", key);
        };
        Ui::SequenceTimelineLabels labels{
            .zoom = text("workspace.sequence.zoom"),
            .scroll = text("workspace.sequence.scroll"),
            .playhead = text("workspace.sequence.playhead"),
            .noContext = text("workspace.sequence.no_context"),
            .unavailable = text("workspace.sequence.unavailable"),
            .empty = text("workspace.sequence.empty"),
            .keys = text("workspace.sequence.keys"),
        };
        constexpr std::array keys{"workspace.sequence.transform", "workspace.sequence.property", "workspace.sequence.camera_cut",
                                  "workspace.sequence.event",     "workspace.sequence.audio",    "workspace.sequence.sub_sequence"};
        for (std::size_t index = 0; index < keys.size(); ++index)
            labels.trackTypes[index] = text(keys[index]);
        if (SequenceTimelineState state = found->timeline;
            Ui::SequenceTimeline(found->source->asset, state, labels, m_context.theme.fonts)) {
            outCommand.command = EditorWorkspaceViewCommand::UpdateSequenceTimeline;
            outCommand.documentInstance = found->source->identity.instance;
            outCommand.sequenceTimeline = state;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceView::DrawNavigationDocument */
    bool EditorWorkspaceView::DrawNavigationDocument(const EditorWorkspaceViewModel &viewModel) const {
        const auto tab = std::ranges::find_if(viewModel.navigationDocuments, [&](const auto &value) {
            return value.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (tab == viewModel.navigationDocuments.end())
            return false;
        DrawNavigationDefinitionEditor(*tab->editor, m_context, tab->asset);
        return true;
    }
}  // namespace Horo::Editor
