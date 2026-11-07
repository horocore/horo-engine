#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/modals/settings/NavigationSettingsDraft.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <span>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceView::DocumentTabHeight */
    float EditorWorkspaceView::DocumentTabHeight(const WorkspaceDockArea area) const {
        if (area != WorkspaceDockArea::Document)
            return 0.0F;
        return Ui::DocumentTabHeight();
    }

    /** @copydoc EditorWorkspaceView::DrawDockContent */
    void EditorWorkspaceView::DrawDockContent(const WorkspaceDockArea area, const std::shared_ptr<IWorkspacePanel> &panel,
                                              const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const {
        if (const bool prefab = std::ranges::any_of(viewModel.prefabDocuments,
                                                    [&](const auto &entry) {
            return entry.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
            area != WorkspaceDockArea::Document && prefab && panel &&
            (panel->GetId() == "horo.hierarchy" || panel->GetId() == "horo.inspector")) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            m_context.localization.Get("editor", "workspace.prefab_document.source_controls"), Theme::Muted(),
                            m_context.theme.fonts);
            return;
        }
        if (area == WorkspaceDockArea::Document && DrawTerrainDocument(viewModel, outCommand))
            return;
        if (area == WorkspaceDockArea::Document &&
            (m_uiCanvasView.Draw(viewModel, outCommand) || DrawPrefabDocument(viewModel, outCommand) ||
             DrawSourceDocument(viewModel, outCommand) || DrawNavigationDocument(viewModel) || DrawPCGDocument(viewModel, outCommand) ||
             DrawSequenceDocument(viewModel, outCommand))) {
            if (panel)
                panel->OnPresentationHidden(outCommand);
            return;
        }
        if (panel)
            GuiLive::InvokeVirtual<&IWorkspacePanel::DrawPanel>(m_context.theme.liveReload, *panel, ImGui::GetWindowPos(),
                                                                ImGui::GetWindowSize(), viewModel, outCommand, m_context);
    }

    /** @copydoc EditorWorkspaceView::DrawWorkspaceDocumentTabs */
    void EditorWorkspaceView::DrawWorkspaceDocumentTabs(const EditorWorkspaceViewModel &viewModel,
                                                        EditorWorkspaceViewCommandData &outCommand) {
        const auto tabs = viewModel.workspacePanelHost.DocumentTabs();
        const auto active = viewModel.workspacePanelHost.ActiveDocument();
        for (std::size_t index = 0; index < tabs.size(); ++index) {
            const auto &tab = tabs[index];
            const auto instance = tab.identity.instance;
            const std::string id = std::to_string(instance.Value());
            ImGui::PushID(id.c_str());
            const std::string &path = tab.identity.key.source.Value();
            const auto separator = path.rfind('/');
            const std::string title = path.substr(separator == std::string::npos ? 0 : separator + 1) + (tab.dirty ? " *" : "");
            const auto action =
                Ui::DocumentTabButton("document", title.c_str(), m_context.localization.Get("editor", "workspace.documents.close").c_str(),
                                      active == instance, m_context.theme.fonts);
            if (action != Ui::DocumentTabAction::None) {
                outCommand.command = action == Ui::DocumentTabAction::Close ? EditorWorkspaceViewCommand::CloseWorkspaceDocument
                                                                            : EditorWorkspaceViewCommand::FocusWorkspaceDocument;
                outCommand.documentInstance = instance;
            }
            if (ImGui::IsItemHovered())
                Ui::ShowTooltip(path.c_str(), &m_context.theme.fonts);
            if (active == instance && active != m_lastPresentedDocument)
                ImGui::SetScrollHereX(0.5F);
            ImGui::SameLine();
            ImGui::PopID();
        }
        m_lastPresentedDocument = active;
    }

    /** @copydoc EditorWorkspaceView::ResetSourceSession */
    void EditorWorkspaceView::ResetSourceSession() noexcept {
        m_uiCanvasView.Reset();
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
        ImGui::SameLine();
        const auto history = index < viewModel.sourceHistory.size() ? viewModel.sourceHistory[index] : SourceHistoryState{};
        DrawSourceEditor(tab->identity.instance, source, history, viewModel, outCommand);
        return true;
    }

    /** @copydoc EditorWorkspaceView::RefreshPrefabNameDrafts */
    void EditorWorkspaceView::RefreshPrefabNameDrafts(const PrefabDocumentSnapshot &source) const {
        if (m_prefabNameFence && m_prefabNameFence->owner == source.fence.owner && m_prefabNameFence->instance == source.fence.instance &&
            m_prefabNameFence->revision == source.fence.revision)
            return;
        m_prefabNameDrafts.resize(source.source->Data().objects.size());
        for (std::size_t index = 0; index < m_prefabNameDrafts.size(); ++index) {
            auto &draft = m_prefabNameDrafts[index];
            draft.fill('\0');
            const auto &name = source.source->Data().objects[index].name;
            std::ranges::copy(name, draft.begin());
        }
        m_prefabNameScope = std::format("{}/{}", source.fence.owner.ToString(), source.fence.instance.Value());
        m_prefabNameFence = source.fence;
    }

    /** @copydoc EditorWorkspaceView::DrawPrefabSourceFields */
    void EditorWorkspaceView::DrawPrefabSourceFields(const PrefabDocumentSnapshot &source,
                                                     EditorWorkspaceViewCommandData &outCommand) const {
        RefreshPrefabNameDrafts(source);
        ImGui::PushID(m_prefabNameScope.c_str());
        ImGui::BeginChild("##prefab_source", {0, ImGui::GetContentRegionAvail().y / 3.0F});
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(source.source->Data().objects.size()));
        while (clipper.Step())
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto slot = static_cast<std::size_t>(index);
                const auto &object = source.source->Data().objects[slot];
                auto &draft = m_prefabNameDrafts[slot];
                std::array<char, 16> id{};
                const auto converted = std::to_chars(id.data(), id.data() + id.size(), object.localId.value);
                ImGui::PushID(id.data(), converted.ptr);
                ImGui::BeginDisabled(source.externalConflict);
                if (Ui::InputTextControl("##name", draft.data(), draft.size(), m_context.theme.fonts,
                                         {.hint = m_context.localization.Get("editor", "workspace.prefab_document.name").c_str(),
                                          .componentSize = Ui::ComponentSize::Medium})) {
                    outCommand.command = EditorWorkspaceViewCommand::PrefabDocument;
                    outCommand.prefabFence = source.fence;
                    outCommand.prefabEdit = RenamePrefabObject{object.localId, std::string{draft.data()}};
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        ImGui::EndChild();
        ImGui::PopID();
    }

    /** @copydoc EditorWorkspaceView::DrawPrefabDocument */
    bool EditorWorkspaceView::DrawPrefabDocument(const EditorWorkspaceViewModel &viewModel,
                                                 EditorWorkspaceViewCommandData &outCommand) const {
        const auto found = std::ranges::find_if(viewModel.prefabDocuments, [&](const auto &entry) {
            return entry.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (found == viewModel.prefabDocuments.end())
            return false;
        const auto &source = *found;
        const auto action = [&](const char *key, const PrefabDocumentAction intent, const bool enabled) {
            if (Ui::Button(
                    {.label = m_context.localization.Get("editor", key).c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans})) {
                outCommand.command = EditorWorkspaceViewCommand::PrefabDocument;
                outCommand.prefabFence = source.fence;
                outCommand.prefabAction = intent;
                if (intent == PrefabDocumentAction::Recover || intent == PrefabDocumentAction::DiscardRecovery)
                    outCommand.prefabRecovery = viewModel.prefabRecovery;
            }
        };
        action("workspace.documents.save", PrefabDocumentAction::Save, source.dirty && !source.externalConflict);
        action("workspace.prefab_document.undo", PrefabDocumentAction::Undo, source.canUndo && !source.externalConflict);
        action("workspace.prefab_document.redo", PrefabDocumentAction::Redo, source.canRedo && !source.externalConflict);
        action("workspace.prefab_document.reload", PrefabDocumentAction::Reload, !source.dirty);
        action("workspace.prefab_document.recover", PrefabDocumentAction::Recover,
               viewModel.prefabRecovery && !source.dirty && viewModel.prefabRecovery->acceptedSource == source.acceptedSource);
        action("workspace.prefab_document.discard_recovery", PrefabDocumentAction::DiscardRecovery, viewModel.prefabRecoveryAvailable);
        action("workspace.prefab_document.discard_reload", PrefabDocumentAction::DiscardAndReload, source.dirty || source.externalConflict);
        action("workspace.prefab_document.discard_close", PrefabDocumentAction::DiscardAndClose, source.dirty);
        if (viewModel.prefabDocumentError)
            Ui::ErrorText(viewModel.prefabDocumentError->message.c_str(), m_context.theme.fonts);
        if (source.externalConflict)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, m_context.localization.Get("editor", "workspace.prefab_document.conflict"),
                            Theme::ConsoleWarning(), m_context.theme.fonts);
        DrawPrefabSourceFields(source, outCommand);
        const auto &panels = m_panelRegistry.GetAllPanels();
        if (const auto viewport = std::ranges::find_if(panels,
                                                       [](const auto &entry) {
            return entry->GetId() == "horo.viewport";
        });
            source.stageActive && viewport != panels.end()) {
            EditorWorkspaceViewCommandData previewOnly;
            GuiLive::InvokeVirtual<&IWorkspacePanel::DrawPanel>(m_context.theme.liveReload, **viewport, ImGui::GetCursorScreenPos(),
                                                                ImGui::GetContentRegionAvail(), viewModel, previewOnly, m_context);
        } else
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            m_context.localization.Get("editor", "workspace.prefab_document.preview_unavailable"), Theme::Muted(),
                            m_context.theme.fonts);
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
        auto intent = widget.Draw({available.x, available.y, Theme::GetActiveTokens().sizes.uiScale, focus, !toolIntent,
                                   &m_context.settings.settings.keymap.profile,
                                   static_cast<float>(m_context.settings.settings.codeFontSizePx), presentation->tools.HandledShortcut(),
                                   &m_context.settings.settings.codeEditor, m_context.settings.settings.codeFontFamily},
                                  &m_context.theme.fonts);
        outCommand.sourceTextInputOwned = toolIntent.has_value() || widget.OwnsInput() || presentation->tools.OwnsInput();
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
