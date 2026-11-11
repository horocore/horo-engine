#include "../registration/DecisionNodeValidation.h"
#include "Horo/AI/DecisionNodeRegistration.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        struct NodeInstance final {
            DecisionNodeId node;
            DecisionNodeRole role;
            DecisionNodeFactoryBinding factory;
            IBehaviorTreeExecutor *instance{};
            std::optional<AiTaskOperationContext> operation;
            bool aborted{};
        };

        struct CallbackRequirement final {
            DecisionNodeId node;
            DecisionNodeRole role;
        };

        std::vector<CallbackRequirement> RequiredCallbacks(const BehaviorTreeExecutionPlan &plan) {
            std::vector<CallbackRequirement> result;
            for (const auto &node : plan.Nodes()) {
                if (node.execution.operation == BehaviorTreeOperation::Task)
                    result.push_back({node.execution.id, DecisionNodeRole::Task});
                if (node.execution.operation == BehaviorTreeOperation::BlackboardCheck)
                    result.push_back({node.execution.id, DecisionNodeRole::Condition});
            }
            for (const auto &service : plan.Services())
                result.push_back({service.id, DecisionNodeRole::Service});
            return result;
        }

        bool MatchesRequirement(const DecisionPlanBlackboardBinding &binding, const DecisionBlackboardRequirement &required) {
            if (binding.kind != required.kind || binding.cardinality != required.cardinality)
                return false;
            if (required.access == BlackboardKeyAccess::ReadWrite && binding.access != BlackboardKeyAccess::ReadWrite)
                return false;
            if (required.presence == BlackboardKeyPresence::Required && binding.presence != BlackboardKeyPresence::Required)
                return false;
            return !required.requireDefault || binding.defaultValue.has_value();
        }

        Result<void> ValidateBoundRequirements(const DecisionAssetPlan &plan, const DecisionPlanNode &node,
                                               const DecisionNodeDescriptor &descriptor) {
            const auto bindings = plan.BindingsForNode(node.id);
            for (const auto &required : descriptor.requirements) {
                const auto found = std::ranges::find(bindings, required.key, &DecisionPlanBlackboardBinding::key);
                if (found == bindings.end())
                    return Failure<void>(AIErrors::DecisionAssetBindingMissing);
                if (!MatchesRequirement(*found, required))
                    return Failure<void>(AIErrors::DecisionAssetDescriptorIncompatible);
            }
            return Result<void>::Success();
        }

        /** @brief Prevents provider callbacks from recursively entering the same execution owner. */
        class CallbackScope final {
        public:
            explicit CallbackScope(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            CallbackScope(const CallbackScope &) = delete;
            CallbackScope &operator=(const CallbackScope &) = delete;

            ~CallbackScope() {
                active_ = false;
            }

        private:
            bool &active_;
        };
    }  // namespace

    class RegisteredDecisionNodeExecutor final : public IBehaviorTreeExecutor {
    public:
        explicit RegisteredDecisionNodeExecutor(std::shared_ptr<const DecisionNodeGeneration> generation)
            : generation_(std::move(generation)) {}

        ~RegisteredDecisionNodeExecutor() override {
            if (std::this_thread::get_id() != generation_->owner_ || invoking_)
                std::terminate();
            CallbackScope scope(invoking_);
            for (auto &node : nodes_) {
                Retire(node, AiTaskCancellationReason::OwnerShutdown);
                node.factory.destroy(node.factory.userData, node.instance);
            }
        }

        Result<void> Bind(const DecisionAssetPlan &plan, AiCapabilitySet capabilities,
                          std::span<const DecisionNodeConfiguration> configuration) {
            if (configuration.size() > plan.Nodes().size())
                return Failure<void>(AIErrors::DecisionAssetSchemaInvalid);
            if (auto valid = ValidateConfiguration(plan, configuration); valid.HasError())
                return valid;
            CallbackScope scope(invoking_);
            const auto providerCount = std::ranges::count_if(plan.Nodes(), [](const auto &node) {
                return node.intrinsic == DecisionControlKind::Unspecified;
            });
            nodes_.reserve(static_cast<std::size_t>(providerCount));
            for (const auto &node : plan.Nodes()) {
                if (node.intrinsic != DecisionControlKind::Unspecified)
                    continue;
                if (auto bound = BindNode(plan, node, capabilities, configuration); bound.HasError())
                    return bound;
            }
            return Result<void>::Success();
        }

        Result<void> BindExecutable(const BehaviorTreeExecutionPlan &execution, AiCapabilitySet capabilities,
                                    std::span<const DecisionNodeConfiguration> configuration) {
            const auto &plan = execution.Bindings();
            if (auto valid = ValidateConfiguration(plan, configuration); valid.HasError())
                return valid;
            const auto callbacks = RequiredCallbacks(execution);
            CallbackScope scope(invoking_);
            nodes_.reserve(callbacks.size());
            for (const auto &required : callbacks) {
                const auto node = std::ranges::find(plan.Nodes(), required.node, &DecisionPlanNode::id);
                if (node == plan.Nodes().end())
                    return Failure<void>(AIErrors::DecisionAssetDescriptorMissing);
                auto resolved = Resolve(*node, capabilities);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                if (generation_->contracts_[resolved.Value()].role != required.role)
                    return Failure<void>(AIErrors::DecisionAssetSchemaInvalid);
                if (auto bound = BindNode(plan, *node, capabilities, configuration); bound.HasError())
                    return bound;
            }
            return Result<void>::Success();
        }

        void Pump() noexcept override {
            if (!OwnerAvailable())
                return;
            CallbackScope scope(invoking_);
            for (auto &node : nodes_) {
                if (generation_->CanAdmit())
                    node.instance->Pump();
                else
                    Retire(node, AiTaskCancellationReason::AgentGenerationRetired);
            }
        }

        Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &operation) noexcept override {
            if (!Callable())
                return Failure<AiTaskState>(AIErrors::RuntimeUnavailable);
            auto *node = Find(context.node, DecisionNodeRole::Task);
            if (!node || node->operation)
                return Failure<AiTaskState>(AIErrors::TaskTransitionInvalid);
            CallbackScope scope(invoking_);
            node->operation = operation;
            node->aborted = false;
            return node->instance->Start(context, operation);
        }

        Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context,
                                   const AiTaskOperationContext &operation) noexcept override {
            if (!Callable())
                return Failure<AiTaskState>(AIErrors::RuntimeUnavailable);
            auto *node = FindOperation(operation);
            if (!node || node->node != context.node || node->aborted)
                return Failure<AiTaskState>(AIErrors::TaskContextInvalid);
            CallbackScope scope(invoking_);
            return node->instance->Resume(context, operation);
        }

        void Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason reason) noexcept override {
            if (!OwnerAvailable())
                return;
            auto *node = FindOperation(operation);
            if (!node || node->aborted)
                return;
            CallbackScope scope(invoking_);
            node->aborted = true;
            node->instance->Abort(operation, reason);
        }

        void Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept override {
            if (!OwnerAvailable())
                return;
            auto *node = FindOperation(operation);
            if (!node)
                return;
            CallbackScope scope(invoking_);
            node->operation.reset();
            node->instance->Cleanup(operation, result);
        }

        Result<bool> Check(const BehaviorTreeEvaluationContext &context) noexcept override {
            if (!Callable())
                return Failure<bool>(AIErrors::RuntimeUnavailable);
            auto *node = Find(context.node, DecisionNodeRole::Condition);
            if (!node)
                return Failure<bool>(AIErrors::DecisionAssetDescriptorMissing);
            CallbackScope scope(invoking_);
            return node->instance->Check(context);
        }

        Result<void> Service(const BehaviorTreeEvaluationContext &context) noexcept override {
            if (!Callable())
                return Failure<void>(AIErrors::RuntimeUnavailable);
            auto *node = Find(context.node, DecisionNodeRole::Service);
            if (!node)
                return Failure<void>(AIErrors::DecisionAssetDescriptorMissing);
            CallbackScope scope(invoking_);
            return node->instance->Service(context);
        }

    private:
        static Result<void> ValidateConfiguration(const DecisionAssetPlan &plan, std::span<const DecisionNodeConfiguration> configuration) {
            if (configuration.size() > plan.Nodes().size())
                return Failure<void>(AIErrors::DecisionAssetLimitExceeded);
            std::vector<DecisionNodeId> seen;
            for (const auto &entry : configuration) {
                const auto node = std::ranges::find(plan.Nodes(), entry.node, &DecisionPlanNode::id);
                if (node == plan.Nodes().end() || node->intrinsic != DecisionControlKind::Unspecified)
                    return Failure<void>(AIErrors::DecisionAssetSchemaInvalid);
                if (std::ranges::find(seen, entry.node) != seen.end())
                    return Failure<void>(AIErrors::DescriptorConflict);
                seen.push_back(entry.node);
            }
            return Result<void>::Success();
        }

        Result<std::size_t> Resolve(const DecisionPlanNode &node, AiCapabilitySet capabilities) const {
            const auto &contracts = generation_->contracts_;
            const auto found = std::ranges::find(contracts, node.type, [](const auto &contract) {
                return contract.descriptor.type;
            });
            if (found == contracts.end())
                return Failure<std::size_t>(AIErrors::DecisionAssetDescriptorMissing);
            if (found->descriptor.origin != node.origin)
                return Failure<std::size_t>(AIErrors::DecisionAssetDescriptorIncompatible);
            if ((capabilities.bits & found->capabilities.bits) != found->capabilities.bits)
                return Failure<std::size_t>(AIErrors::CapabilityUnavailable);
            const auto index = static_cast<std::size_t>(found - contracts.begin());
            if (!generation_->factories_[index].create)
                return Failure<std::size_t>(AIErrors::DecisionAssetDescriptorMissing);
            return Result<std::size_t>::Success(index);
        }

        Result<void> BindNode(const DecisionAssetPlan &plan, const DecisionPlanNode &node, AiCapabilitySet capabilities,
                              std::span<const DecisionNodeConfiguration> configuration) {
            auto resolved = Resolve(node, capabilities);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            const auto index = resolved.Value();
            const auto &contract = generation_->contracts_[index];
            if (auto matched = ValidateBoundRequirements(plan, node, contract.descriptor); matched.HasError())
                return matched;
            const auto found = std::ranges::find(configuration, node.id, &DecisionNodeConfiguration::node);
            const std::span<const BehaviorTreeProperty> values =
                found == configuration.end() ? std::span<const BehaviorTreeProperty>{} : found->properties;
            auto properties = Detail::BindDecisionProperties(contract, values);
            if (properties.HasError())
                return Result<void>::Failure(properties.ErrorValue());
            const auto factory = generation_->factories_[index];
            auto *instance = factory.create(factory.userData, properties.Value());
            if (!instance)
                return Failure<void>(AIErrors::RuntimeUnavailable);
            nodes_.push_back({.node = node.id, .role = contract.role, .factory = factory, .instance = instance});
            return Result<void>::Success();
        }

        bool OwnerAvailable() const noexcept {
            return std::this_thread::get_id() == generation_->owner_ && !invoking_;
        }

        bool Callable() const noexcept {
            return OwnerAvailable() && generation_->CanAdmit();
        }

        NodeInstance *Find(DecisionNodeId id, DecisionNodeRole role) noexcept {
            const auto found = std::ranges::find(nodes_, id, &NodeInstance::node);
            return found != nodes_.end() && found->role == role ? &*found : nullptr;
        }

        NodeInstance *FindOperation(const AiTaskOperationContext &operation) noexcept {
            for (auto &node : nodes_) {
                if (!node.operation)
                    continue;
                const auto &owned = *node.operation;
                if (owned.task == operation.task && owned.agent == operation.agent && owned.taskDefinition == operation.taskDefinition)
                    return &node;
            }
            return nullptr;
        }

        static void Retire(NodeInstance &node, AiTaskCancellationReason reason) noexcept {
            if (!node.operation)
                return;
            const auto operation = std::move(*node.operation);
            node.operation.reset();
            if (!node.aborted)
                node.instance->Abort(operation, reason);
            node.instance->Cleanup(operation, {.state = AiTaskState::Cancelled, .failure = {}, .cancellationReason = reason});
            node.aborted = true;
        }

        std::shared_ptr<const DecisionNodeGeneration> generation_;
        std::vector<NodeInstance> nodes_;
        bool invoking_{};
    };

    std::uint64_t DecisionNodeGeneration::ExecutorStorageBytes(std::size_t providers) noexcept {
        if (providers > DecisionAssetValidationHardLimits::NodesPerAsset)
            return std::numeric_limits<std::uint64_t>::max();
        return sizeof(RegisteredDecisionNodeExecutor) + providers * sizeof(NodeInstance);
    }

    Result<std::unique_ptr<IBehaviorTreeExecutor>> DecisionNodeGeneration::CreateExecutor(
        std::shared_ptr<const DecisionNodeGeneration> self, const DecisionAssetPlan &plan, AiCapabilitySet capabilities,
        const std::span<const DecisionNodeConfiguration> configuration) const {
        using Output = Result<std::unique_ptr<IBehaviorTreeExecutor>>;
        if (self.get() != this || !CanAdmit() || !capabilities.IsValid())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        try {
            auto result = std::make_unique<RegisteredDecisionNodeExecutor>(std::move(self));
            if (auto bound = result->Bind(plan, capabilities, configuration); bound.HasError())
                return Output::Failure(bound.ErrorValue());
            if (!CanAdmit())
                return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
            return Output::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        } catch (...) {
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        }
    }

    Result<std::unique_ptr<IBehaviorTreeExecutor>> DecisionNodeGeneration::CreateExecutor(
        std::shared_ptr<const DecisionNodeGeneration> self, const BehaviorTreeExecutionPlan &plan, AiCapabilitySet capabilities,
        std::span<const DecisionNodeConfiguration> configuration) const {
        using Output = Result<std::unique_ptr<IBehaviorTreeExecutor>>;
        if (self.get() != this || !CanAdmit() || !capabilities.IsValid())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        try {
            auto result = std::make_unique<RegisteredDecisionNodeExecutor>(std::move(self));
            if (auto bound = result->BindExecutable(plan, capabilities, configuration); bound.HasError())
                return Output::Failure(bound.ErrorValue());
            if (!CanAdmit())
                return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
            return Output::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        } catch (...) {
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        }
    }
}  // namespace Horo::AI
