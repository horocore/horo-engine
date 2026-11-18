#include "DecisionGraphCanvasModel.h"

#include "Horo/AI/DecisionGraphCompiler.h"

#include <algorithm>
#include <map>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Converts an already validated tree identity into the shared authored node namespace. */
        AI::DecisionNodeId TreeNode(AI::BehaviorTreeNodeId id) {
            return AI::DecisionNodeId::Create(id.Value()).Value();
        }

        /** @brief Translates canonical tree node kinds into localized presentation roles. */
        GraphNodeRole TreeRole(const AI::BehaviorTreeNode &node) {
            using enum AI::BehaviorTreeNodeKind;
            switch (node.kind) {
                case Root:
                    return GraphNodeRole::Root;
                case Composite:
                    if (node.control.kind == AI::DecisionControlKind::Sequence)
                        return GraphNodeRole::Sequence;
                    if (node.control.kind == AI::DecisionControlKind::Selector)
                        return GraphNodeRole::Selector;
                    return GraphNodeRole::Composite;
                case Decorator:
                    return GraphNodeRole::Decorator;
                case Service:
                    return GraphNodeRole::Service;
                case Task:
                    return node.control.kind == AI::DecisionControlKind::Subplan ? GraphNodeRole::Subtree : GraphNodeRole::Task;
                default:
                    return GraphNodeRole::Unknown;
            }
        }

        /** @brief Preserves real tree connections, stable pin IDs, and child execution order. */
        void ProjectTree(const AI::BehaviorTreeAsset &tree, GraphViewSnapshot &out) {
            std::map<AI::BehaviorTreePinId, std::size_t> pins;
            std::map<AI::BehaviorTreeNodeId, std::size_t> nodes;
            out.nodes.reserve(tree.Nodes().size());
            for (const auto &node : tree.Nodes()) {
                const auto id = TreeNode(node.id);
                nodes.emplace(node.id, out.nodes.size());
                out.nodes.push_back({.id = id,
                                     .role = TreeRole(node),
                                     .label = node.displayName,
                                     .firstPin = out.pins.size(),
                                     .pinCount = node.pins.size(),
                                     .initial = node.id == tree.Root(),
                                     .missing = node.kind == AI::BehaviorTreeNodeKind::Unknown});
                for (const auto &pin : node.pins) {
                    pins.emplace(pin.id, out.pins.size());
                    out.pins.push_back({id, pin.id.Value(), pin.direction == AI::BehaviorTreePinDirection::Output});
                }
            }
            out.links.reserve(tree.Edges().size());
            for (const auto &edge : tree.Edges()) {
                const bool child = edge.kind == AI::BehaviorTreeEdgeKind::Child;
                out.links.push_back({child ? GraphLinkRole::Child : GraphLinkRole::Service, edge.id.Value(), pins.at(edge.sourcePin),
                                     pins.at(edge.targetPin)});
                if (child)
                    out.nodes[nodes.at(edge.targetNode)].incomingOrder = edge.order;
            }
        }

        /** @brief Assembles synthetic visual endpoints for a family with endpoint identities on the nodes. */
        std::size_t AppendEndpointNode(GraphViewSnapshot &out, AI::DecisionNodeId id, GraphNodeRole role, bool initial) {
            const auto start = out.pins.size();
            out.nodes.push_back({.id = id, .role = role, .firstPin = start, .pinCount = 2, .initial = initial});
            out.pins.push_back({id, 0, false});
            out.pins.push_back({id, 0, true});
            return start;
        }

        /** @brief Projects authored transition identities separately from structural parent relationships. */
        void ProjectStates(const AI::StateMachineAssetDescriptor &machine, GraphViewSnapshot &out) {
            std::map<AI::DecisionNodeId, std::size_t> pins;
            for (const auto &state : machine.states) {
                auto role = GraphNodeRole::RunningState;
                if (state.terminal == AI::DecisionStateTerminal::Succeeded)
                    role = GraphNodeRole::SucceededState;
                if (state.terminal == AI::DecisionStateTerminal::Failed)
                    role = GraphNodeRole::FailedState;
                pins.emplace(state.id, AppendEndpointNode(out, state.id, role, state.id == machine.initial));
            }
            const auto addLink = [&](GraphLinkRole role, std::uint64_t authored, AI::DecisionNodeId source, AI::DecisionNodeId target) {
                const auto from = pins.find(source);
                const auto to = pins.find(target);
                if (from != pins.end() && to != pins.end())
                    out.links.push_back({role, authored, from->second + 1, to->second});
            };
            for (const auto &state : machine.states)
                if (state.parent.IsValid())
                    addLink(GraphLinkRole::Hierarchy, state.id.Value(), state.parent, state.id);
            for (const auto &edge : machine.transitions)
                addLink(GraphLinkRole::Transition, edge.id.Value(), edge.source, edge.target);
        }

        /** @brief Shows scorer-to-option associations using the stable authored option identity. */
        void ProjectUtility(const AI::UtilityDecisionSource &utility, GraphViewSnapshot &out) {
            const auto root = AppendEndpointNode(out, utility.root, GraphNodeRole::Root, true);
            for (const auto &option : utility.options) {
                const auto id = option.option.id;
                const auto target = AppendEndpointNode(out, id, GraphNodeRole::UtilityOption, false);
                out.links.push_back({GraphLinkRole::UtilityOption, id.Value(), root + 1, target});
            }
        }
    }  // namespace

    /** @copydoc ProjectDecisionCanvas */
    GraphViewSnapshot ProjectDecisionCanvas(const AI::DecisionGraphSource &graph, DecisionGraphDocumentFence fence, bool editable) {
        GraphViewSnapshot projection{.instance = fence.instance, .revision = fence.revision, .editable = editable};
        std::visit([&](const auto &source) {
            using Source = std::remove_cvref_t<decltype(source)>;
            if constexpr (std::is_same_v<Source, std::shared_ptr<const AI::BehaviorTreeAsset>>) {
                projection.family = AI::DecisionPlanKind::BehaviorTree;
                ProjectTree(*source, projection);
            } else if constexpr (std::is_same_v<Source, AI::StateMachineAssetDescriptor>) {
                projection.family = AI::DecisionPlanKind::StateMachine;
                ProjectStates(source, projection);
            } else {
                projection.family = AI::DecisionPlanKind::Utility;
                ProjectUtility(source, projection);
            }
        }, graph.body);
        return projection;
    }

    /** @copydoc ResolveDecisionCanvasEdit */
    std::optional<StateMachineEditRequest> ResolveDecisionCanvasEdit(const GraphViewSnapshot &snapshot, const GraphEditCommand &intent) {
        if (!snapshot.editable || snapshot.family != AI::DecisionPlanKind::StateMachine || snapshot.instance != intent.fence.instance ||
            snapshot.revision != intent.fence.revision)
            return {};
        StateMachineEditRequest request{.instance = snapshot.instance, .revision = snapshot.revision};
        if (const auto *link = std::get_if<GraphRemoveLinkIntent>(&intent.operation)) {
            if (link->role != GraphLinkRole::Transition || std::ranges::none_of(snapshot.links, [&](const auto &edge) {
                return edge.role == link->role && edge.authoredId == link->id;
            }))
                return {};
            const auto id = AI::DecisionNodeId::Create(link->id);
            if (id.HasError())
                return {};
            request.operation = StateMachineEditOperation::RemoveTransition;
            request.node = id.Value();
            return request;
        }
        const auto &[first, second] = std::get<GraphConnectIntent>(intent.operation);
        if (first.output == second.output || std::ranges::find(snapshot.pins, first) == snapshot.pins.end() ||
            std::ranges::find(snapshot.pins, second) == snapshot.pins.end())
            return {};
        request.operation = StateMachineEditOperation::Connect;
        request.node = first.output ? first.node : second.node;
        request.target = first.output ? second.node : first.node;
        const auto source = std::ranges::find(snapshot.nodes, request.node, &GraphNodeView::id);
        return source != snapshot.nodes.end() && source->role == GraphNodeRole::RunningState ? std::optional{request} : std::nullopt;
    }
}  // namespace Horo::Editor
