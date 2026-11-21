#include "DecisionCompileDetail.h"
#include "DecisionSourceReachability.h"
#include "Horo/AI/BlackboardUsageInspection.h"

#include <algorithm>
#include <new>

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

        /** @brief Major versions alone identify the native/tree provider wire contract. */
        DecisionVersionRange TreeProviderVersions(const BehaviorTreeNode &node) {
            return {node.typeVersion.major, node.typeVersion.major};
        }

        SourceLocation NodeSource(const SourceLocation &preferred, const SourceLocation &graph) {
            return preferred.source.empty() ? graph : preferred;
        }

        Result<DecisionAssetNode> NodeMetadata(const BehaviorTreeNode &node, const SourceLocation &source) {
            auto id = DecisionNodeId::Create(node.id.Value());
            if (id.HasError())
                return Result<DecisionAssetNode>::Failure(id.ErrorValue());
            DecisionAssetNode result{.id = id.Value(),
                                     .descriptorVersion = TreeProviderVersions(node),
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
            if (!call.asset.IsValid() || call.kind >= DecisionPlanKind::Count || !call.schema.IsValid())
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
                if (auto executable = node.control.kind == DecisionControlKind::Service ? LowerService(graph, *tree, node, id)
                                                                                        : LowerControl(graph, *tree, node, id);
                    executable.HasError())
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
                                       : (std::holds_alternative<StateMachineAssetDescriptor>(source.body) ? DecisionPlanKind::StateMachine
                                                                                                           : DecisionPlanKind::Utility),
                           .schemaVersion = source.schemaVersion,
                           .blackboardSchema = source.blackboard,
                           .requiredBlackboardSchemaVersion = source.blackboardVersion,
                           .source = source.location};
        if (auto lowered = std::visit(
                [&]<typename VisitedValue>(const VisitedValue &body) -> Result<void> {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, std::shared_ptr<const BehaviorTreeAsset>>)
                return LowerTree(result, source, body);
            else if constexpr (std::is_same_v<Body, StateMachineAssetDescriptor>)
                return LowerStateMachine(result, body, source.location);
            else
                return LowerUtility(result, body, source.location);
        }, source.body);
            lowered.HasError())
            return Result<LoweredDecisionGraph>::Failure(lowered.ErrorValue());
        return Result<LoweredDecisionGraph>::Success(std::move(result));
    }
}  // namespace Horo::AI::Detail

namespace Horo::AI {
    /** @brief Retains captured descriptor values without retaining their original owner. */
    DecisionBlackboardProviderMetadata::DecisionBlackboardProviderMetadata(std::vector<DecisionNodeDescriptor> descriptors)
        : descriptors_(std::move(descriptors)) {}

    /** @copydoc DecisionBlackboardProviderMetadata::Capture */
    Result<std::shared_ptr<const DecisionBlackboardProviderMetadata>> DecisionBlackboardProviderMetadata::Capture(
        const DecisionNodeGeneration &generation) {
        using Output = Result<std::shared_ptr<const DecisionBlackboardProviderMetadata>>;
        if (!generation.CanUse())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        try {
            std::vector<DecisionNodeDescriptor> descriptors;
            descriptors.reserve(generation.Contracts().size());
            for (const auto &contract : generation.Contracts())
                descriptors.push_back(contract.descriptor);
            if (!generation.CanUse())
                return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
            return Output::Success(
                std::shared_ptr<const DecisionBlackboardProviderMetadata>{new DecisionBlackboardProviderMetadata(std::move(descriptors))});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    namespace {
        /** @brief Finds the detached declaration for an exact stable type. */
        const DecisionNodeDescriptor *FindProviderDeclaration(std::span<const DecisionNodeDescriptor> descriptors,
                                                              DecisionNodeTypeId type) {
            const auto found = std::ranges::lower_bound(descriptors, type, {}, &DecisionNodeDescriptor::type);
            return found != descriptors.end() && found->type == type ? &*found : nullptr;
        }

        /** @brief Resolves one immutable authored binding without loading or starting its provider. */
        std::optional<DecisionProviderBindingIssueKind> InspectProviderDeclaration(const DecisionBlackboardProviderMetadata *providers,
                                                                                   DecisionNodeTypeId type, DecisionVersionRange versions) {
            using enum DecisionProviderBindingIssueKind;
            if (!providers)
                return CatalogUnavailable;
            const auto *found = FindProviderDeclaration(providers->Descriptors(), type);
            if (!found)
                return DescriptorMissing;
            if (!versions.Contains(found->origin.version))
                return DescriptorVersionMismatch;
            return std::nullopt;
        }

        /** @brief Independent source inspection preserves known nodes when another node cannot be lowered. */
        void InspectTreeProviderDeclarations(const BehaviorTreeAsset &tree, const DecisionBlackboardProviderMetadata *providers,
                                             std::vector<DecisionProviderBindingIssue> &out) {
            for (const auto &node : tree.Nodes()) {
                if (node.kind == BehaviorTreeNodeKind::Unknown || !Detail::ProviderControl(node.control.kind))
                    continue;
                const auto id = DecisionNodeId::Create(node.id.Value()).Value();
                const auto type = DecisionNodeTypeId::Create(node.type.Value());
                if (node.typeVersion.minor != 0) {
                    out.push_back({id, DecisionProviderBindingIssueKind::DescriptorVersionMismatch});
                } else if (type.HasError()) {
                    out.push_back({id, DecisionProviderBindingIssueKind::DescriptorMissing});
                } else if (const auto issue = InspectProviderDeclaration(providers, type.Value(), Detail::TreeProviderVersions(node))) {
                    out.push_back({id, *issue});
                }
            }
        }

        /** @brief The owning state displays the first unresolved authored action in entry/update/exit order. */
        void InspectStateProviderDeclarations(const StateMachineAssetDescriptor &machine,
                                              const DecisionBlackboardProviderMetadata *providers,
                                              std::vector<DecisionProviderBindingIssue> &out) {
            for (const auto &state : machine.states) {
                for (const auto *action : {&state.entryProvider, &state.updateProvider, &state.exitProvider}) {
                    if (!*action)
                        continue;
                    const auto &binding = action->value().binding;
                    if (const auto issue = InspectProviderDeclaration(providers, binding.type, binding.descriptorVersion)) {
                        out.push_back({state.id, *issue});
                        break;
                    }
                }
            }
        }

        /** @brief Collects authored query outputs independently of provider availability. */
        Result<void> CollectQueryOutputs(std::span<const DecisionNodeConfiguration> configurations, BlackboardKeyId key,
                                         std::vector<DecisionBlackboardQueryUse> &outputs) {
            for (const auto &configuration : configurations) {
                for (const auto &property : configuration.properties) {
                    const auto *output = std::get_if<BehaviorTreeQueryOutput>(&property.value);
                    if (!output || output->key != key)
                        continue;
                    if (outputs.size() == DecisionAssetValidationHardLimits::TotalRequirements)
                        return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
                    outputs.emplace_back(configuration.node, *output);
                }
            }
            return Result<void>::Success();
        }

        /** @brief Copies matching requirement evidence with its originating declaration. */
        bool CollectMatchingRequirements(std::vector<DecisionBlackboardSourceUse> &uses, DecisionNodeId node, BlackboardKeyId key,
                                         std::span<const DecisionBlackboardRequirement> requirements,
                                         std::optional<DecisionDescriptorOrigin> origin = {}) {
            for (const auto &requirement : requirements) {
                if (requirement.key != key)
                    continue;
                if (uses.size() == DecisionAssetValidationHardLimits::TotalRequirements)
                    return false;
                uses.emplace_back(node, requirement, origin);
            }
            return true;
        }
    }  // namespace

    /** @copydoc InspectDecisionProviderBindings */
    std::vector<DecisionProviderBindingIssue> InspectDecisionProviderBindings(const DecisionGraphSource &source,
                                                                              const DecisionBlackboardProviderMetadata *providers) {
        std::vector<DecisionProviderBindingIssue> out;
        if (const auto *tree = std::get_if<std::shared_ptr<const BehaviorTreeAsset>>(&source.body)) {
            if (*tree)
                InspectTreeProviderDeclarations(**tree, providers, out);
        } else if (const auto *machine = std::get_if<StateMachineAssetDescriptor>(&source.body)) {
            InspectStateProviderDeclarations(*machine, providers, out);
        } else {
            for (const auto &option : std::get<UtilityDecisionSource>(source.body).options) {
                const auto &binding = option.provider;
                if (const auto issue = InspectProviderDeclaration(providers, binding.type, binding.descriptorVersion))
                    out.push_back({option.option.id, *issue});
            }
        }
        std::ranges::sort(out, {}, &DecisionProviderBindingIssue::node);
        return out;
    }

    /** @copydoc InspectDecisionBlackboardUses */
    Result<DecisionBlackboardSourceUses> InspectDecisionBlackboardUses(const DecisionGraphSource &source, BlackboardKeyId key,
                                                                       const DecisionBlackboardProviderMetadata *providers) {
        using Report = Result<DecisionBlackboardSourceUses>;
        if (!key.IsValid())
            return Report::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
        auto lowered = Detail::LowerDecisionGraph(source);
        if (lowered.HasError())
            return Report::Failure(lowered.ErrorValue());
        DecisionBlackboardSourceUses result;
        const auto contracts = providers ? providers->Descriptors() : std::span<const DecisionNodeDescriptor>{};
        for (const auto &node : lowered.Value().metadata.nodes) {
            if (!CollectMatchingRequirements(result.uses, node.id, key, node.requirements))
                return Report::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            if (node.intrinsic != DecisionControlKind::Unspecified)
                continue;
            const auto *found = FindProviderDeclaration(contracts, node.type);
            if (!found || !node.descriptorVersion.Contains(found->origin.version)) {
                result.unresolvedProviders.push_back(node.id);
                continue;
            }
            if (!CollectMatchingRequirements(result.uses, node.id, key, found->requirements, found->origin))
                return Report::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
        }
        if (auto outputs = CollectQueryOutputs(lowered.Value().properties, key, result.queryOutputs); outputs.HasError())
            return Report::Failure(outputs.ErrorValue());
        return Report::Success(std::move(result));
    }
}  // namespace Horo::AI
