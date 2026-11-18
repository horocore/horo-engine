#include "DecisionGraphCanvas.h"

#include "DecisionGraphCanvasModel.h"
#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <array>
#include <cmath>
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

        /** @brief Resolves canonical logical positions in the same widget order as the projection. */
        DecisionGraphLayout CanvasPositions(const GraphViewSnapshot &graph, const DecisionGraphLayout *saved) {
            const auto automatic = AutomaticDecisionLayout(graph);
            DecisionGraphLayout result;
            result.reserve(graph.nodes.size());
            for (const auto &node : graph.nodes) {
                if (saved) {
                    const auto override = std::ranges::lower_bound(*saved, node.id, {}, &DecisionNodePlacement::node);
                    if (override != saved->end() && override->node == node.id) {
                        result.push_back(*override);
                        continue;
                    }
                }
                const auto fallback = std::ranges::lower_bound(automatic, node.id, {}, &DecisionNodePlacement::node);
                result.push_back(*fallback);
            }
            return result;
        }

        /** @brief Emits a whole drag as one command and restores the last accepted widget positions until the owner commits. */
        DecisionGraphLayout ReadCompletedDrag(const DecisionGraphLayout &baseline, float scale) {
            DecisionGraphLayout moved;
            if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::GetIO().AppFocusLost)
                return moved;
            constexpr float LogicalEpsilon = 0.5F;
            for (std::size_t index = 0; index < baseline.size(); ++index) {
                const auto &accepted = baseline[index];
                const auto widgetId = WidgetId<Widget::NodeId>(index);
                const auto current = Widget::GetNodePosition(widgetId);
                const auto x = current.x / scale;
                const auto y = current.y / scale;
                if (std::abs(x - accepted.x) <= LogicalEpsilon && std::abs(y - accepted.y) <= LogicalEpsilon)
                    continue;
                moved.push_back({accepted.node, x, y});
                Widget::SetNodePosition(widgetId, {accepted.x * scale, accepted.y * scale});
            }
            return moved;
        }

        /** @brief Uses localized roles for blank labels and typed unknown/terminal presentation. */
        const std::string &RoleLabel(GraphNodeRole role, const ILocalizationService &localization) {
            constexpr std::array Keys{"workspace.decision_canvas.root",        "workspace.decision_canvas.sequence",
                                      "workspace.decision_canvas.selector",    "workspace.decision_canvas.composite",
                                      "workspace.decision_canvas.decorator",   "workspace.decision_canvas.service",
                                      "workspace.decision_canvas.task",        "workspace.decision_canvas.unknown",
                                      "workspace.decision_graph.edit_running", "workspace.decision_graph.succeeded",
                                      "workspace.decision_graph.failed",       "workspace.decision_graph.edit_utility_option",
                                      "workspace.decision_canvas.subtree"};
            return localization.Get("editor", Keys[static_cast<std::size_t>(role)]);
        }

        /** @brief Maps detached binding evidence to a localized, node-local warning. */
        const char *ProviderIssueLabel(AI::DecisionProviderBindingIssueKind kind) {
            using enum AI::DecisionProviderBindingIssueKind;
            switch (kind) {
                case CatalogUnavailable:
                    return "workspace.decision_canvas.provider_catalog_unavailable";
                case DescriptorMissing:
                    return "workspace.decision_canvas.provider_missing";
                case DescriptorVersionMismatch:
                    return "workspace.decision_canvas.provider_version_mismatch";
            }
            return "workspace.decision_canvas.provider_catalog_unavailable";
        }

        /** @brief Registers one native endpoint and draws it with existing editor theme tokens. */
        void DrawPin(const GraphPinView &pin, std::size_t slot, float scale) {
            Widget::BeginPin(WidgetId<Widget::PinId>(slot), pin.output ? Widget::PinKind::Output : Widget::PinKind::Input);
            const auto pos = ImGui::GetCursorScreenPos();
            const float box = 12.0F * scale;
            ImGui::Dummy({box, box});
            if (ImGui::IsItemVisible()) {
                ImGui::GetWindowDrawList()->AddCircleFilled({pos.x + box * 0.5F, pos.y + box * 0.5F}, box * 0.28F,
                                                            Theme::U32(pin.output ? Theme::Accent() : Theme::Muted()));
            }
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
                if (!node.missing || !node.label.empty())
                    ImGui::TextUnformatted(title.c_str());
                if (node.missing)
                    ImGui::TextColored(Theme::Warn(), "%s", role.c_str());
                if (node.providerIssue)
                    ImGui::TextColored(Theme::Warn(), "%s",
                                       context.localization.Get("editor", ProviderIssueLabel(*node.providerIssue)).c_str());
                if (node.incomingOrder) {
                    const auto label = node.role == GraphNodeRole::UtilityOption ? "workspace.decision_canvas.utility_order"
                                                                                 : "workspace.decision_graph.edit_tree_order";
                    ImGui::TextColored(Theme::Muted(), "%s %zu", context.localization.Get("editor", label).c_str(),
                                       *node.incomingOrder + 1);
                }
                ImGui::PopTextWrapPos();
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

        /** @brief Confirms the selected node's center is actually inside the displayed native canvas. */
        bool WidgetNodeInView(std::size_t index, ImVec2 origin, ImVec2 viewport) {
            const auto widget = WidgetId<Widget::NodeId>(index);
            const auto position = Widget::GetNodePosition(widget);
            const auto size = Widget::GetNodeSize(widget);
            if (size.x <= 0.0F || size.y <= 0.0F)
                return false;
            const auto center = Widget::CanvasToScreen({position.x + size.x * 0.5F, position.y + size.y * 0.5F});
            const auto right = Widget::CanvasToScreen({position.x + size.x, position.y + size.y * 0.5F});
            return right.x - center.x >= ImGui::GetFontSize() * 0.5F && center.x >= origin.x && center.x <= origin.x + viewport.x &&
                   center.y >= origin.y && center.y <= origin.y + viewport.y;
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
        struct Focus final {
            DecisionGraphDocumentFence fence;
            AI::DecisionNodeId node;
        };

        struct Session final {
            GraphViewSnapshot snapshot;
            std::shared_ptr<const AI::DecisionGraphSource> source;
            std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> providers;
            DecisionGraphLayout positions;
            float scale{};
            std::unique_ptr<Widget::EditorContext, CanvasContextDeleter> context;
            std::optional<std::size_t> revealing;
        };

        std::optional<Focus> pendingFocus;

        std::map<DocumentInstanceId, Session> sessions;
    };

    DecisionGraphCanvas::DecisionGraphCanvas() : state_(std::make_unique<Impl>()) {}

    DecisionGraphCanvas::~DecisionGraphCanvas() = default;

    /** @copydoc DecisionGraphCanvas::RequestFocus */
    void DecisionGraphCanvas::RequestFocus(DecisionGraphDocumentFence fence, AI::DecisionNodeId node) {
        state_->pendingFocus = Impl::Focus{fence, node};
    }

    /** @copydoc DecisionGraphCanvas::Retain */
    void DecisionGraphCanvas::Retain(std::span<const DecisionGraphWorkspaceDocument> documents) {
        std::erase_if(state_->sessions, [&](const auto &entry) {
            return std::ranges::none_of(documents, [&](const auto &document) {
                return document.identity.instance == entry.first;
            });
        });
    }

    /** @copydoc DecisionGraphCanvas::Draw */
    DecisionCanvasInteraction DecisionGraphCanvas::Draw(const DecisionGraphWorkspaceDocument &document, const EditorGuiContext &context,
                                                        const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &providers) {
        DecisionCanvasInteraction out;
        auto &session = state_->sessions[document.identity.instance];
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const bool rebuildCanvas = !session.context || session.source != document.graph || session.scale != scale;
        const bool replacePositions = rebuildCanvas || session.snapshot.revision != document.revision;
        if (rebuildCanvas) {
            session.snapshot = ProjectDecisionCanvas(*document.graph, {document.identity.instance, document.revision}, document.editable);
            session.source = document.graph;
            session.scale = scale;
            session.context = CreateCanvasContext();
            session.revealing.reset();
        }
        if (rebuildCanvas || session.providers != providers) {
            ApplyDecisionCanvasProviderIssues(session.snapshot, *document.graph, providers.get());
            session.providers = providers;
        }
        if (replacePositions) {
            session.snapshot.revision = document.revision;
            session.positions = CanvasPositions(session.snapshot, document.layout.get());
            session.revealing.reset();
        }
        if (!session.context)
            return out;

        const auto &graph = session.snapshot;
        ScopedCanvasContext current(session.context.get());
        const auto available = ImGui::GetContentRegionAvail();
        const auto &fonts = context.theme.fonts;
        const auto &frameLabel = context.localization.Get("editor", "workspace.decision_canvas.fit");
        const bool fit = Ui::Button({.label = frameLabel.c_str(), .font = fonts.sans});
        if (document.editable) {
            const auto &automatic = context.localization.Get("editor", "workspace.decision_canvas.auto_layout");
            if (Ui::Button({.label = automatic.c_str(), .enabled = document.layoutWritable, .font = fonts.sans}))
                out.layout =
                    DecisionGraphLayoutRequest{{document.identity.instance, document.revision}, DecisionLayoutOperation::AutoLayout};
            if (!document.layoutWritable)
                Ui::WrappedText(available.x, context.localization.Get("editor", "workspace.decision_canvas.layout_unavailable"),
                                Theme::Muted(), fonts);
        }
        Widget::PushStyleColor(Widget::StyleColor_Bg, Theme::Bg0());
        Widget::PushStyleColor(Widget::StyleColor_Grid, Theme::Border());
        Widget::PushStyleColor(Widget::StyleColor_NodeBg, Theme::Bg1());
        Widget::PushStyleColor(Widget::StyleColor_NodeBorder, Theme::Border());
        Widget::PushStyleColor(Widget::StyleColor_SelNodeBorder, Theme::Accent());
        const auto origin = ImGui::GetCursorScreenPos();
        const ImVec2 viewport{std::max(1.0F, available.x), std::clamp(available.y * 0.42F, 160.0F * scale, 420.0F * scale)};
        Widget::Begin("##DecisionGraphCanvas", viewport);
        if (replacePositions) {
            for (std::size_t i = 0; i < session.positions.size(); ++i) {
                const auto &placement = session.positions[i];
                Widget::SetNodePosition(WidgetId<Widget::NodeId>(i), {placement.x * scale, placement.y * scale});
            }
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
        if (fit || rebuildCanvas) {
            session.revealing.reset();
            Widget::NavigateToContent(0.0F);
        }
        if (state_->pendingFocus) {
            const auto focus = *state_->pendingFocus;
            state_->pendingFocus.reset();
            session.revealing.reset();
            if (focus.fence.instance == graph.instance && focus.fence.revision == graph.revision) {
                const auto node = std::ranges::find(graph.nodes, focus.node, &GraphNodeView::id);
                if (node != graph.nodes.end()) {
                    const auto index = static_cast<std::size_t>(node - graph.nodes.begin());
                    Widget::SelectNode(WidgetId<Widget::NodeId>(index), false);
                    Widget::NavigateToSelection(true, 0.0F);
                    session.revealing = index;
                    out.node = node->id;
                }
            }
        }
        Widget::End();
        if (session.revealing && WidgetNodeInView(*session.revealing, origin, viewport)) {
            out.revealed = graph.nodes[*session.revealing].id;
            session.revealing.reset();
        }
        auto moved = ReadCompletedDrag(session.positions, scale);
        if (!out.edit && !out.layout && document.layoutWritable && !moved.empty() && !ImGui::GetIO().AppFocusLost &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            out.layout = DecisionGraphLayoutRequest{{document.identity.instance, document.revision},
                                                    DecisionLayoutOperation::Move,
                                                    std::move(moved)};
        Widget::PopStyleColor(5);
        return out;
    }
}  // namespace Horo::Editor
