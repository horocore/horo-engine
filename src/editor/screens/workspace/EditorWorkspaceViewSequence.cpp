#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
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
                                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) {
        if (area == WorkspaceDockArea::Document && DrawSequenceDocument(viewModel, outCommand))
            return;
        if (panel)
            panel->DrawPanel(ImGui::GetWindowPos(), ImGui::GetWindowSize(), viewModel, outCommand, m_context);
    }

    /** @copydoc EditorWorkspaceView::DrawSequenceDocumentTabs */
    void EditorWorkspaceView::DrawSequenceDocumentTabs(const EditorWorkspaceViewModel &viewModel,
                                                       EditorWorkspaceViewCommandData &outCommand) {
        for (const auto &session : viewModel.sequenceDocuments) {
            ImGui::SameLine();
            const auto instance = session.source->identity.instance;
            const std::string id = std::to_string(instance.value);
            ImGui::PushID(id.c_str());
            const std::string title = session.source->asset.Data().name + "###sequence";
            if (Ui::Button({.label = title.c_str(),
                            .variant = viewModel.workspacePanelHost.ActiveDocument() == instance ? Ui::ButtonVariant::Primary
                                                                                                 : Ui::ButtonVariant::Secondary,
                            .font = m_context.theme.fonts.sans})) {
                outCommand.command = EditorWorkspaceViewCommand::FocusSequenceDocument;
                outCommand.documentInstance = instance;
            }
            ImGui::SameLine();
            const std::string close = m_context.localization.Get("editor", "workspace.sequence.close") + "###close";
            if (Ui::Button({.label = close.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = m_context.theme.fonts.sans})) {
                outCommand.command = EditorWorkspaceViewCommand::CloseSequenceDocument;
                outCommand.documentInstance = instance;
            }
            ImGui::PopID();
        }
    }

    /** @copydoc EditorWorkspaceView::DrawSequenceDocument */
    bool EditorWorkspaceView::DrawSequenceDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) {
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
        SequenceTimelineState state = found->timeline;
        if (Ui::SequenceTimeline(found->source->asset, state, labels, m_context.theme.fonts)) {
            outCommand.command = EditorWorkspaceViewCommand::UpdateSequenceTimeline;
            outCommand.documentInstance = found->source->identity.instance;
            outCommand.sequenceTimeline = state;
        }
        return true;
    }
}  // namespace Horo::Editor
