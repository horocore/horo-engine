#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Horo::Editor {
    namespace {
        /** @brief Exact source-session fence copied by ImGui only for the active drag lifetime. */
        struct CanvasDrag final {
            DocumentInstanceId instance;
            UiCanvasDocumentStateId state;
            Runtime::Ui::UiElementId element;
        };

        /** @brief Emits sibling ordering using stable IDs from the complete unfiltered projection. */
        void Reorder(const UiCanvasWorkspaceDocument &document, const bool earlier, EditorWorkspaceViewCommandData &output) {
            const auto selected = std::ranges::find(document.rows, document.selection, &UiCanvasHierarchyRow::element);
            if (selected == document.rows.end() || !selected->parent.IsValid() || !document.search.empty())
                return;
            const auto index = static_cast<std::size_t>(selected - document.rows.begin());
            if (earlier) {
                for (std::size_t previous = index; previous > 0; --previous)
                    if (document.rows[previous - 1].parent == selected->parent) {
                        output.command = EditorWorkspaceViewCommand::UiCanvasMove;
                        output.uiCanvasMove = UiCanvasMoveElement{selected->element, selected->parent, document.rows[previous - 1].element};
                        return;
                    }
            } else {
                bool passedNext = false;
                for (std::size_t next = index + 1; next < document.rows.size(); ++next)
                    if (document.rows[next].parent == selected->parent) {
                        if (passedNext) {
                            output.command = EditorWorkspaceViewCommand::UiCanvasMove;
                            output.uiCanvasMove = UiCanvasMoveElement{selected->element, selected->parent, document.rows[next].element};
                            return;
                        }
                        passedNext = true;
                    }
                if (passedNext) {
                    output.command = EditorWorkspaceViewCommand::UiCanvasMove;
                    output.uiCanvasMove = UiCanvasMoveElement{selected->element, selected->parent, {}};
                }
            }
        }

        /** @brief Resolves canvas copy through the host localization service. */
        std::string Text(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key);
        }

        /** @brief Emits one typed canvas toolbar gesture using the shared button. */
        void ToolbarButton(const EditorGuiContext &context, const char *key, const EditorWorkspaceViewCommand command, const bool enabled,
                           EditorWorkspaceViewCommandData &output) {
            if (Ui::Button({.label = Text(context, key).c_str(), .enabled = enabled, .font = context.theme.fonts.sans}))
                output.command = command;
        }

        /** @brief Presents persistence/history actions and the bounded transient search draft. */
        void DrawToolbar(const EditorGuiContext &context, const UiCanvasWorkspaceDocument &document,
                         EditorWorkspaceViewCommandData &output) {
            using enum EditorWorkspaceViewCommand;
            ToolbarButton(context, "workspace.documents.save", UiCanvasSave, document.dirty, output);
            ImGui::SameLine();
            ToolbarButton(context, "workspace.ui_canvas.undo", UiCanvasUndo, document.canUndo, output);
            ImGui::SameLine();
            ToolbarButton(context, "workspace.ui_canvas.redo", UiCanvasRedo, document.canRedo, output);
            ToolbarButton(context, "workspace.ui_canvas.reload", UiCanvasReload, !document.dirty, output);
            if (document.diagnostic)
                Ui::ErrorText(Text(context, "workspace.ui_canvas.command_failed").c_str(), context.theme.fonts);
            std::array<char, 257> search{};
            std::copy_n(document.search.begin(), std::min(document.search.size(), search.size() - 1), search.begin());
            const auto hint = Text(context, "workspace.ui_canvas.search");
            if (Ui::InputTextControl("search", search.data(), search.size(), context.theme.fonts, {.hint = hint.c_str()})) {
                output.command = UiCanvasSearch;
                output.stringPayload = search.data();
            }
        }

        /** @brief Enumerates the canonical descriptor palette and emits stable-neighbor ordering gestures. */
        void DrawPalette(const EditorGuiContext &context, const UiCanvasWorkspaceDocument &document,
                         EditorWorkspaceViewCommandData &output) {
            const auto selected = std::ranges::find(document.rows, document.selection, &UiCanvasHierarchyRow::element);
            const auto *parentType = selected == document.rows.end() ? nullptr : Runtime::Ui::UiElementCatalog::Find(selected->label);
            const bool canCreate = !document.selection.IsValid() || (parentType && parentType->admitsChildren);
            if (Ui::Button(
                    {.label = Text(context, "workspace.ui_canvas.create").c_str(), .enabled = canCreate, .font = context.theme.fonts.sans}))
                ImGui::OpenPopup("palette");
            if (Ui::BeginMenuPopup("palette")) {
                for (const auto &descriptor : Runtime::Ui::UiElementCatalog::All()) {
                    if (descriptor.kind == Runtime::Ui::UiElementKind::Canvas)
                        continue;
                    const auto label = context.localization.Get("editor", descriptor.localizationKey);
                    if (Ui::ContextMenuItem(label.c_str(), nullptr, context.theme.fonts)) {
                        output.command = EditorWorkspaceViewCommand::UiCanvasCreate;
                        output.uiCanvasKind = descriptor.kind;
                        output.uiCanvasElement = document.selection.IsValid() ? document.selection : document.root;
                    }
                }
                Ui::EndMenuPopup();
            }
            const bool movable = selected != document.rows.end() && selected->parent.IsValid() && document.search.empty();
            if (Ui::Button(
                    {.label = Text(context, "workspace.ui_canvas.move_up").c_str(), .enabled = movable, .font = context.theme.fonts.sans}))
                Reorder(document, true, output);
            ImGui::SameLine();
            if (Ui::Button({.label = Text(context, "workspace.ui_canvas.move_down").c_str(),
                            .enabled = movable,
                            .font = context.theme.fonts.sans}))
                Reorder(document, false, output);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "workspace.ui_canvas.reparent_hint"), Theme::Muted(),
                            context.theme.fonts);
        }

        /** @brief Copies exact drag evidence and emits a reparent gesture only within its unchanged source session. */
        void DrawDrag(const EditorGuiContext &context, const UiCanvasWorkspaceDocument &document, const UiCanvasHierarchyRow &row,
                      const Runtime::Ui::UiElementTypeDescriptor *type, const std::string &label, EditorWorkspaceViewCommandData &output) {
            if (row.parent.IsValid() && ImGui::BeginDragDropSource()) {
                const CanvasDrag payload{document.identity.instance, document.state, row.element};
                ImGui::SetDragDropPayload("HORO_UI_CANVAS_ELEMENT", &payload, sizeof(payload));
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, label, Theme::Text(), context.theme.fonts);
                ImGui::EndDragDropSource();
            }
            if (type && type->admitsChildren && ImGui::BeginDragDropTarget()) {
                if (const auto *payload = ImGui::AcceptDragDropPayload("HORO_UI_CANVAS_ELEMENT");
                    payload && payload->DataSize == static_cast<int>(sizeof(CanvasDrag))) {
                    CanvasDrag drag;
                    std::memcpy(&drag, payload->Data, sizeof(drag));
                    if (drag.instance == document.identity.instance && drag.state == document.state) {
                        output.command = EditorWorkspaceViewCommand::UiCanvasMove;
                        output.uiCanvasMove = UiCanvasMoveElement{drag.element, row.element, {}};
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        /** @brief Draws one visible hierarchy row, leaving mutations with its typed command receiver. */
        void DrawRow(const EditorGuiContext &context, const UiCanvasWorkspaceDocument &document, const UiCanvasHierarchyRow &row,
                     const float rowHeight, EditorWorkspaceViewCommandData &output) {
            const auto &bytes = row.element.Bytes();
            const auto *id = reinterpret_cast<const char *>(bytes.data());
            ImGui::PushID(id, id + bytes.size());
            const float indent = std::min(static_cast<float>(row.depth) * Theme::GetActiveTokens().spacing.propertyRowGap,
                                          std::max(0.0F, ImGui::GetContentRegionAvail().x * 0.5F));
            ImGui::Indent(indent);
            const auto *type = Runtime::Ui::UiElementCatalog::Find(row.label);
            const auto label = type ? context.localization.Get("editor", type->localizationKey) : row.label;
            if (Ui::Button({.label = label.c_str(),
                            .size = {ImGui::GetContentRegionAvail().x, rowHeight - ImGui::GetStyle().ItemSpacing.y},
                            .variant = row.element == document.selection ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                            .font = context.theme.fonts.sans})) {
                output.command = EditorWorkspaceViewCommand::UiCanvasSelect;
                output.uiCanvasElement = row.element;
            }
            DrawDrag(context, document, row, type, label, output);
            ImGui::Unindent(indent);
            ImGui::PopID();
        }

        /** @brief Clips cached rows to the visible region without rebuilding source topology during draw work. */
        void DrawHierarchy(const EditorGuiContext &context, const UiCanvasWorkspaceDocument &document,
                           EditorWorkspaceViewCommandData &output) {
            const auto &tokens = Theme::GetActiveTokens();
            const float rowHeight =
                DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium).minimumHeight + ImGui::GetStyle().ItemSpacing.y;
            if (ImGui::BeginChild("hierarchy", {0, 0})) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(document.rows.size()), rowHeight);
                while (clipper.Step())
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                        DrawRow(context, document, document.rows[static_cast<std::size_t>(index)], rowHeight, output);
                if (document.rows.empty())
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "workspace.ui_canvas.no_results"), Theme::Muted(),
                                    context.theme.fonts);
            }
            ImGui::EndChild();
        }
    }  // namespace

    /** @copydoc EditorWorkspaceView::DrawUiCanvasDocument */
    bool EditorWorkspaceView::DrawUiCanvasDocument(const EditorWorkspaceViewModel &viewModel,
                                                   EditorWorkspaceViewCommandData &outCommand) const {
        const auto document = std::ranges::find_if(viewModel.uiCanvasDocuments, [&](const auto &entry) {
            return entry.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (document == viewModel.uiCanvasDocuments.end())
            return false;
        ImGui::PushID(static_cast<int>(document->identity.instance.Value()));
        DrawToolbar(m_context, *document, outCommand);
        DrawPalette(m_context, *document, outCommand);
        DrawHierarchy(m_context, *document, outCommand);
        ImGui::PopID();
        if (outCommand.command >= EditorWorkspaceViewCommand::UiCanvasSelect &&
            outCommand.command <= EditorWorkspaceViewCommand::UiCanvasReload) {
            outCommand.documentInstance = document->identity.instance;
            outCommand.uiCanvasState = document->state;
        }
        return true;
    }
}  // namespace Horo::Editor
