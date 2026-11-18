#include "../registration/DecisionNodeValidation.h"
#include "DecisionCompileDetail.h"

#include <algorithm>

namespace Horo::AI::Detail {
    namespace {
        Result<DecisionNodeRole> RequiredRole(const LoweredDecisionGraph &source, DecisionNodeId node) {
            for (const auto &control : source.controls) {
                if (control.id != node)
                    continue;
                if (control.operation == BehaviorTreeOperation::Task)
                    return Result<DecisionNodeRole>::Success(DecisionNodeRole::Task);
                if (control.operation == BehaviorTreeOperation::BlackboardCheck)
                    return Result<DecisionNodeRole>::Success(DecisionNodeRole::Condition);
            }
            if (std::ranges::find(source.services, node, &BehaviorTreeExecutionService::id) != source.services.end())
                return Result<DecisionNodeRole>::Success(DecisionNodeRole::Service);
            if (source.stateMachine || source.utility)
                return Result<DecisionNodeRole>::Success(DecisionNodeRole::Task);
            return Result<DecisionNodeRole>::Failure(MakeError(AIErrors::DecisionAssetDescriptorIncompatible));
        }

        Result<void> ValidateReferences(std::span<const BehaviorTreeProperty> properties, const DecisionAssetDescriptor &source) {
            for (const auto &property : properties) {
                const auto *reference = std::get_if<BehaviorTreeNodeId>(&property.value);
                if (!reference)
                    continue;
                const auto found = std::ranges::find_if(source.nodes, [&](const auto &node) {
                    return node.id.Value() == reference->Value();
                });
                if (found == source.nodes.end())
                    return Result<void>::Failure(MakeError(AIErrors::BehaviorTreeTopologyInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Checks that the declared key can accept one native query result. */
        bool IsWritableQueryKey(const DecisionBlackboardRequirement &requirement, BlackboardKeyId key) noexcept {
            return requirement.key == key && requirement.access == BlackboardKeyAccess::ReadWrite &&
                   requirement.cardinality == BlackboardValueCardinality::Scalar &&
                   (requirement.kind == BlackboardValueKind::WorldCoordinate || requirement.kind == BlackboardValueKind::EntityReference);
        }

        /** @brief Requires one typed writable output declaration for a query task. */
        Result<void> ValidateQueryOutput(std::span<const BehaviorTreeProperty> properties, const DecisionAssetNode &node,
                                         const DecisionNodeContract &contract) {
            const auto output = std::ranges::find(properties, BehaviorTreePropertyKind::QueryOutput, &BehaviorTreeProperty::kind);
            if (output == properties.end()) {
                if (const auto declared =
                        std::ranges::find(contract.properties, BehaviorTreePropertyKind::QueryOutput, &BehaviorTreeProperty::kind);
                    declared != contract.properties.end())
                    return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
                return Result<void>::Success();
            }
            if (contract.role != DecisionNodeRole::Task ||
                std::ranges::find(std::ranges::subrange{output + 1, properties.end()}, BehaviorTreePropertyKind::QueryOutput,
                                  &BehaviorTreeProperty::kind) != properties.end())
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            const auto key = std::get<BehaviorTreeQueryOutput>(output->value).key;
            if (const auto writable = [key](const DecisionBlackboardRequirement &requirement) {
                return IsWritableQueryKey(requirement, key);
            }; !std::ranges::any_of(node.requirements, writable) && !std::ranges::any_of(contract.descriptor.requirements, writable))
                return Result<void>::Failure(MakeError(AIErrors::BlackboardValueTypeMismatch));
            return Result<void>::Success();
        }

        Result<void> AdmitProvider(const LoweredDecisionGraph &source, const DecisionAssetNode &node,
                                   const DecisionCompilationEnvironment &environment) {
            const auto contracts = environment.nodes->Contracts();
            const auto contract = std::ranges::find(contracts, node.type, [](const auto &value) {
                return value.descriptor.type;
            });
            if (contract == contracts.end())
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorMissing));
            if (!environment.nodes->HasImplementation(node.type))
                return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
            auto role = RequiredRole(source, node.id);
            if (role.HasError())
                return Result<void>::Failure(role.ErrorValue());
            if (role.Value() != contract->role ||
                (contract->capabilities.bits & environment.capabilities.bits) != contract->capabilities.bits)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorIncompatible));
            auto configuration = std::ranges::find(source.properties, node.id, &DecisionNodeConfiguration::node);
            if (configuration == source.properties.end())
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            auto properties = BindDecisionProperties(*contract, configuration->properties);
            if (properties.HasError())
                return Result<void>::Failure(properties.ErrorValue());
            if (auto output = ValidateQueryOutput(configuration->properties, node, *contract); output.HasError())
                return output;
            return ValidateReferences(properties.Value(), source.metadata);
        }
    }  // namespace

    Result<void> ValidateDecisionProviders(const LoweredDecisionGraph &source, const DecisionCompilationEnvironment &environment) {
        if (!environment.nodes || !environment.nodes->CanUse() || !environment.capabilities.IsValid())
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        for (const auto &node : source.metadata.nodes) {
            if (node.intrinsic != DecisionControlKind::Unspecified)
                continue;
            if (auto admitted = AdmitProvider(source, node, environment); admitted.HasError())
                return admitted;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::AI::Detail
