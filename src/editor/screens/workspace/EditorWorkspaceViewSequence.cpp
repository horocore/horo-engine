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
    namespace {
        /** @brief Selects the exact byte range of an active script diagnostic. */
        bool DrawScriptDiagnosticJump(const EditorGuiContext &context, const SourceDocumentSnapshot &source,
                                      const std::shared_ptr<const Language::LanguageResult> &publication, SourceEditorWidget &widget) {
            if (!publication || !source.IsCurrent() || !source.SharesStateWith(publication->context.source) ||
                ClassifySourceLanguage(source.Identity().key.source.Value()) != SourceLanguage::HoroScript)
                return false;
            const auto *diagnostics = std::get_if<Language::LanguageDiagnostics>(&publication->payload);
            if (!diagnostics || diagnostics->items.empty())
                return false;
            const auto &title = context.localization.Get("editor", "workspace.source_language.script_diagnostics");
            std::string label;
            const Ui::ComboItemSource labels{.label = [&](const int index) {
                if (index == 0)
                    return title.c_str();
                const auto &item = diagnostics->items[static_cast<std::size_t>(index - 1)];
                label = std::format("{}. {}", index, item.message);
                return label.c_str();
            }};
            int choice = 0;
            if (!Ui::ComboControl("##ScriptDiagnostics", &choice, static_cast<int>(diagnostics->items.size()) + 1, labels,
                                  context.theme.fonts) ||
                choice == 0)
                return false;
            const auto span = diagnostics->items[static_cast<std::size_t>(choice - 1)].span;
            return widget.Select({span.start, span.end});
        }

        /** @brief Resolves the localized notice for the active source revision. */
        const char *LanguageNotice(const SourceLanguageStatusKind kind) {
            switch (kind) {
                case SourceLanguageStatusKind::Waiting:
                    return "workspace.source_language.waiting";
                case SourceLanguageStatusKind::Refused:
                    return "workspace.source_language.retry";
                case SourceLanguageStatusKind::NoDefinition:
                    return "workspace.source_language.no_definition";
                case SourceLanguageStatusKind::NoCompletion:
                    return "workspace.source_language.no_completion";
                case SourceLanguageStatusKind::NoSymbol:
                    return "workspace.source_language.no_symbol";
                case SourceLanguageStatusKind::ScriptModuleUnavailable:
                    return "workspace.source_language.script_module_unavailable";
            }
            return "workspace.source_language.retry";
        }

        /** @brief Displays a typed result set in the shared dropdown at the originating view selection. */
        void DrawSourceCompletions(const EditorGuiContext &context, const SourceDocumentSnapshot &source, const SourceEditingView &view,
                                   const std::shared_ptr<const SourceLanguageCompletions> &result,
                                   EditorWorkspaceViewCommandData &command) {
            if (!result || !result->source.SharesStateWith(source) || result->view.viewId != view.viewId ||
                result->view.anchor != view.anchor || result->view.caret != view.caret)
                return;
            const auto &items = result->completions.items;
            const auto &placeholder = context.localization.Get("editor", "workspace.source_language.choose_completion");
            int choice = 0;
            const Ui::ComboItemSource labels{.label = [&](const int index) {
                return index == 0 ? placeholder.c_str() : items[static_cast<std::size_t>(index - 1)].label.c_str();
            }};
            if (Ui::ComboControl("##SourceCompletions", &choice, static_cast<int>(items.size()) + 1, labels, context.theme.fonts) &&
                choice > 0 && command.command == EditorWorkspaceViewCommand::None) {
                command.command = EditorWorkspaceViewCommand::ApplySourceCompletion;
                command.sourceCompletionIntent = SourceCompletionIntent{result, static_cast<std::size_t>(choice - 1)};
            }
        }

        /** @brief Offers the actions supported by the exact registered source adapter. */
        void DrawSourceLanguageActions(const EditorGuiContext &context, const SourceDocumentSnapshot &source,
                                       const SourceEditorWidget &widget, const EditorWorkspaceViewModel &model,
                                       EditorWorkspaceViewCommandData &command) {
            const auto identity = source.Identity();
            const auto family = ClassifySourceLanguage(identity.key.source.Value());
            const auto script = family == SourceLanguage::HoroScript &&
                                Language::FindScriptModuleSourceBinding(model.sourceScriptBindings, identity.key.source);
            if (!script && family != SourceLanguage::Cpp)
                return;
            const auto notice = [&](const char *key) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, context.localization.Get("editor", key), Theme::Muted(),
                                context.theme.fonts);
            };
            if (!script && model.sourceLanguageUnavailable) {
                notice("workspace.source_language.unavailable");
                return;
            }
            if (!script && !model.sourceLanguageConfigured)
                return;
            if (model.sourceLanguageStatus && model.sourceLanguageStatus->source.SharesStateWith(source))
                notice(LanguageNotice(model.sourceLanguageStatus->kind));
            const auto view = widget.View();
            const std::array labels{context.localization
                                        .Get("editor",
                                             script ? "workspace.source_language.script_actions" : "workspace.source_language.actions")
                                        .c_str(),
                                    context.localization
                                        .Get("editor",
                                             script ? "workspace.source_language.describe_symbol" : "workspace.source_language.definition")
                                        .c_str(),
                                    context.localization.Get("editor", "workspace.source_language.complete").c_str()};
            const std::array kinds{script ? SourceLanguageRequestKind::Hover : SourceLanguageRequestKind::Definition,
                                   SourceLanguageRequestKind::Completion};
            int action = 0;
            if (Ui::ComboControl("##SourceLanguageActions", &action, labels.data(), static_cast<int>(labels.size()), context.theme.fonts) &&
                action > 0 && command.command == EditorWorkspaceViewCommand::None) {
                command.command = EditorWorkspaceViewCommand::RequestSourceLanguage;
                command.sourceLanguageIntent = SourceLanguageIntent{source, kinds[static_cast<std::size_t>(action - 1)], view};
            }
            DrawSourceCompletions(context, source, view, model.sourceCompletions, command);
            if (script && model.sourceScriptHover && model.sourceScriptHover->source.SharesStateWith(source) &&
                model.sourceScriptHover->view.viewId == view.viewId && model.sourceScriptHover->view.caret == view.caret) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, model.sourceScriptHover->text, Theme::Muted(), context.theme.fonts);
            }
        }
    }  // namespace

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
             DrawSequenceDocument(viewModel, outCommand) || DrawBlackboardDocument(viewModel, outCommand) ||
             DrawEnvironmentQueryDocument(viewModel, outCommand) || DrawDecisionGraphDocument(viewModel, outCommand))) {
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
            if (const auto action = Ui::DocumentTabButton("document", title.c_str(),
                                                          m_context.localization.Get("editor", "workspace.documents.close").c_str(),
                                                          active == instance, m_context.theme.fonts);
                action != Ui::DocumentTabAction::None) {
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
                if (intent == PrefabDocumentAction::Reload || intent == PrefabDocumentAction::DiscardAndReload)
                    outCommand.prefabExternalComparison = source.externalComparison;
            }
        };
        action("workspace.documents.save", PrefabDocumentAction::Save, source.dirty && !source.externalConflict);
        action("workspace.prefab_document.save_as", PrefabDocumentAction::SaveAs, source.dirty);
        action("workspace.prefab_document.undo", PrefabDocumentAction::Undo, source.canUndo && !source.externalConflict);
        action("workspace.prefab_document.redo", PrefabDocumentAction::Redo, source.canRedo && !source.externalConflict);
        action("workspace.prefab_document.reload", PrefabDocumentAction::Reload, !source.dirty);
        if (source.externalConflict)
            action("workspace.prefab_document.compare", PrefabDocumentAction::CompareExternal,
                   static_cast<bool>(source.externalComparison));
        action("workspace.prefab_document.recover", PrefabDocumentAction::Recover,
               viewModel.prefabRecovery && !source.dirty && viewModel.prefabRecovery->acceptedSource == source.acceptedSource);
        action("workspace.prefab_document.discard_recovery", PrefabDocumentAction::DiscardRecovery, viewModel.prefabRecoveryAvailable);
        action("workspace.prefab_document.discard_reload", PrefabDocumentAction::DiscardAndReload, source.dirty || source.externalConflict);
        action("workspace.prefab_document.discard_close", PrefabDocumentAction::DiscardAndClose, source.dirty);
        if (viewModel.prefabDocumentError)
            Ui::ErrorText(viewModel.prefabDocumentError->message.c_str(), m_context.theme.fonts);
        if (viewModel.prefabPostCommitError)
            Ui::ErrorText(m_context.localization.Get("editor", "workspace.prefab_document.save_as_refresh").c_str(), m_context.theme.fonts);
        if (source.externalConflict)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            m_context.localization.Get("editor", source.keepExternalDraft ? "workspace.prefab_document.kept"
                                                                                          : "workspace.prefab_document.conflict"),
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
        const auto diagnostics = viewModel.sourceLanguageDiagnostics.find(instance);
        const auto projection = diagnostics == viewModel.sourceLanguageDiagnostics.end() ? nullptr : diagnostics->second;
        presentation->PublishDiagnostics(source, projection);
        if (viewModel.sourceEditingResult && viewModel.sourceEditingResult->HasValue())
            static_cast<void>(widget.Restore(viewModel.sourceEditingResult->Value()));
        const bool languageFocus = presentation->ApplyLanguageFocus(source, viewModel.sourceLanguageFocus);
        const bool scriptFocus = DrawScriptDiagnosticJump(m_context, source, projection, widget);
        DrawSourceLanguageActions(m_context, source, widget, viewModel, outCommand);
        auto toolIntent = presentation->tools.Draw(widget, source, history, viewModel.sourceEditingResult, m_context);
        const bool focus =
            languageFocus || scriptFocus || m_sourceSession.NeedsFocus(instance) || (!toolIntent && presentation->tools.TakeFocusRequest());
        const auto available = ImGui::GetContentRegionAvail();
        auto intent = widget.Draw({available.x, available.y, Theme::GetActiveTokens().sizes.uiScale, focus, !toolIntent,
                                   m_context.inputRouter ? &m_context.inputRouter->Profile() : &m_context.settings.settings.keymap.profile,
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
