#include "DecisionGraphCanvas.h"

#include "DecisionGraphCanvasModel.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <imgui_node_editor.h>
#include <map>

namespace Horo::Editor {
    namespace {
        namespace Widget = ax::NodeEditor;

        struct CanvasContextDeleter final {
            void operator()(Widget::EditorContext *context) const {
                Widget::DestroyEditor(context);
            }
        };

        /** @brief Restores the previous widget context on every presentation exit. */
        class ScopedCanvasContext final {
        public:
            explicit ScopedCanvasContext(Widget::EditorContext *context) : previous_(Widget::GetCurrentEditor()) {
                Widget::SetCurrentEditor(context);
            }

            ~ScopedCanvasContext() {
                Widget::SetCurrentEditor(previous_);
            }

        private:
            Widget::EditorContext *previous_;
        };

        /** @brief Allocates a presentation-only context without a sidecar or ambient settings file. */
        auto CreateCanvasContext() {
            Widget::Config config;
            config.SettingsFile = nullptr;
            return std::unique_ptr<Widget::EditorContext, CanvasContextDeleter>{Widget::CreateEditor(&config)};
        }

        /** @brief Vendor identities are compact ordinals within one exact snapshot revision. */
        template <class Id> Id WidgetId(std::size_t index) {
            return Id{static_cast<std::uintptr_t>(index + 1)};
        }

        /** @brief Rejects empty or foreign widget IDs before resolving the current typed snapshot. */
        template <class Id> std::optional<std::size_t> SnapshotIndex(Id id, std::size_t count) {
            return id.Get() > 0 && id.Get() <= count ? std::optional<std::size_t>{id.Get() - 1} : std::nullopt;
        }

        constexpr std::size_t PinsPerCanvasRow{9};

        /** @brief Assigns bounded deterministic positions with cycles handled by visited breadth layers. */
        std::vector<ImVec2> InitialCanvasPositions(const GraphViewSnapshot &graph, float scale) {
            const auto noDepth = graph.nodes.size();
            std::map<AI::DecisionNodeId, std::size_t> indices;
            std::vector<std::vector<std::size_t>> connections(graph.nodes.size());
            std::vector<std::size_t> depth(graph.nodes.size(), noDepth);
            std::vector<std::size_t> pending;
            pending.reserve(graph.nodes.size());
            for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
                const auto &node = graph.nodes[index];
                indices.emplace(node.id, index);
                if (node.initial) {
                    depth[index] = 0;
                    pending.push_back(index);
                }
            }
            for (const auto &link : graph.links)
                connections[indices.at(graph.pins[link.from].node)].push_back(indices.at(graph.pins[link.to].node));
            for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
                const auto source = pending[cursor];
                for (const auto target : connections[source]) {
                    if (depth[target] == noDepth) {
                        depth[target] = depth[source] + 1;
                        pending.push_back(target);
                    }
                }
            }
            const auto lastReachable = pending.empty() ? 0 : depth[pending.back()];
            std::vector<float> row(lastReachable + 2, 32.0F);
            std::vector<ImVec2> positions;
            positions.reserve(graph.nodes.size());
            for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
                const auto level = depth[index];
                const auto column = level == noDepth ? lastReachable + 1 : level;
                const auto pinRows = (graph.nodes[index].pinCount + PinsPerCanvasRow - 1) / PinsPerCanvasRow;
                positions.push_back({scale * (32.0F + static_cast<float>(column) * 236.0F), scale * row[column]});
                row[column] += 116.0F + 24.0F * static_cast<float>(pinRows);
            }
            return positions;
        }

        /** @brief Uses localized roles for blank labels and typed unknown/terminal presentation. */
        const std::string &RoleLabel(GraphNodeRole role, const ILocalizationService &localization) {
            constexpr std::array Keys{"workspace.decision_canvas.root",        "workspace.decision_canvas.sequence",
                                      "workspace.decision_canvas.selector",    "workspace.decision_canvas.composite",
                                      "workspace.decision_canvas.decorator",   "workspace.decision_canvas.service",
                                      "workspace.decision_canvas.task",        "workspace.decision_canvas.unknown",
                                      "workspace.decision_graph.edit_running", "workspace.decision_graph.succeeded",
                                      "workspace.decision_graph.failed",       "workspace.decision_graph.edit_utility_option"};
            return localization.Get("editor", Keys[static_cast<std::size_t>(role)]);
        }

        /** @brief Registers one native endpoint and draws it with existing editor theme tokens. */
        void DrawPin(const GraphPinView &pin, std::size_t slot, float scale) {
            Widget::BeginPin(WidgetId<Widget::PinId>(slot), pin.output ? Widget::PinKind::Output : Widget::PinKind::Input);
            const auto pos = ImGui::GetCursorScreenPos();
            const float box = 12.0F * scale;
            ImGui::Dummy({box, box});
            ImGui::GetWindowDrawList()->AddCircleFilled({pos.x + box * 0.5F, pos.y + box * 0.5F}, box * 0.28F,
                                                        Theme::U32(pin.output ? Theme::Accent() : Theme::Muted()));
            Widget::EndPin();
        }

        /** @brief Projects one static node and its authored pin range. */
        void DrawNode(const GraphViewSnapshot &graph, std::size_t index, const EditorGuiContext &context, float scale) {
            const auto &node = graph.nodes[index];
            const Widget::NodeId identity = WidgetId<Widget::NodeId>(index);
            Widget::BeginNode(identity);
            ImGui::PushID(static_cast<int>(index));
            {
                const auto &fonts = context.theme.fonts;
                Theme::ScopedTextStyle style(fonts.sans, Theme::TextPx::Label(), Theme::FontPx::Sans);
                const auto &role = RoleLabel(node.role, context.localization);
                const auto &title = node.label.empty() ? role : node.label;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 172.0F * scale);
                ImGui::TextUnformatted(title.c_str());
                if (node.missing && !node.label.empty())
                    ImGui::TextColored(Theme::Warn(), "%s", role.c_str());
                ImGui::PopTextWrapPos();
                if (node.incomingOrder)
                    ImGui::TextColored(Theme::Muted(), "%s %zu",
                                       context.localization.Get("editor", "workspace.decision_graph.edit_tree_order").c_str(),
                                       *node.incomingOrder + 1);
            }

            for (std::size_t offset = 0; offset < node.pinCount; ++offset) {
                if (offset % PinsPerCanvasRow != 0)
                    ImGui::SameLine();
                const auto slot = node.firstPin + offset;
                DrawPin(graph.pins[slot], slot, scale);
            }
            ImGui::PopID();
            Widget::EndNode();
        }

        /** @brief Reads exact selection using only current native indices and immutable authored identities. */
        void ReadSelection(const GraphViewSnapshot &graph, DecisionCanvasInteraction &out) {
            if (!Widget::HasSelectionChanged() || Widget::GetSelectedObjectCount() != 1)
                return;
            Widget::NodeId node;
            if (Widget::GetSelectedNodes(&node, 1) == 1) {
                if (const auto index = SnapshotIndex(node, graph.nodes.size()))
                    out.node = graph.nodes[*index].id;
                return;
            }
            Widget::LinkId link;
            if (Widget::GetSelectedLinks(&link, 1) != 1)
                return;
            const auto index = SnapshotIndex(link, graph.links.size());
            if (!index)
                return;
            const auto &selected = graph.links[*index];
            if (selected.role == GraphLinkRole::Transition) {
                const auto transition = AI::DecisionNodeId::Create(selected.authoredId);
                if (transition.HasValue())
                    out.transition = transition.Value();
            } else {
                out.node = graph.pins[selected.to].node;
            }
        }

        /** @brief Offers one complete link gesture to the common typed document owner. */
        void ReadTopologyIntent(const GraphViewSnapshot &graph, DecisionCanvasInteraction &out) {
            if (!graph.editable || graph.family != AI::DecisionPlanKind::StateMachine)
                return;
            const DecisionGraphDocumentFence fence{graph.instance, graph.revision};
            if (Widget::BeginCreate()) {
                Widget::PinId first;
                Widget::PinId second;
                if (Widget::QueryNewLink(&first, &second)) {
                    const auto from = SnapshotIndex(first, graph.pins.size());
                    const auto to = SnapshotIndex(second, graph.pins.size());
                    const auto request =
                        from && to ? ResolveDecisionCanvasEdit(graph, {fence, GraphConnectIntent{graph.pins[*from], graph.pins[*to]}})
                                   : std::nullopt;
                    if (!request)
                        Widget::RejectNewItem();
                    else if (Widget::AcceptNewItem())
                        out.edit = request;
                }
            }
            Widget::EndCreate();
            if (Widget::BeginDelete()) {
                Widget::NodeId node;
                while (Widget::QueryDeletedNode(&node))
                    Widget::RejectDeletedItem();
                Widget::LinkId link;
                while (Widget::QueryDeletedLink(&link)) {
                    const auto index = SnapshotIndex(link, graph.links.size());
                    const auto request =
                        index ? ResolveDecisionCanvasEdit(graph, {fence, GraphRemoveLinkIntent{graph.links[*index].role,
                                                                                               graph.links[*index].authoredId}})
                              : std::nullopt;
                    if (out.edit || !request)
                        Widget::RejectDeletedItem();
                    else if (Widget::AcceptDeletedItem(false))
                        out.edit = request;
                }
            }
            Widget::EndDelete();
        }
    }  // namespace

    struct DecisionGraphCanvas::Impl final {
        struct Session final {
            GraphViewSnapshot snapshot;
            std::unique_ptr<Widget::EditorContext, CanvasContextDeleter> context;
        };

        std::map<DocumentInstanceId, Session> sessions;
    };

    DecisionGraphCanvas::DecisionGraphCanvas() : state_(std::make_unique<Impl>()) {}

    DecisionGraphCanvas::~DecisionGraphCanvas() = default;

    /** @copydoc DecisionGraphCanvas::Retain */
    void DecisionGraphCanvas::Retain(std::span<const DecisionGraphWorkspaceDocument> documents) {
        std::erase_if(state_->sessions, [&](const auto &entry) {
            return std::ranges::none_of(documents, [&](const auto &document) {
                return document.identity.instance == entry.first;
            });
        });
    }

    /** @copydoc DecisionGraphCanvas::Draw */
    DecisionCanvasInteraction DecisionGraphCanvas::Draw(const DecisionGraphWorkspaceDocument &document, const EditorGuiContext &context) {
        DecisionCanvasInteraction out;
        auto &session = state_->sessions[document.identity.instance];
        const bool newSnapshot = !session.context || session.snapshot.revision != document.revision;
        if (newSnapshot) {
            session.snapshot = ProjectDecisionCanvas(*document.graph, {document.identity.instance, document.revision}, document.editable);
            session.context = CreateCanvasContext();
        }
        if (!session.context)
            return out;

        const auto &graph = session.snapshot;
        ScopedCanvasContext current(session.context.get());
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const auto available = ImGui::GetContentRegionAvail();
        const auto &fonts = context.theme.fonts;
        const auto &frameLabel = context.localization.Get("editor", "workspace.decision_canvas.fit");
        const bool fit = Ui::Button({.label = frameLabel.c_str(), .font = fonts.sans});
        Widget::PushStyleColor(Widget::StyleColor_Bg, Theme::Bg0());
        Widget::PushStyleColor(Widget::StyleColor_Grid, Theme::Border());
        Widget::PushStyleColor(Widget::StyleColor_NodeBg, Theme::Bg1());
        Widget::PushStyleColor(Widget::StyleColor_NodeBorder, Theme::Border());
        Widget::PushStyleColor(Widget::StyleColor_SelNodeBorder, Theme::Accent());
        Widget::Begin("##DecisionGraphCanvas",
                      {std::max(1.0F, available.x), std::clamp(available.y * 0.42F, 160.0F * scale, 420.0F * scale)});
        if (newSnapshot) {
            const auto positions = InitialCanvasPositions(graph, scale);
            for (std::size_t i = 0; i < positions.size(); ++i)
                Widget::SetNodePosition(WidgetId<Widget::NodeId>(i), positions[i]);
        }
        for (std::size_t index = 0; index < graph.nodes.size(); ++index)
            DrawNode(graph, index, context, scale);
        for (std::size_t index = 0; index < graph.links.size(); ++index) {
            const auto &link = graph.links[index];
            const bool secondary = link.role == GraphLinkRole::Hierarchy || link.role == GraphLinkRole::Service;
            Widget::Link(WidgetId<Widget::LinkId>(index), WidgetId<Widget::PinId>(link.from), WidgetId<Widget::PinId>(link.to),
                         secondary ? Theme::Muted() : Theme::Accent(), scale * (secondary ? 1.0F : 2.0F));
        }
        ReadSelection(graph, out);
        ReadTopologyIntent(graph, out);
        if (fit || newSnapshot)
            Widget::NavigateToContent(0.0F);
        Widget::End();
        Widget::PopStyleColor(5);
        return out;
    }
}  // namespace Horo::Editor
