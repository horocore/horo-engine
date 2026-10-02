#include "Horo/AI/BehaviorTreeRuntime.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>

namespace Horo::AI {
    namespace {
        /** @brief Returns a typed compiler rejection without publishing partial storage. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Checks operation-specific cardinality and finite typed policy inputs. */
        bool ValidNode(const BehaviorTreeExecutionNode &node) noexcept {
            using enum BehaviorTreeOperation;
            if (!node.id.IsValid() || node.operation >= Count || node.parallel >= BehaviorTreeParallelPolicy::Count ||
                node.abort >= BehaviorTreeAbortMode::Count || node.iterations == 0 ||
                node.children.size() > BehaviorTreeExecutionLimits::HardNodes)
                return false;
            if (node.operation == Task)
                return node.children.empty() && node.task.IsValid() && node.abort == BehaviorTreeAbortMode::None;
            if (node.task.IsValid() || (node.operation != BlackboardCheck && node.abort != BehaviorTreeAbortMode::None))
                return false;
            if (node.operation == Sequence || node.operation == Selector || node.operation == Parallel)
                return !node.children.empty();
            return node.children.size() == 1 && (node.operation != TimeLimit || node.durationTicks > 0);
        }

        /** @brief Admits every control/service identity against the shared compiled binding table. */
        Result<void> ValidateIdentities(const DecisionAssetPlan &bindings, const std::span<const BehaviorTreeExecutionNode> nodes,
                                        const std::span<const BehaviorTreeExecutionService> services) {
            std::vector<DecisionNodeId> identities;
            identities.reserve(nodes.size() + services.size());
            for (const auto &node : nodes) {
                if (!ValidNode(node))
                    return Failure<void>(AIErrors::BehaviorTreeSchemaInvalid);
                identities.push_back(node.id);
            }
            for (const auto &service : services) {
                if (!service.id.IsValid() || !service.owner.IsValid() || service.mode >= BehaviorTreeServiceMode::Count ||
                    service.intervalTicks == 0)
                    return Failure<void>(AIErrors::BehaviorTreeSchemaInvalid);
                identities.push_back(service.id);
            }
            std::ranges::sort(identities);
            for (std::size_t index = 0; index < identities.size(); ++index) {
                if (identities[index] != bindings.Nodes()[index].id || (index > 0 && identities[index] == identities[index - 1]))
                    return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);
            }
            return Result<void>::Success();
        }

        /** @brief Rejects missing/repeated structural parents and illegal lower-priority observers. */
        Result<std::vector<std::size_t>> ValidateParents(const std::span<const BehaviorTreeExecutionNode> nodes,
                                                         const std::size_t rootIndex) {
            const auto sourceIndex = [nodes](const DecisionNodeId id) {
                const auto found = std::ranges::find(nodes, id, &BehaviorTreeExecutionNode::id);
                return static_cast<std::size_t>(found - nodes.begin());
            };
            std::vector parents(nodes.size(), nodes.size());
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                for (const auto child : nodes[index].children) {
                    const std::size_t target = sourceIndex(child);
                    if (target == nodes.size() || target == rootIndex || parents[target] != nodes.size())
                        return Failure<std::vector<std::size_t>>(AIErrors::BehaviorTreeTopologyInvalid);
                    parents[target] = index;
                }
            }
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (index != rootIndex && parents[index] == nodes.size())
                    return Failure<std::vector<std::size_t>>(AIErrors::BehaviorTreeTopologyInvalid);
                const auto abort = nodes[index].abort;
                if ((abort == BehaviorTreeAbortMode::LowerPriority || abort == BehaviorTreeAbortMode::Both) &&
                    (index == rootIndex || nodes[parents[index]].operation != BehaviorTreeOperation::Selector))
                    return Failure<std::vector<std::size_t>>(AIErrors::BehaviorTreeTopologyInvalid);
            }

            return Result<std::vector<std::size_t>>::Success(std::move(parents));
        }

        /** @brief Matches full admitted bindings; schema pointer identity fences the exact publication. */
        bool CompatibleBindings(const DecisionAssetPlan &left, const DecisionAssetPlan &right) noexcept {
            if (left.Asset() != right.Asset() || left.SchemaVersion() != right.SchemaVersion() ||
                left.BlackboardSchema() != right.BlackboardSchema() || left.Nodes().size() != right.Nodes().size() ||
                left.Bindings().size() != right.Bindings().size())
                return false;
            for (std::size_t index = 0; index < left.Nodes().size(); ++index) {
                const auto &a = left.Nodes()[index];
                const auto &b = right.Nodes()[index];
                if (a.id != b.id || a.type != b.type || a.origin != b.origin || a.firstBlackboardBinding != b.firstBlackboardBinding ||
                    a.blackboardBindingCount != b.blackboardBindingCount)
                    return false;
            }
            for (std::size_t index = 0; index < left.Bindings().size(); ++index) {
                const auto &a = left.Bindings()[index];
                const auto &b = right.Bindings()[index];
                if (a.key != b.key || a.schemaIndex != b.schemaIndex || a.kind != b.kind || a.cardinality != b.cardinality ||
                    a.access != b.access || a.presence != b.presence || a.defaultValue != b.defaultValue)
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc BehaviorTreeExecutionPlan::Compile */
    Result<std::shared_ptr<const BehaviorTreeExecutionPlan>> BehaviorTreeExecutionPlan::Compile(
        std::shared_ptr<const DecisionAssetPlan> bindings, const DecisionNodeId root,
        const std::span<const BehaviorTreeExecutionNode> nodes, const std::span<const BehaviorTreeExecutionService> services,
        const BehaviorTreeExecutionLimits &limits) {
        using PlanResult = Result<std::shared_ptr<const BehaviorTreeExecutionPlan>>;
        if (limits.maximumNodes == 0 || limits.maximumNodes > BehaviorTreeExecutionLimits::HardNodes || limits.maximumDepth == 0 ||
            limits.maximumDepth > BehaviorTreeExecutionLimits::HardNodes || nodes.size() > limits.maximumNodes ||
            services.size() > limits.maximumNodes || nodes.size() + services.size() > limits.maximumNodes)
            return Failure<std::shared_ptr<const BehaviorTreeExecutionPlan>>(AIErrors::BehaviorTreeLimitExceeded);
        if (!bindings || bindings->Kind() != DecisionPlanKind::BehaviorTree || !root.IsValid() || nodes.empty() ||
            bindings->Nodes().size() != nodes.size() + services.size())
            return Failure<std::shared_ptr<const BehaviorTreeExecutionPlan>>(AIErrors::BehaviorTreeSchemaInvalid);
        // Cross-plan calls need a dependency runner; never silently execute an incomplete plan.
        if (!bindings->Dependencies().empty())
            return Failure<std::shared_ptr<const BehaviorTreeExecutionPlan>>(AIErrors::DecisionAssetSubtreeIncompatible);
        try {
            auto plan = std::make_shared<BehaviorTreeExecutionPlan>(ConstructionKey{});
            plan->bindings_ = std::move(bindings);
            if (const auto admitted = ValidateIdentities(*plan->bindings_, nodes, services); admitted.HasError())
                return PlanResult::Failure(admitted.ErrorValue());
            if (const auto topology = plan->BuildTopology(root, nodes, limits); topology.HasError())
                return PlanResult::Failure(topology.ErrorValue());
            if (const auto attached = plan->Attach(services); attached.HasError())
                return PlanResult::Failure(attached.ErrorValue());
            return PlanResult::Success(std::move(plan));
        } catch (const std::bad_alloc &) {
            return Failure<std::shared_ptr<const BehaviorTreeExecutionPlan>>(AIErrors::BehaviorTreeStorageUnavailable);
        }
    }

    /** @copydoc BehaviorTreeExecutionPlan::BuildTopology */
    Result<void> BehaviorTreeExecutionPlan::BuildTopology(const DecisionNodeId root, const std::span<const BehaviorTreeExecutionNode> nodes,
                                                          const BehaviorTreeExecutionLimits &limits) {
        const auto sourceIndex = [nodes](const DecisionNodeId id) {
            const auto found = std::ranges::find(nodes, id, &BehaviorTreeExecutionNode::id);
            return static_cast<std::size_t>(found - nodes.begin());
        };
        const std::size_t rootIndex = sourceIndex(root);
        if (rootIndex == nodes.size())
            return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
        if (const auto parents = ValidateParents(nodes, rootIndex); parents.HasError())
            return Result<void>::Failure(parents.ErrorValue());

        struct CompileFrame {
            std::size_t source;
            std::size_t nextChild{};
            std::size_t target;
        };

        std::vector<CompileFrame> stack;
        stack.reserve(limits.maximumDepth);
        std::vector<std::uint8_t> visited(nodes.size());
        nodes_.reserve(nodes.size());
        const auto append = [&](const std::size_t source, const std::size_t parent) {
            const std::size_t target = nodes_.size();
            nodes_.push_back({.execution = nodes[source], .parent = parent});
            visited[source] = true;
            stack.push_back({.source = source, .target = target});
            depth_ = std::max(depth_, stack.size());
        };
        append(rootIndex, 0);
        while (!stack.empty()) {
            auto &frame = stack.back();
            if (frame.nextChild == nodes[frame.source].children.size()) {
                nodes_[frame.target].subtreeEnd = nodes_.size();
                stack.pop_back();
                continue;
            }
            const auto source = sourceIndex(nodes[frame.source].children[frame.nextChild++]);
            if (visited[source])
                return Failure<void>(AIErrors::BehaviorTreeCycle);
            if (stack.size() == limits.maximumDepth)
                return Failure<void>(AIErrors::BehaviorTreeLimitExceeded);
            append(source, frame.target);
        }
        if (nodes_.size() != nodes.size())
            return Failure<void>(AIErrors::BehaviorTreeCycle);
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeExecutionPlan::Attach */
    Result<void> BehaviorTreeExecutionPlan::Attach(const std::span<const BehaviorTreeExecutionService> services) {
        for (auto &node : nodes_) {
            node.firstChild = children_.size();
            for (const auto child : node.execution.children) {
                const auto found = std::ranges::find_if(nodes_, [child](const auto &entry) {
                    return entry.execution.id == child;
                });
                children_.push_back(static_cast<std::size_t>(found - nodes_.begin()));
            }
            node.childCount = node.execution.children.size();
            node.firstService = services_.size();
            for (const auto &service : services)
                if (service.owner == node.execution.id)
                    services_.push_back(service);
            node.serviceCount = services_.size() - node.firstService;
        }
        if (services_.size() != services.size())
            return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
        return Result<void>::Success();
    }

    /** @copydoc BehaviorTreeExecutionPlan::IsCompatible */
    bool BehaviorTreeExecutionPlan::IsCompatible(const BehaviorTreeExecutionPlan &candidate) const noexcept {
        if (nodes_.size() != candidate.nodes_.size() || services_ != candidate.services_ ||
            !CompatibleBindings(*bindings_, *candidate.bindings_))
            return false;
        for (std::size_t index = 0; index < nodes_.size(); ++index)
            if (nodes_[index].execution != candidate.nodes_[index].execution)
                return false;
        return true;
    }
}  // namespace Horo::AI
