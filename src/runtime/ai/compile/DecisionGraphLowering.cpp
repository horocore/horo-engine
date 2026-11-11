#include "DecisionCompileDetail.h"
#include "DecisionSourceReachability.h"

#include <algorithm>

namespace Horo::AI::Detail {
    namespace {
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &code = AIErrors::DecisionAssetSchemaInvalid) {
            return Result<T>::Failure(MakeError(code));
        }

        bool ProviderControl(DecisionControlKind kind) noexcept {
            using enum Horo::AI::DecisionControlKind;
            return kind == Task || kind == Condition || kind == Service;
        }

        Result<BehaviorTreeOperation> DecoratorOperation(DecisionControlKind kind);

        Result<BehaviorTreeOperation> Operation(DecisionControlKind kind) {
            using enum DecisionControlKind;
            switch (kind) {
                case Root:
                case Sequence:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Sequence);
                case Selector:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Selector);
                case Parallel:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Parallel);
                case Inverter:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Inverter);
                case Cooldown:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Cooldown);
                case Loop:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Loop);
                default:
                    return DecoratorOperation(kind);
            }
        }

        Result<BehaviorTreeOperation> DecoratorOperation(DecisionControlKind kind) {
            switch (kind) {
                case DecisionControlKind::TimeLimit:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::TimeLimit);
                case DecisionControlKind::Task:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Task);
                case DecisionControlKind::Condition:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::BlackboardCheck);
                case DecisionControlKind::Subplan:
                    return Result<BehaviorTreeOperation>::Success(BehaviorTreeOperation::Subplan);
                default:
                    return Invalid<BehaviorTreeOperation>();
            }
        }

        SourceLocation NodeSource(const SourceLocation &preferred, const SourceLocation &graph) {
            return preferred.source.empty() ? graph : preferred;
        }

        Result<DecisionAssetNode> NodeMetadata(const BehaviorTreeNode &node, const SourceLocation &source) {
            auto id = DecisionNodeId::Create(node.id.Value());
            if (id.HasError())
                return Result<DecisionAssetNode>::Failure(id.ErrorValue());
            DecisionAssetNode result{.id = id.Value(),
                                     .descriptorVersion = {node.typeVersion.major, node.typeVersion.major},
                                     .requirements = node.requirements,
                                     .source = NodeSource(node.source, source)};
            if (ProviderControl(node.control.kind)) {
                auto type = DecisionNodeTypeId::Create(node.type.Value());
                if (type.HasError() || node.typeVersion.minor != 0)
                    return Invalid<DecisionAssetNode>();
                result.type = type.Value();
            } else {
                if (node.type.IsValid())
                    return Invalid<DecisionAssetNode>();
                result.intrinsic = node.control.kind;
            }
            return Result<DecisionAssetNode>::Success(std::move(result));
        }

        Result<void> AddCall(LoweredDecisionGraph &graph, DecisionNodeId owner, const DecisionSubplanCall &call,
                             const SourceLocation &source) {
            if (!call.asset.IsValid() || call.kind >= DecisionPlanKind::Utility || !call.schema.IsValid())
                return Invalid<void>(AIErrors::DecisionAssetSubtreeIncompatible);
            graph.metadata.subtrees.emplace_back(call.asset, call.kind, call.schema, owner, source);
            graph.calls.emplace_back(owner, call);
            return Result<void>::Success();
        }

        Result<void> LowerCall(LoweredDecisionGraph &graph, const BehaviorTreeNode &node, DecisionNodeId owner,
                               const SourceLocation &source) {
            if (node.control.kind != DecisionControlKind::Subplan)
                return node.call || node.nestedSubtree ? Invalid<void>() : Result<void>::Success();
            if (node.call && node.nestedSubtree)
                return Invalid<void>();
            if (node.call)
                return AddCall(graph, owner, *node.call, source);
            if (!node.nestedSubtree || node.nestedSubtree->requiredSchemaVersion.minor != 0)
                return Invalid<void>();
            auto asset = DecisionGraphAssetId::Create(node.nestedSubtree->graph.Value());
            if (asset.HasError())
                return Result<void>::Failure(asset.ErrorValue());
            const auto version = node.nestedSubtree->requiredSchemaVersion.major;
            return AddCall(graph, owner, {asset.Value(), DecisionPlanKind::BehaviorTree, {version, version}}, source);
        }

        Result<void> LowerService(LoweredDecisionGraph &graph, const BehaviorTreeAsset &tree, const BehaviorTreeNode &node,
                                  DecisionNodeId id) {
            const auto edge = std::ranges::find_if(tree.Edges(), [&](const auto &value) {
                return value.kind == BehaviorTreeEdgeKind::ServiceAttachment && value.targetNode == node.id;
            });
            if (edge == tree.Edges().end())
                return Invalid<void>();
            auto owner = DecisionNodeId::Create(edge->sourceNode.Value());
            if (owner.HasError())
                return Result<void>::Failure(owner.ErrorValue());
            graph.services.emplace_back(id, owner.Value(), node.control.serviceMode, node.control.intervalTicks);
            return Result<void>::Success();
        }

        Result<void> LowerControl(LoweredDecisionGraph &graph, const BehaviorTreeAsset &tree, const BehaviorTreeNode &node,
                                  DecisionNodeId id) {
            auto operation = Operation(node.control.kind);
            if (operation.HasError())
                return Result<void>::Failure(operation.ErrorValue());
            BehaviorTreeExecutionNode control{.id = id,
                                              .operation = operation.Value(),
                                              .parallel = node.control.parallelPolicy,
                                              .abort = node.control.abortMode,
                                              .durationTicks = node.control.durationTicks,
                                              .iterations = node.control.iterations,
                                              .task = node.task};
            for (const auto &edge : tree.Edges()) {
                if (edge.kind != BehaviorTreeEdgeKind::Child || edge.sourceNode != node.id)
                    continue;
                auto child = DecisionNodeId::Create(edge.targetNode.Value());
                if (child.HasError())
                    return Result<void>::Failure(child.ErrorValue());
                control.children.push_back(child.Value());
            }
            graph.controls.push_back(std::move(control));
            return Result<void>::Success();
        }

        Result<void> LowerTree(LoweredDecisionGraph &graph, const DecisionGraphSource &source,
                               const std::shared_ptr<const BehaviorTreeAsset> &tree) {
            if (!tree || tree->Graph().Value() != source.asset.Value())
                return Invalid<void>();
            auto root = DecisionNodeId::Create(tree->Root().Value());
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            graph.root = root.Value();
            for (const auto &node : tree->Nodes()) {
                auto metadata = NodeMetadata(node, source.location);
                if (metadata.HasError())
                    return Result<void>::Failure(metadata.ErrorValue());
                const auto id = metadata.Value().id;
                if (auto call = LowerCall(graph, node, id, metadata.Value().source); call.HasError())
                    return call;
                auto executable = node.control.kind == DecisionControlKind::Service ? LowerService(graph, *tree, node, id)
                                                                                    : LowerControl(graph, *tree, node, id);
                if (executable.HasError())
                    return executable;
                if (ProviderControl(node.control.kind))
                    graph.properties.emplace_back(id, node.properties);
                graph.metadata.nodes.push_back(std::move(metadata).Value());
            }
            return Result<void>::Success();
        }

        void AddIntrinsic(LoweredDecisionGraph &graph, DecisionNodeId id, DecisionControlKind kind,
                          std::span<const DecisionBlackboardRequirement> requirements, const SourceLocation &source) {
            graph.metadata.nodes.push_back(
                {.id = id, .requirements = {requirements.begin(), requirements.end()}, .source = source, .intrinsic = kind});
        }

        Result<void> AddAction(LoweredDecisionGraph &graph, const std::optional<TaskId> &task,
                               const std::optional<StateMachineProviderAction> &action, const SourceLocation &source) {
            if (task.has_value() != action.has_value())
                return Invalid<void>();
            if (!action)
                return Result<void>::Success();
            if (!task->IsValid() || action->binding.intrinsic != DecisionControlKind::Unspecified)
                return Invalid<void>();
            auto binding = action->binding;
            binding.source = NodeSource(binding.source, source);
            graph.properties.emplace_back(binding.id, action->properties);
            graph.metadata.nodes.push_back(std::move(binding));
            return Result<void>::Success();
        }

        Result<void> LowerState(LoweredDecisionGraph &graph, const StateMachineState &state, const SourceLocation &source) {
            if (state.terminal != DecisionStateTerminal::None && (state.update || state.exit))
                return Invalid<void>(StateMachineErrors::TopologyInvalid);
            AddIntrinsic(graph, state.id, DecisionControlKind::State, {}, NodeSource(state.source, source));
            if (state.call) {
                if (auto call = AddCall(graph, state.id, *state.call, NodeSource(state.source, source)); call.HasError())
                    return call;
            }
            if (auto entry = AddAction(graph, state.entry, state.entryProvider, source); entry.HasError())
                return entry;
            if (auto update = AddAction(graph, state.update, state.updateProvider, source); update.HasError())
                return update;
            return AddAction(graph, state.exit, state.exitProvider, source);
        }

        Result<void> LowerStateMachine(LoweredDecisionGraph &graph, const StateMachineAssetDescriptor &machine,
                                       const SourceLocation &source) {
            if (auto reachable = ValidateStateSourceReachability(machine); reachable.HasError())
                return reachable;
            graph.stateMachine = machine;
            for (const auto &state : machine.states) {
                if (auto result = LowerState(graph, state, source); result.HasError())
                    return result;
            }
            for (const auto &transition : machine.transitions)
                AddIntrinsic(graph, transition.id, DecisionControlKind::Transition, transition.requirements,
                             NodeSource(transition.location, source));
            return Result<void>::Success();
        }
    }  // namespace

    Result<LoweredDecisionGraph> LowerDecisionGraph(const DecisionGraphSource &source) {
        LoweredDecisionGraph result;
        if (source.schemaVersion != CurrentDecisionAssetSchemaVersion)
            return Invalid<LoweredDecisionGraph>();
        if (std::holds_alternative<StateMachineAssetDescriptor>(source.body) && !source.executionTask.IsValid())
            return Invalid<LoweredDecisionGraph>(AIErrors::TaskContextInvalid);
        result.metadata = {.asset = source.asset,
                           .kind = std::holds_alternative<std::shared_ptr<const BehaviorTreeAsset>>(source.body)
                                       ? DecisionPlanKind::BehaviorTree
                                       : DecisionPlanKind::StateMachine,
                           .schemaVersion = source.schemaVersion,
                           .blackboardSchema = source.blackboard,
                           .requiredBlackboardSchemaVersion = source.blackboardVersion,
                           .source = source.location};
        auto lowered = std::visit([&](const auto &body) -> Result<void> {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, std::shared_ptr<const BehaviorTreeAsset>>)
                return LowerTree(result, source, body);
            else
                return LowerStateMachine(result, body, source.location);
        }, source.body);
        if (lowered.HasError())
            return Result<LoweredDecisionGraph>::Failure(lowered.ErrorValue());
        return Result<LoweredDecisionGraph>::Success(std::move(result));
    }
}  // namespace Horo::AI::Detail
