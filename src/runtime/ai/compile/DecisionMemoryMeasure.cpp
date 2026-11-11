#include "DecisionCompileDetail.h"
#include "Horo/AI/DecisionGraphInstance.h"

#include <algorithm>
#include <limits>

namespace Horo::AI::Detail {
    namespace {
        std::uint64_t PropertyBytes(const BehaviorTreeProperty &property) {
            auto bytes = static_cast<std::uint64_t>(property.displayName.capacity());
            if (const auto *string = std::get_if<std::string>(&property.value))
                bytes += string->capacity();
            if (const auto *opaque = std::get_if<BehaviorTreeOpaqueProperty>(&property.value))
                bytes += opaque->bytes.capacity();
            return bytes;
        }

        std::uint64_t PropertiesBytes(const std::vector<BehaviorTreeProperty> &properties) {
            auto bytes = properties.capacity() * sizeof(BehaviorTreeProperty);
            for (const auto &property : properties)
                bytes += PropertyBytes(property);
            return bytes;
        }

        std::uint64_t RequirementsBytes(const std::vector<DecisionBlackboardRequirement> &requirements) {
            auto bytes = requirements.capacity() * sizeof(DecisionBlackboardRequirement);
            for (const auto &requirement : requirements)
                bytes += requirement.source.source.capacity();
            return bytes;
        }

        std::uint64_t ActionBytes(const std::optional<StateMachineProviderAction> &action) {
            if (!action)
                return 0;
            return PropertiesBytes(action->properties) + RequirementsBytes(action->binding.requirements) +
                   action->binding.source.source.capacity();
        }

        std::uint64_t MachineNestedBytes(const CookedStateMachinePlan &machine) {
            std::uint64_t bytes{};
            for (const auto &state : machine.States())
                bytes += state.source.source.capacity() + ActionBytes(state.entryProvider) + ActionBytes(state.updateProvider) +
                         ActionBytes(state.exitProvider);
            for (const auto &transition : machine.Transitions())
                bytes += transition.guards.capacity() * sizeof(StateMachineGuard) + transition.location.source.capacity() +
                         RequirementsBytes(transition.requirements);
            return bytes;
        }

        Result<void> Add(std::uint64_t &target, std::uint64_t amount) {
            if (amount > std::numeric_limits<std::uint64_t>::max() - target)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
            target += amount;
            return Result<void>::Success();
        }

        Result<void> ChargeProvider(DecisionStorageRequirements &storage, const DecisionNodeContract &contract) {
            const auto &memory = contract.memory;
            switch (memory.kind) {
                case DecisionProviderMemoryKind::UnknownNative:
                    ++storage.unknownNativeProviders;
                    break;
                case DecisionProviderMemoryKind::NativeDeclared: {
                    ++storage.nativeDeclaredProviders;
                    if (auto state = Add(storage.nativeDeclaredBytes, memory.stateBytes); state.HasError())
                        return state;
                    return Add(storage.nativeDeclaredBytes, memory.scratchBytes);
                }
                case DecisionProviderMemoryKind::EnforcedVm: {
                    if (auto state = Add(storage.coreInstanceBytes, memory.stateBytes); state.HasError())
                        return state;
                    if (auto scratch = Add(storage.scratchBytes, memory.scratchBytes); scratch.HasError())
                        return scratch;
                    return Add(storage.enforcedVmBytes, memory.enforcedVmBytes);
                }
                default:
                    return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            }
            return Result<void>::Success();
        }

        Result<void> ChargeProviders(DecisionStorageRequirements &storage, const DecisionCompiledGraph &graph,
                                     const DecisionCompilationEnvironment &environment) {
            std::size_t providers{};
            for (const auto &node : graph.bindings->Nodes()) {
                if (node.intrinsic != DecisionControlKind::Unspecified)
                    continue;
                const auto contracts = environment.nodes->Contracts();
                const auto contract = std::ranges::find(contracts, node.type, [](const auto &value) {
                    return value.descriptor.type;
                });
                if (contract == contracts.end())
                    return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorMissing));
                if (auto charged = ChargeProvider(storage, *contract); charged.HasError())
                    return charged;
                ++providers;
            }
            if (std::holds_alternative<std::shared_ptr<const CookedStateMachinePlan>>(graph.execution))
                storage.taskSlots += providers;
            const auto executorBytes = std::holds_alternative<std::shared_ptr<const CookedUtilityPlan>>(graph.execution)
                                           ? providers * DecisionNodeGeneration::ExecutorStorageBytes(1)
                                           : DecisionNodeGeneration::ExecutorStorageBytes(providers);
            return Add(storage.coreInstanceBytes, executorBytes);
        }
    }  // namespace

    Result<DecisionStorageRequirements> MeasureDecisionGraph(const DecisionCompiledGraph &graph,
                                                             const DecisionCompilationEnvironment &environment) {
        DecisionStorageRequirements storage;
        const auto kernel = std::visit([](const auto &plan) {
            return plan->Storage();
        }, graph.execution);
        storage.compiledBytes = kernel.compiledBytes + sizeof(DecisionCompiledGraph);
        storage.coreInstanceBytes = kernel.instanceBytes + DecisionGraphInstance::CoreStorageBytes(graph);
        storage.scratchBytes = kernel.scratchBytes;
        storage.taskSlots = kernel.taskSlots;
        storage.maximumExecutionDepth = kernel.maximumDepth;
        const auto &bindings = *graph.bindings;
        storage.compiledBytes += bindings.StorageBytes();
        storage.compiledBytes += graph.providerProperties.capacity() * sizeof(DecisionNodeConfiguration);
        for (const auto &configuration : graph.providerProperties)
            storage.compiledBytes += PropertiesBytes(configuration.properties);
        storage.compiledBytes += graph.calls.capacity() * sizeof(DecisionCallSite);
        if (const auto *machine = std::get_if<std::shared_ptr<const CookedStateMachinePlan>>(&graph.execution))
            storage.compiledBytes += MachineNestedBytes(**machine);
        if (auto charged = ChargeProviders(storage, graph, environment); charged.HasError())
            return Result<DecisionStorageRequirements>::Failure(charged.ErrorValue());
        return Result<DecisionStorageRequirements>::Success(storage);
    }
}  // namespace Horo::AI::Detail
