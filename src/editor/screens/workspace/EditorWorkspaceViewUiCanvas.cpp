#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"
#include "editor/screens/workspace/panels/inspector/UiCanvasInspectorPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

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
                for (std::size_t next = index + 1; next < document.rows.size(); ++next) {
                    if (document.rows[next].parent != selected->parent)
                        continue;
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
            if (const bool canCreate = !document.selection.IsValid() || (parentType && parentType->admitsChildren); Ui::Button(
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
                           EditorWorkspaceViewCommandData &output, const float height = 0) {
            const auto &tokens = Theme::GetActiveTokens();
            const float rowHeight =
                DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium).minimumHeight + ImGui::GetStyle().ItemSpacing.y;
            if (ImGui::BeginChild("hierarchy", {0, height})) {
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

    namespace {
        /** @brief Edits finite workspace controls through the shared editor design system. */
        bool DrawPreviewControls(const EditorGuiContext &context, UiCanvasPreviewState &state) {
            const auto label = [&](const char *key) {
                return Text(context, key);
            };
            std::array<std::string, 3> deviceText{label("workspace.ui_canvas.preview.desktop"), label("workspace.ui_canvas.preview.phone"),
                                                  label("workspace.ui_canvas.preview.tablet")};
            std::array<const char *, 3> devices{deviceText[0].c_str(), deviceText[1].c_str(), deviceText[2].c_str()};
            int device = static_cast<int>(state.device);
            bool changed =
                Ui::ComboControl("preview_device", &device, devices.data(), static_cast<int>(devices.size()), context.theme.fonts);
            state.device = static_cast<UiCanvasPreviewDevice>(device);
            changed |= Ui::CheckboxControl(label("workspace.ui_canvas.preview.rotate").c_str(), &state.rotated, context.theme.fonts);
            std::array<std::string, 4> toolText{label("workspace.ui_canvas.preview.select"), label("workspace.ui_canvas.preview.move"),
                                                label("workspace.ui_canvas.preview.resize"), label("workspace.ui_canvas.preview.anchors")};
            std::array<const char *, 4> tools{toolText[0].c_str(), toolText[1].c_str(), toolText[2].c_str(), toolText[3].c_str()};
            int tool = static_cast<int>(state.tool);
            changed |= Ui::ComboControl("preview_tool", &tool, tools.data(), static_cast<int>(tools.size()), context.theme.fonts);
            state.tool = static_cast<UiCanvasLayoutTool>(tool);
            changed |= Ui::CheckboxControl(label("workspace.ui_canvas.preview.snap").c_str(), &state.snap, context.theme.fonts);
            if (Ui::Button({.label = label("workspace.ui_canvas.preview.zoom_out").c_str(),
                            .enabled = state.zoomPercent > 10,
                            .font = context.theme.fonts.sans})) {
                state.zoomPercent = std::max(10U, state.zoomPercent - 10);
                changed = true;
            }
            ImGui::SameLine();
            if (Ui::Button({.label = label("workspace.ui_canvas.preview.zoom_in").c_str(),
                            .enabled = state.zoomPercent < 800,
                            .font = context.theme.fonts.sans})) {
                state.zoomPercent = std::min(800U, state.zoomPercent + 10);
                changed = true;
            }
            if (Ui::Button({.label = label("workspace.ui_canvas.preview.clear_guides").c_str(),
                            .enabled = state.verticalGuideCount != 0 || state.horizontalGuideCount != 0,
                            .font = context.theme.fonts.sans})) {
                state.verticalGuideCount = 0;
                state.horizontalGuideCount = 0;
                changed = true;
            }
            return changed;
        }

        struct PreviewGeometry final {
            ImVec2 origin;
            float scale;
        };

        /** @brief Draws copied Runtime UI layout output and bounded session guides with semantic theme colors. */
        PreviewGeometry DrawPreviewGeometry(const UiCanvasWorkspaceDocument &document, const ImVec2 start, const ImVec2 available) {
            const auto &preview = *document.preview;
            const auto &state = document.previewState;
            const float padding = Theme::GetActiveTokens().spacing.propertyRowGap * 2;
            const float pixelWidth = static_cast<float>(preview.viewport.pixelExtent.width);
            const float pixelHeight = static_cast<float>(preview.viewport.pixelExtent.height);
            const float fit =
                std::max(0.001F, std::min((available.x - padding * 2) / pixelWidth, (available.y - padding * 2) / pixelHeight));
            const float deviceScale = fit * static_cast<float>(state.zoomPercent) / 100;
            const auto safe = preview.canvas.ContentPixelRect();
            const float scale = deviceScale * static_cast<float>(safe.width) / static_cast<float>(preview.canvas.logicalExtent.width);
            const ImVec2 deviceOrigin{start.x + padding + static_cast<float>(state.pan.x) * scale,
                                      start.y + padding + static_cast<float>(state.pan.y) * scale};
            const ImVec2 origin{deviceOrigin.x + static_cast<float>(safe.x) * deviceScale,
                                deviceOrigin.y + static_cast<float>(safe.y) * deviceScale};
            const float widthDip = static_cast<float>(preview.canvas.logicalExtent.width) / 64;
            const float heightDip = static_cast<float>(preview.canvas.logicalExtent.height) / 64;
            const auto point = [&](Runtime::Ui::UiLogicalPoint p) {
                return ImVec2{origin.x + static_cast<float>(p.x) * scale, origin.y + static_cast<float>(p.y) * scale};
            };
            const auto color = ImGui::ColorConvertFloat4ToU32(Theme::Muted());
            const auto accent = ImGui::GetColorU32(ImGuiCol_ButtonActive);
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddRect(deviceOrigin, {deviceOrigin.x + pixelWidth * deviceScale, deviceOrigin.y + pixelHeight * deviceScale}, accent);
            draw->AddRect(origin, {origin.x + widthDip * 64 * scale, origin.y + heightDip * 64 * scale}, color);
            // Rulers use bounded major divisions regardless of zoom or document extent.
            for (int i = 0; i <= 10; ++i) {
                const float x = origin.x + widthDip * 64 * scale * static_cast<float>(i) / 10;
                const float y = origin.y + heightDip * 64 * scale * static_cast<float>(i) / 10;
                draw->AddLine({x, origin.y - padding}, {x, origin.y}, color);
                draw->AddLine({origin.x - padding, y}, {origin.x, y}, color);
            }
            for (std::size_t i = 0; i < state.verticalGuideCount; ++i) {
                const float x = point({state.verticalGuides[i], 0}).x;
                draw->AddLine({x, start.y}, {x, start.y + available.y}, accent);
            }
            for (std::size_t i = 0; i < state.horizontalGuideCount; ++i) {
                const float y = point({0, state.horizontalGuides[i]}).y;
                draw->AddLine({start.x, y}, {start.x + available.x, y}, accent);
            }
            for (const auto &box : preview.boxes) {
                const auto a = point(box.bounds.origin);
                const ImVec2 b{a.x + static_cast<float>(box.bounds.extent.width) * scale,
                               a.y + static_cast<float>(box.bounds.extent.height) * scale};
                draw->AddRect(a, b, box.element == document.selection ? accent : color);
            }
            return {origin, scale};
        }
    }  // namespace

    namespace {
        /** @brief Convert through captured presentation scale with an explicit fixed-point overflow bound. */
        Runtime::Ui::UiScalar PreviewLogicalDelta(const float value, const float scale) {
            return static_cast<Runtime::Ui::UiScalar>(
                std::clamp(std::round(value / scale), static_cast<float>(std::numeric_limits<Runtime::Ui::UiScalar>::min() / 2),
                           static_cast<float>(std::numeric_limits<Runtime::Ui::UiScalar>::max() / 2)));
        }

        /** @brief Add one bounded workspace guide without touching authored layout. */
        bool AddPreviewGuide(UiCanvasPreviewState &state, const ImVec2 mouse, const ImVec2 origin, const float scale) {
            if (ImGui::GetIO().KeyCtrl && state.horizontalGuideCount < state.horizontalGuides.size()) {
                state.horizontalGuides[state.horizontalGuideCount++] = PreviewLogicalDelta(mouse.y - origin.y, scale);
                return true;
            }
            if (!ImGui::GetIO().KeyCtrl && state.verticalGuideCount < state.verticalGuides.size()) {
                state.verticalGuides[state.verticalGuideCount++] = PreviewLogicalDelta(mouse.x - origin.x, scale);
                return true;
            }
            return false;
        }

        /** @brief Apply a completed pan with saturating session coordinates. */
        void OffsetPreviewPan(UiCanvasPreviewState &state, const Runtime::Ui::UiLogicalPoint delta) {
            const auto offset = [](const auto value, const auto change) {
                return static_cast<Runtime::Ui::UiScalar>(
                    std::clamp(static_cast<std::int64_t>(value) + change,
                               static_cast<std::int64_t>(std::numeric_limits<Runtime::Ui::UiScalar>::min()),
                               static_cast<std::int64_t>(std::numeric_limits<Runtime::Ui::UiScalar>::max())));
            };
            state.pan = {offset(state.pan.x, delta.x), offset(state.pan.y, delta.y)};
        }
    }  // namespace

    /** @copydoc EditorWorkspaceView::HandleUiCanvasPreviewPress */
    void EditorWorkspaceView::HandleUiCanvasPreviewPress(const UiCanvasWorkspaceDocument &document, const ImVec2 origin, const float scale,
                                                         const bool changed, EditorWorkspaceViewCommandData &output) const {
        if (changed || !ImGui::IsItemHovered())
            return;
        const auto mouse = ImGui::GetIO().MousePos;
        const auto capture = [&](const bool pan) {
            m_uiCanvasGesture = UiCanvasGesture{document.identity.instance,
                                                document.state,
                                                document.selection,
                                                document.selectionRevision,
                                                mouse,
                                                scale,
                                                document.previewState,
                                                pan};
        };
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            capture(true);
            return;
        }
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            return;
        if (ImGui::GetIO().KeyShift) {
            auto state = document.previewState;
            if (AddPreviewGuide(state, mouse, origin, scale)) {
                output.command = EditorWorkspaceViewCommand::UiCanvasUpdatePreview;
                output.uiCanvasPreviewState = state;
            }
            return;
        }
        const auto &boxes = document.preview->boxes;
        const auto hit = std::find_if(boxes.rbegin(), boxes.rend(), [&](const auto &box) {
            const float x = origin.x + static_cast<float>(box.bounds.origin.x) * scale;
            const float y = origin.y + static_cast<float>(box.bounds.origin.y) * scale;
            return mouse.x >= x && mouse.y >= y && mouse.x <= x + static_cast<float>(box.bounds.extent.width) * scale &&
                   mouse.y <= y + static_cast<float>(box.bounds.extent.height) * scale;
        });
        if (hit == boxes.rend())
            return;
        if (hit->element != document.selection) {
            output.command = EditorWorkspaceViewCommand::UiCanvasSelect;
            output.uiCanvasElement = hit->element;
        } else if (document.previewState.tool != UiCanvasLayoutTool::Select)
            capture(false);
    }

    /** @copydoc EditorWorkspaceView::FinishUiCanvasGesture */
    void EditorWorkspaceView::FinishUiCanvasGesture(const UiCanvasWorkspaceDocument &document,
                                                    EditorWorkspaceViewCommandData &output) const {
        if (!m_uiCanvasGesture || !ImGui::IsMouseReleased(m_uiCanvasGesture->pan ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left))
            return;
        const auto gesture = *m_uiCanvasGesture;
        m_uiCanvasGesture.reset();
        const auto mouse = ImGui::GetIO().MousePos;
        if (mouse.x == gesture.start.x && mouse.y == gesture.start.y)
            return;
        output.command = EditorWorkspaceViewCommand::UiCanvasCommitLayout;
        output.uiCanvasElement = gesture.element;
        output.uiCanvasSelectionRevision = gesture.selectionRevision;
        output.uiCanvasLayoutDelta = {PreviewLogicalDelta(mouse.x - gesture.start.x, gesture.scale),
                                      PreviewLogicalDelta(mouse.y - gesture.start.y, gesture.scale)};
        if (gesture.pan) {
            auto state = document.previewState;
            OffsetPreviewPan(state, output.uiCanvasLayoutDelta);
            output.command = EditorWorkspaceViewCommand::UiCanvasUpdatePreview;
            output.uiCanvasPreviewState = state;
        }
    }

    /** @copydoc EditorWorkspaceView::DrawUiCanvasPreview */
    void EditorWorkspaceView::DrawUiCanvasPreview(const UiCanvasWorkspaceDocument &document, EditorWorkspaceViewCommandData &output) const {
        auto state = document.previewState;
        const auto label = [&](const char *key) {
            return Text(m_context, key);
        };
        const bool changed = DrawPreviewControls(m_context, state);
        if (changed) {
            output.command = EditorWorkspaceViewCommand::UiCanvasUpdatePreview;
            output.uiCanvasPreviewState = state;
            m_uiCanvasGesture.reset();
        }
        if (document.previewDiagnostic || !document.preview) {
            Ui::ErrorText(label("workspace.ui_canvas.preview.unavailable").c_str(), m_context.theme.fonts);
            m_uiCanvasGesture.reset();
            return;
        }
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, label("workspace.ui_canvas.preview.hint"), Theme::Muted(), m_context.theme.fonts);
        const float height = ImGui::GetContentRegionAvail().y * 0.45F;
        if (height <= 0)
            return;
        if (!ImGui::BeginChild("canvas_preview", {0, height})) {
            ImGui::EndChild();
            return;
        }
        const auto start = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        const auto geometry = DrawPreviewGeometry(document, start, available);
        ImGui::InvisibleButton("canvas_interaction", available);
        if (m_uiCanvasGesture &&
            (m_uiCanvasGesture->document != document.identity.instance || m_uiCanvasGesture->state != document.state ||
             m_uiCanvasGesture->element != document.selection || m_uiCanvasGesture->selectionRevision != document.selectionRevision ||
             m_uiCanvasGesture->preview != state || m_uiCanvasGesture->scale != geometry.scale || ImGui::IsKeyPressed(ImGuiKey_Escape)))
            m_uiCanvasGesture.reset();
        HandleUiCanvasPreviewPress(document, geometry.origin, geometry.scale, changed, output);
        FinishUiCanvasGesture(document, output);
        ImGui::EndChild();
    }

    /** @copydoc EditorWorkspaceView::DrawUiCanvasDocument */
    bool EditorWorkspaceView::DrawUiCanvasDocument(const EditorWorkspaceViewModel &viewModel,
                                                   EditorWorkspaceViewCommandData &outCommand) const {
        const auto document = std::ranges::find_if(viewModel.uiCanvasDocuments, [&](const auto &entry) {
            return entry.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (document == viewModel.uiCanvasDocuments.end()) {
            m_uiCanvasInspector->Clear();
            m_uiCanvasGesture.reset();
            return false;
        }
        ImGui::PushID(static_cast<int>(document->identity.instance.Value()));
        DrawToolbar(m_context, *document, outCommand);
        DrawPalette(m_context, *document, outCommand);
        DrawUiCanvasPreview(*document, outCommand);
        const float available = ImGui::GetContentRegionAvail().x;
        const float minimumColumn = ImGui::CalcTextSize(Text(m_context, "workspace.ui_inspector.title").c_str()).x +
                                    Theme::GetActiveTokens().spacing.propertyRowGap * 2;
        if (available >= minimumColumn * 2 &&
            ImGui::BeginTable("ui_canvas_inspector_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableNextColumn();
            DrawHierarchy(m_context, *document, outCommand);
            ImGui::TableNextColumn();
            if (ImGui::BeginChild("ui_inspector"))
                m_uiCanvasInspector->Draw(*document, m_context, outCommand);
            ImGui::EndChild();
            ImGui::EndTable();
        } else {
            DrawHierarchy(m_context, *document, outCommand, ImGui::GetContentRegionAvail().y * 0.35F);
            if (ImGui::BeginChild("ui_inspector"))
                m_uiCanvasInspector->Draw(*document, m_context, outCommand);
            ImGui::EndChild();
        }
        ImGui::PopID();
        if (outCommand.command >= EditorWorkspaceViewCommand::UiCanvasSelect &&
            outCommand.command <= EditorWorkspaceViewCommand::UiCanvasReload) {
            outCommand.documentInstance = document->identity.instance;
            outCommand.uiCanvasState = document->state;
        }
        return true;
    }
}  // namespace Horo::Editor
