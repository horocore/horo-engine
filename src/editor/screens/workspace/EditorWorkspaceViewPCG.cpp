#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/pcg/PCGAuthoringInspector.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace Horo::Editor {
    namespace {
        /** @brief Thin frame adapter; allocates a detached presentation request only when input changes. */
        struct CanvasFrame final {
            const PCGWorkspaceDocument &document;
            std::span<const PCGPaletteEntry> palette;
            const ILocalizationService &localization;
            const Theme::Fonts &fonts;
            std::optional<PCGEditorRequest> request;
            const PCG::PCGNodeCatalogSnapshot *schemas{};
            PCGAuthoringInspectorState *inspector{};

            std::string Text(const char *key) const {
                return localization.Get("editor", key);
            }

            const PCGCanvasState &State() const {
                return request ? request->presentation : document.canvas;
            }

            PCGEditorRequest &Edit() {
                if (!request)
                    request = PCGEditorRequest{.fence = {.session = document.snapshot.session,
                                                         .expectedRevision = document.snapshot.revision,
                                                         .catalogRevision = document.snapshot.catalogRevision,
                                                         .canEdit = true},
                                               .presentation = document.canvas};
                return *request;
            }

            bool Button(const char *key, bool enabled = true, bool selected = false) const {
                return Ui::Button({.label = Text(key).c_str(),
                                   .variant = selected ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                                   .enabled = enabled,
                                   .font = fonts.sans});
            }

            std::string NodeLabel(const PCG::PCGGraphNode &node) const {
                const auto entry = std::ranges::find(palette, node.type, [](const auto &value) {
                    return value.prototype.type;
                });
                return entry == palette.end() ? Text("workspace.pcg.unknown") : localization.Get("editor", entry->labelKey);
            }
        };

        /** @brief Performs toolbar navigation and source operations with shared keyboard-focusable controls. */
        void DrawTools(CanvasFrame &frame) {
            if (frame.Button("workspace.documents.save", frame.document.snapshot.dirty))
                frame.Edit().action = PCGEditorAction::Save;
            ImGui::SameLine();
            if (frame.Button("workspace.pcg.undo"))
                frame.Edit().action = PCGEditorAction::Undo;
            ImGui::SameLine();
            if (frame.Button("workspace.pcg.redo"))
                frame.Edit().action = PCGEditorAction::Redo;
            if (frame.Button("workspace.pcg.reset_view")) {
                auto &state = frame.Edit().presentation;
                state.pan = {};
                state.zoom = 1;
            }
            ImGui::SameLine();
            if (frame.Button("workspace.pcg.focus", !frame.State().selection.empty())) {
                const auto &state = frame.State();
                const auto found = std::ranges::find(state.placements, state.selection.front(), &PCGNodePlacement::node);
                if (found != state.placements.end())
                    frame.Edit().presentation.pan = {-found->position.x * state.zoom, -found->position.y * state.zoom};
            }
            if (float zoom = frame.State().zoom; Ui::DrawFloatPropRow(frame.Text("workspace.pcg.zoom").c_str(), "##zoom", zoom, frame.fonts,
                                                                      {.speed = 0.01F, .minimum = 0.25F, .maximum = 2.0F})
                                                     .changed)
                frame.Edit().presentation.zoom = std::clamp(zoom, 0.25F, 2.0F);
            Ui::Hint(frame.Text("workspace.pcg.navigation_hint").c_str(), frame.fonts);
        }

        /** @brief Presents host recipes without deriving schemas from graph nodes or modifying source containers. */
        void DrawPalette(CanvasFrame &frame) {
            Ui::SectionTitle(frame.Text("workspace.pcg.palette").c_str(), frame.fonts);
            std::array<char, 129> filter{};
            const auto &current = frame.State().filter;
            std::ranges::copy(current, filter.begin());
            if (Ui::InputTextControl("##palette_filter", filter.data(), filter.size(), frame.fonts,
                                     {.hint = frame.Text("workspace.pcg.filter").c_str()}))
                frame.Edit().presentation.filter = filter.data();
            for (const auto &entry : frame.palette) {
                const auto label = frame.localization.Get("editor", entry.labelKey);
                if (!frame.State().filter.empty() && label.find(frame.State().filter) == std::string::npos)
                    continue;
                const auto id = std::to_string(entry.prototype.type.Value());
                ImGui::PushID(id.c_str());
                if (Ui::Button({.label = label.c_str(),
                                .variant = Ui::ButtonVariant::Secondary,
                                .font = frame.fonts.sans,
                                .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                    auto &request = frame.Edit();
                    request.action = PCGEditorAction::CreateNode;
                    request.type = entry.prototype.type;
                }
                ImGui::PopID();
            }
            if (frame.palette.empty())
                Ui::Hint(frame.Text("workspace.pcg.no_catalog").c_str(), frame.fonts);
        }

        /** @brief Resolves an immutable pin's graph-space anchor for rendering directed edges. */
        ImVec2 PinAnchor(const CanvasFrame &frame, PCG::NodeId nodeId, PCG::PinId pinId, ImVec2 origin, float row, float width) {
            const auto &data = frame.document.snapshot.source->Data();
            const auto node = std::ranges::find(data.nodes, nodeId, &PCG::PCGGraphNode::id);
            const auto placement = std::ranges::find(frame.State().placements, nodeId, &PCGNodePlacement::node);
            if (node == data.nodes.end() || placement == frame.State().placements.end())
                return origin;
            const auto pin = std::ranges::find(node->pins, pinId, &PCG::PCGGraphPin::id);
            if (pin == node->pins.end())
                return origin;
            const float x = placement->position.x * frame.State().zoom;
            const float y = placement->position.y * frame.State().zoom;
            return {origin.x + frame.State().pan.x + x + (pin->direction == PCG::PCGPinDirection::Output ? width : 0),
                    origin.y + frame.State().pan.y + y + row * (1.5F + static_cast<float>(pin - node->pins.begin()))};
        }

        /** @brief Selects stable node identities; Ctrl extends the bounded selection without semantic mutation. */
        void SelectNode(CanvasFrame &frame, PCG::NodeId id) {
            auto &selected = frame.Edit().presentation.selection;
            if (!ImGui::GetIO().KeyCtrl)
                selected.clear();
            const auto found = std::ranges::find(selected, id);
            if (found != selected.end())
                selected.erase(found);
            else if (selected.size() < 256)
                selected.push_back(id);
        }

        /** @brief Draws typed pin endpoints; a completed connection is one exact semantic command. */
        void DrawPin(CanvasFrame &frame, const PCG::PCGGraphNode &node, const PCG::PCGGraphPin &pin, float width) {
            constexpr std::array types{"points", "boolean", "signed", "unsigned", "scalar", "vector2", "vector3", "vector4"};
            const bool output = pin.direction == PCG::PCGPinDirection::Output;
            const auto direction = frame.Text(output ? "workspace.pcg.output" : "workspace.pcg.input");
            const auto type =
                frame.localization.Get("editor", std::string{"workspace.pcg.types."} + types[static_cast<std::size_t>(pin.type)]);
            const auto label = std::format("{} {} ({})###pin{}", direction, type, pin.id.Value(), pin.id.Value());
            if (Ui::Button({.label = label.c_str(),
                            .size = {width / Theme::GetActiveTokens().sizes.uiScale, 0},
                            .variant = frame.State().connectingPin == pin.id ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                            .enabled = output || frame.State().connectingPin.has_value(),
                            .font = frame.fonts.sans})) {
                auto &request = frame.Edit();
                if (output) {
                    request.presentation.connectingNode = node.id;
                    request.presentation.connectingPin = pin.id;
                } else {
                    request.action = PCGEditorAction::Connect;
                    request.sourceNode = *request.presentation.connectingNode;
                    request.sourcePin = *request.presentation.connectingPin;
                    request.targetNode = node.id;
                    request.targetPin = pin.id;
                }
            }
            if (ImGui::IsItemHovered())
                Ui::ShowTooltip(label.substr(0, label.find("###")).c_str(), &frame.fonts);
        }

        /** @brief Cancels a workspace-only drag or connection; no command/history entry was submitted. */
        void CancelGesture(CanvasFrame &frame) {
            CancelPCGCanvasGesture(frame.Edit().presentation);
        }

        /** @brief Draws one node using shared controls and theme tokens; only presentation moves during a drag. */
        void DrawNode(CanvasFrame &frame, const PCG::PCGGraphNode &node, ImVec2 origin, float row, float width) {
            const auto placement = std::ranges::find(frame.State().placements, node.id, &PCGNodePlacement::node);
            if (placement == frame.State().placements.end())
                return;
            const ImVec2 position{origin.x + frame.State().pan.x + placement->position.x * frame.State().zoom,
                                  origin.y + frame.State().pan.y + placement->position.y * frame.State().zoom};
            const float height = row * (1 + static_cast<float>(node.pins.size()));
            const auto window = ImGui::GetWindowPos();
            if (const auto size = ImGui::GetWindowSize(); position.x + width < window.x || position.y + height < window.y ||
                                                          position.x > window.x + size.x || position.y > window.y + size.y)
                return;
            const auto id = std::to_string(node.id.Value());
            ImGui::PushID(id.c_str());
            ImGui::SetCursorScreenPos(position);
            const auto title = std::format("{} ({})###node", frame.NodeLabel(node), node.id.Value());
            if (const bool selected = std::ranges::find(frame.State().selection, node.id) != frame.State().selection.end();
                Ui::Button({.label = title.c_str(),
                            .size = {width / Theme::GetActiveTokens().sizes.uiScale, 0},
                            .variant = selected ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                            .font = frame.fonts.sans}))
                SelectNode(frame, node.id);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                auto &state = frame.Edit().presentation;
                auto target = std::ranges::find(state.placements, node.id, &PCGNodePlacement::node);
                if (!state.draggingNode) {
                    state.draggingNode = node.id;
                    state.dragOrigin = target->position;
                }
                target->position.x = std::clamp(target->position.x + ImGui::GetIO().MouseDelta.x / state.zoom, -1'000'000.0F, 1'000'000.0F);
                target->position.y = std::clamp(target->position.y + ImGui::GetIO().MouseDelta.y / state.zoom, -1'000'000.0F, 1'000'000.0F);
            }
            for (std::size_t index = 0; index < node.pins.size(); ++index) {
                ImGui::SetCursorScreenPos({position.x, position.y + row * static_cast<float>(index + 1)});
                DrawPin(frame, node, node.pins[index], width);
            }
            ImGui::PopID();
        }

        /** @brief Composes the real node canvas; theme-independent domain snapshots remain immutable. */
        void DrawCanvas(CanvasFrame &frame, ImVec2 size) {
            Ui::ScopedCard card("##pcg_canvas", size);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const auto &tokens = Theme::GetActiveTokens();
            const float row = DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium).minimumHeight + ImGui::GetStyle().ItemSpacing.y;
            const float width = std::max(240.0F * tokens.sizes.uiScale, row * 6.0F);
            auto *draw = ImGui::GetWindowDrawList();
            for (const auto &edge : frame.document.snapshot.source->Data().edges) {
                const auto from = PinAnchor(frame, edge.sourceNode, edge.sourcePin, origin, row, width);
                const auto to = PinAnchor(frame, edge.targetNode, edge.targetPin, origin, row, width);
                const float bend = std::max(row, std::abs(to.x - from.x) * 0.5F);
                draw->AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to, Theme::U32(Theme::Muted()), 2.0F);
            }
            for (const auto &node : frame.document.snapshot.source->Data().nodes)
                DrawNode(frame, node, origin, row, width);
            if (frame.State().connectingPin) {
                const auto from = PinAnchor(frame, *frame.State().connectingNode, *frame.State().connectingPin, origin, row, width);
                draw->AddLine(from, ImGui::GetIO().MousePos, Theme::U32(Theme::Text()), 2.0F);
            }
            const bool hovered = ImGui::IsWindowHovered();
            if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                auto &state = frame.Edit().presentation;
                state.pan.x = std::clamp(state.pan.x + ImGui::GetIO().MouseDelta.x, -1'000'000.0F, 1'000'000.0F);
                state.pan.y = std::clamp(state.pan.y + ImGui::GetIO().MouseDelta.y, -1'000'000.0F, 1'000'000.0F);
            }
            if (hovered && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0) {
                const auto before = frame.State();
                auto &state = frame.Edit().presentation;
                state.zoom = std::clamp(before.zoom + ImGui::GetIO().MouseWheel * 0.1F, 0.25F, 2.0F);
                const float ratio = state.zoom / before.zoom;
                state.pan = {ImGui::GetIO().MousePos.x - origin.x - (ImGui::GetIO().MousePos.x - origin.x - before.pan.x) * ratio,
                             ImGui::GetIO().MousePos.y - origin.y - (ImGui::GetIO().MousePos.y - origin.y - before.pan.y) * ratio};
            }
            if ((frame.State().draggingNode || frame.State().connectingPin) &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::GetIO().AppFocusLost))
                CancelGesture(frame);
            else if (frame.State().draggingNode && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                frame.Edit().presentation.draggingNode.reset();
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
                frame.Edit().presentation.selection.clear();
                CancelGesture(frame);
            }
        }

        /** @brief Forwards one explicit property Apply while retaining presentation-only buffers in the panel. */
        bool DrawAuthoringControls(CanvasFrame &frame) {
            if (frame.schemas && frame.inspector) {
                ImGui::BeginDisabled(frame.request && frame.request->action != PCGEditorAction::Present);
                const bool submitted = DrawPCGAuthoringInspector(*frame.inspector, frame.document.snapshot, *frame.schemas,
                                                                 frame.State().selection, true, frame.localization, frame.fonts)
                                           .has_value();
                if (submitted) {
                    frame.Edit().action = PCGEditorAction::ApplyProperties;
                    frame.Edit().authoring = frame.inspector->draft;
                }
                ImGui::EndDisabled();
                return submitted;
            }
            return false;
        }

        /** @brief Composes selection inspection and reference-aware removal without decoding node-owned payloads. */
        void DrawInspector(CanvasFrame &frame) {
            if (DrawAuthoringControls(frame))
                return;
            Ui::SectionTitle(frame.Text("workspace.pcg.inspector").c_str(), frame.fonts);
            if (frame.State().selection.empty()) {
                Ui::Hint(frame.Text("workspace.pcg.select_hint").c_str(), frame.fonts);
                return;
            }
            if (frame.Button("workspace.pcg.remove"))
                frame.Edit().action = PCGEditorAction::RemoveSelection;
            for (const auto id : frame.State().selection) {
                const auto &nodes = frame.document.snapshot.source->Data().nodes;
                const auto node = std::ranges::find(nodes, id, &PCG::PCGGraphNode::id);
                if (node == nodes.end())
                    continue;
                const auto key = std::to_string(id.Value());
                ImGui::PushID(key.c_str());
                Ui::LabeledSeparator(frame.NodeLabel(*node).c_str(), frame.fonts);
                Ui::DrawPropRow(frame.Text("workspace.pcg.identity").c_str(), key.c_str(), frame.fonts);
                Ui::DrawPropRow(frame.Text("workspace.pcg.version").c_str(),
                                std::format("{}.{}", node->version.major, node->version.minor).c_str(), frame.fonts);
                Ui::DrawPropRow(frame.Text("workspace.pcg.payload").c_str(), std::to_string(node->payload.size()).c_str(), frame.fonts);
                for (const auto &pin : node->pins)
                    DrawPin(frame, *node, pin, std::max(1.0F, ImGui::GetContentRegionAvail().x));
                ImGui::PopID();
            }
            Ui::LabeledSeparator(frame.Text("workspace.pcg.connections").c_str(), frame.fonts);
            for (const auto &edge : frame.document.snapshot.source->Data().edges) {
                if (const auto &selected = frame.State().selection; std::ranges::find(selected, edge.sourceNode) == selected.end() &&
                                                                    std::ranges::find(selected, edge.targetNode) == selected.end())
                    continue;
                const auto label = std::format("{} {}:{} → {}:{}###edge{}", frame.Text("workspace.pcg.disconnect"), edge.sourceNode.Value(),
                                               edge.sourcePin.Value(), edge.targetNode.Value(), edge.targetPin.Value(), edge.id.Value());
                if (Ui::Button({.label = label.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = frame.fonts.sans})) {
                    frame.Edit().action = PCGEditorAction::Disconnect;
                    frame.Edit().edge = edge.id;
                }
            }
        }

        /** @brief Registered document contribution; mounting controls presentation admission, never source lifetime. */
        class PCGGraphWorkspacePanel final : public IWorkspacePanel {
        public:
            /** @copydoc IWorkspacePanel::GetId */
            std::string GetId() const override {
                return std::string{PCGGraphPanelId};
            }

            /** @copydoc IWorkspacePanel::GetDisplayName */
            std::string GetDisplayName() const override {
                return "web_workspace.menu.pcg_graph_editor";
            }

            /** @copydoc IWorkspacePanel::GetDefaultDockArea */
            WorkspaceDockArea GetDefaultDockArea() const override {
                return WorkspaceDockArea::Document;
            }

            /** @copydoc IWorkspacePanel::GetObservedEventTypes */
            std::vector<std::string> GetObservedEventTypes() const override {
                return {};
            }

            /** @copydoc IWorkspacePanel::OnAttach */
            void OnAttach(PanelContext &) override {
                attached_ = true;
            }

            /** @copydoc IWorkspacePanel::OnDetach */
            void OnDetach() override {
                attached_ = false;
                inspector_ = {};
            }

            /** @copydoc IWorkspacePanel::DrawIcon */
            void DrawIcon(ImDrawList *, const ImVec2 &, const ImVec2 &, ImU32) override {
                // This asset-document presenter has no activity-bar placement; its document tab supplies navigation.
            }

            /** @copydoc IWorkspacePanel::DrawPanel */
            void DrawPanel(const ImVec2 &, const ImVec2 &, const EditorWorkspaceViewModel &workspace,
                           EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override {
                if (!attached_)
                    return;
                const auto active = workspace.workspacePanelHost.ActiveDocument();
                const auto found = std::ranges::find_if(workspace.pcgDocuments, [active](const auto &entry) {
                    return active == entry.snapshot.identity.instance;
                });
                if (found == workspace.pcgDocuments.end())
                    return;
                if (auto request = DrawPCGGraphEditor(*found, workspace.pcgPalette, context.localization, context.theme.fonts,
                                                      &workspace.pcgSchemas, &inspector_)) {
                    command.command = EditorWorkspaceViewCommand::EditPCGGraph;
                    command.documentInstance = found->snapshot.identity.instance;
                    command.pcgRequest = std::move(request);
                }
            }

        private:
            bool attached_{};
            PCGAuthoringInspectorState inspector_;
        };
    }  // namespace

    /** @copydoc DrawPCGGraphEditor */
    std::optional<PCGEditorRequest> DrawPCGGraphEditor(const PCGWorkspaceDocument &document, std::span<const PCGPaletteEntry> palette,
                                                       const ILocalizationService &localization, const Theme::Fonts &fonts,
                                                       const PCG::PCGNodeCatalogSnapshot *schemas, PCGAuthoringInspectorState *inspector) {
        CanvasFrame frame{document, palette, localization, fonts, {}, schemas, inspector};
        ImGui::PushID(std::to_string(document.snapshot.identity.instance.Value()).c_str());
        if ((frame.State().draggingNode || frame.State().connectingPin) &&
            (ImGui::GetIO().AppFocusLost || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)))
            CancelGesture(frame);
        DrawTools(frame);
        if (document.error) {
            Ui::ErrorText(frame.Text("workspace.pcg.rejected").c_str(), frame.fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, document.error->code.Value(), Theme::Muted(), frame.fonts);
        }
        if (frame.State().connectingPin && frame.Button("workspace.pcg.cancel_connection"))
            CancelGesture(frame);
        const auto available = ImGui::GetContentRegionAvail();
        const float gap = Theme::GetActiveTokens().spacing.propertyRowGap;
        const bool wide = available.x >= 900.0F * Theme::GetActiveTokens().sizes.uiScale;
        const float side = wide ? available.x * 0.24F : available.x;
        const float height = wide ? std::max(1.0F, available.y) : std::max(120.0F, available.y * 0.3F);
        {
            Ui::ScopedCard paletteCard("##pcg_palette", {side, height});
            DrawPalette(frame);
        }
        if (wide)
            ImGui::SameLine(0, gap);
        DrawCanvas(frame, {wide ? std::max(1.0F, available.x - side * 2 - gap * 2) : available.x,
                           wide ? height : std::max(160.0F, available.y * 0.4F)});
        if (wide)
            ImGui::SameLine(0, gap);
        {
            Ui::ScopedCard inspectorCard("##pcg_inspector", {side, height});
            DrawInspector(frame);
        }
        ImGui::PopID();
        return std::move(frame.request);
    }

    /** @copydoc CreatePCGGraphWorkspacePanel */
    std::shared_ptr<IWorkspacePanel> CreatePCGGraphWorkspacePanel() {
        return std::make_shared<PCGGraphWorkspacePanel>();
    }

    /** @copydoc EditorWorkspaceView::DrawPCGDocument */
    bool EditorWorkspaceView::DrawPCGDocument(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &outCommand) const {
        const auto active = viewModel.workspacePanelHost.ActiveDocument();
        if (const auto found = std::ranges::find_if(viewModel.pcgDocuments,
                                                    [active](const auto &entry) {
            return active == entry.snapshot.identity.instance;
        });
            found == viewModel.pcgDocuments.end())
            return false;
        const auto &panels = m_panelRegistry.GetPanelsForArea(WorkspaceDockArea::Document);
        if (const auto presenter = std::ranges::find_if(panels,
                                                        [](const auto &panel) {
            return panel->GetId() == PCGGraphPanelId;
        });
            presenter != panels.end())
            (*presenter)->DrawPanel(ImGui::GetWindowPos(), ImGui::GetWindowSize(), viewModel, outCommand, m_context);
        else
            Ui::ErrorText(m_context.localization.Get("editor", "workspace.source_open.unavailable").c_str(), m_context.theme.fonts);
        return true;
    }
}  // namespace Horo::Editor
