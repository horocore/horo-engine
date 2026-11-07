#include "Horo/AI/DecisionNodeRegistration.h"

#include "../registration/DecisionNodeValidation.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        }

        bool ValidStringProperty(const BehaviorTreeProperty &property) noexcept {
            const auto *value = std::get_if<std::string>(&property.value);
            return value && value->size() <= BehaviorTreeAssetHardLimits::PropertyBytes && value->find('\0') == std::string::npos;
        }

        bool ValidPropertyValue(const BehaviorTreeProperty &property) noexcept {
            using enum BehaviorTreePropertyKind;
            switch (property.kind) {
                case Boolean:
                    return std::holds_alternative<bool>(property.value);
                case SignedInteger:
                    return std::holds_alternative<std::int64_t>(property.value);
                case Scalar: {
                    const auto *value = std::get_if<double>(&property.value);
                    return value && std::isfinite(*value);
                }
                case String:
                    return ValidStringProperty(property);
                case NodeReference: {
                    const auto *value = std::get_if<BehaviorTreeNodeId>(&property.value);
                    return value && value->IsValid();
                }
                default:
                    return false;  // Unknown properties remain authored data but cannot become executable defaults.
            }
        }

        bool ValidPort(const DecisionNodePort &port) noexcept {
            return port.id != 0 && port.direction < DecisionPortDirection::Count && port.kind < BlackboardValueKind::Count &&
                   port.cardinality < BlackboardValueCardinality::Count;
        }

        Result<void> ValidatePorts(const std::span<const DecisionNodePort> ports) {
            std::vector<std::uint64_t> ids;
            for (const auto &port : ports) {
                if (!ValidPort(port))
                    return Invalid<void>();
                ids.push_back(port.id);
            }
            std::ranges::sort(ids);
            if (std::ranges::adjacent_find(ids) != ids.end())
                return Invalid<void>();
            return Result<void>::Success();
        }

        Result<void> ValidateDefaults(const std::span<const BehaviorTreeProperty> properties) {
            std::vector<BehaviorTreePropertyId> ids;
            for (const auto &property : properties) {
                if (!Detail::ValidDecisionProperty(property))
                    return Invalid<void>();
                ids.push_back(property.id);
            }
            std::ranges::sort(ids);
            if (std::ranges::adjacent_find(ids) != ids.end())
                return Invalid<void>();
            return Result<void>::Success();
        }

        bool ValidSource(const SourceLocation &source) noexcept {
            return !source.source.empty() && source.source.size() <= 4096 && (source.line != 0 || source.column == 0);
        }

        bool ValidRequirement(const DecisionBlackboardRequirement &requirement) noexcept {
            return requirement.key.IsValid() && requirement.kind < BlackboardValueKind::Count &&
                   requirement.cardinality < BlackboardValueCardinality::Count && requirement.access < BlackboardKeyAccess::Count &&
                   requirement.presence < BlackboardKeyPresence::Count && ValidSource(requirement.source);
        }

        Result<void> ValidateRequirements(const DecisionNodeDescriptor &descriptor) {
            if (!ValidSource(descriptor.source))
                return Invalid<void>();
            for (const auto &requirement : descriptor.requirements)
                if (!ValidRequirement(requirement))
                    return Invalid<void>();
            return Result<void>::Success();
        }

        bool ValidContractIdentity(const DecisionNodeContract &contract) noexcept {
            const auto &descriptor = contract.descriptor;
            return descriptor.type.IsValid() && descriptor.origin.provider.IsValid() && descriptor.origin.version != 0 &&
                   descriptor.origin.kind < DecisionDescriptorSourceKind::Count && contract.role < DecisionNodeRole::Count;
        }

        bool ValidContractMemory(const DecisionNodeContract &contract) noexcept {
            const auto &memory = contract.memory;
            if (memory.stateBytes > 256ULL * 1024ULL * 1024ULL || memory.scratchBytes > 256ULL * 1024ULL * 1024ULL)
                return false;
            switch (memory.kind) {
                case DecisionProviderMemoryKind::UnknownNative:
                    return memory.stateBytes == 0 && memory.scratchBytes == 0 && memory.enforcedVmBytes == 0;
                case DecisionProviderMemoryKind::NativeDeclared:
                    return memory.enforcedVmBytes == 0;
                case DecisionProviderMemoryKind::EnforcedVm:
                    return contract.descriptor.origin.kind == DecisionDescriptorSourceKind::Script && memory.enforcedVmBytes > 0 &&
                           memory.enforcedVmBytes <= 16ULL * 1024ULL * 1024ULL;
                default:
                    return false;
            }
        }

        bool ValidContractBounds(const DecisionNodeContract &contract) noexcept {
            return contract.capabilities.IsValid() && contract.ports.size() <= 128 && contract.properties.size() <= 64 &&
                   contract.descriptor.requirements.size() <= DecisionAssetValidationHardLimits::RequirementsPerNode &&
                   contract.descriptor.source.source.size() <= 4096;
        }

        Result<void> ValidateDeclarations(std::span<const DecisionNodeRegistration> registrations, bool vmAuthority) {
            for (const auto &entry : registrations) {
                if (!vmAuthority && entry.contract.memory.kind == DecisionProviderMemoryKind::EnforcedVm)
                    return Invalid<void>();
                if (auto valid = ValidateDecisionNodeRegistration(entry); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    namespace Detail {
        bool ValidDecisionProperty(const BehaviorTreeProperty &property) noexcept {
            return property.id.IsValid() && property.displayName.size() <= BehaviorTreeAssetHardLimits::DisplayNameBytes &&
                   property.displayName.find('\0') == std::string::npos && ValidPropertyValue(property);
        }

        Result<std::vector<BehaviorTreeProperty>> BindDecisionProperties(const DecisionNodeContract &contract,
                                                                         const std::span<const BehaviorTreeProperty> authored) {
            if (authored.size() > contract.properties.size())
                return Invalid<std::vector<BehaviorTreeProperty>>();
            auto result = contract.properties;
            std::vector<BehaviorTreePropertyId> seen;
            for (const auto &value : authored) {
                if (!ValidDecisionProperty(value))
                    return Invalid<std::vector<BehaviorTreeProperty>>();
                const auto found = std::ranges::find(result, value.id, &BehaviorTreeProperty::id);
                if (found == result.end() || found->kind != value.kind)
                    return Invalid<std::vector<BehaviorTreeProperty>>();
                if (std::ranges::find(seen, value.id) != seen.end())
                    return Invalid<std::vector<BehaviorTreeProperty>>();
                seen.push_back(value.id);
                *found = value;
            }
            return Result<std::vector<BehaviorTreeProperty>>::Success(std::move(result));
        }
    }  // namespace Detail

    Result<void> ValidateDecisionNodeRegistration(const DecisionNodeRegistration &registration) {
        try {
            const auto &contract = registration.contract;
            const auto &descriptor = contract.descriptor;
            if (!ValidContractIdentity(contract))
                return Invalid<void>();
            if (!ValidContractBounds(contract) || !ValidContractMemory(contract))
                return Invalid<void>();
            if ((registration.factory.create == nullptr) != (registration.factory.destroy == nullptr))
                return Invalid<void>();
            if (auto requirements = ValidateRequirements(descriptor); requirements.HasError())
                return requirements;
            if (auto ports = ValidatePorts(contract.ports); ports.HasError())
                return ports;
            return ValidateDefaults(contract.properties);

        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    Result<std::shared_ptr<const DecisionNodeGeneration>> DecisionNodeGeneration::Capture(
        const std::span<const DecisionNodeRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
        std::shared_ptr<const DecisionNodeGeneration> provider) {
        return CaptureAdmitted(registrations, std::move(admission), std::move(provider), false);
    }

    Result<std::shared_ptr<const DecisionNodeGeneration>> DecisionNodeGeneration::CaptureVm(
        std::span<const DecisionNodeRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
        std::shared_ptr<const DecisionNodeGeneration> provider) {
        return CaptureAdmitted(registrations, std::move(admission), std::move(provider), true);
    }

    Result<std::shared_ptr<const DecisionNodeGeneration>> DecisionNodeGeneration::CaptureAdmitted(
        const std::span<const DecisionNodeRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
        std::shared_ptr<const DecisionNodeGeneration> provider, const bool vmAuthority) {
        using Output = Result<std::shared_ptr<const DecisionNodeGeneration>>;
        if (!admission || registrations.size() > MaximumNodes)
            return Invalid<std::shared_ptr<const DecisionNodeGeneration>>();
        if (!ValidProvider(provider))
            return Invalid<std::shared_ptr<const DecisionNodeGeneration>>();
        try {
            if (auto declarations = ValidateDeclarations(registrations, vmAuthority); declarations.HasError())
                return Output::Failure(declarations.ErrorValue());
            auto result = std::make_shared<DecisionNodeGeneration>();
            result->admission_ = std::move(admission);
            result->provider_ = std::move(provider);
            result->owner_ = std::this_thread::get_id();
            if (auto stored = result->StoreDeclarations({registrations.begin(), registrations.end()}); stored.HasError())
                return Output::Failure(stored.ErrorValue());
            return Output::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    bool DecisionNodeGeneration::CanAdmit() const noexcept {
        return std::this_thread::get_id() == owner_ && CanUse();
    }

    bool DecisionNodeGeneration::CanUse() const noexcept {
        if (provider_ && !provider_->CanUse())
            return false;
        for (const auto &parent : parents_)
            if (!parent->CanUse())
                return false;
        return admission_ ? admission_->load(std::memory_order_acquire) : !parents_.empty();
    }

    bool DecisionNodeGeneration::HasImplementation(DecisionNodeTypeId type) const noexcept {
        if (!CanUse())
            return false;
        const auto found = std::ranges::lower_bound(contracts_, type, {}, [](const auto &contract) {
            return contract.descriptor.type;
        });
        if (found == contracts_.end() || found->descriptor.type != type)
            return false;
        return factories_[static_cast<std::size_t>(found - contracts_.begin())].create != nullptr;
    }

    bool DecisionNodeGeneration::ValidProvider(const std::shared_ptr<const DecisionNodeGeneration> &provider) noexcept {
        return !provider || (!provider->provider_ && provider->parents_.empty());
    }

    Result<std::shared_ptr<const DecisionNodeGeneration>> DecisionNodeGeneration::Compose(
        std::span<const std::shared_ptr<const DecisionNodeGeneration>> generations) {
        using Output = Result<std::shared_ptr<const DecisionNodeGeneration>>;
        if (generations.empty() || generations.size() > MaximumNodes)
            return Invalid<std::shared_ptr<const DecisionNodeGeneration>>();
        try {
            auto result = std::make_shared<DecisionNodeGeneration>();
            result->owner_ = std::this_thread::get_id();
            std::vector<DecisionNodeRegistration> declarations;
            for (const auto &generation : generations) {
                if (auto appended = result->AppendGeneration(generation, declarations); appended.HasError())
                    return Output::Failure(appended.ErrorValue());
            }
            if (auto stored = result->StoreDeclarations(std::move(declarations)); stored.HasError())
                return Output::Failure(stored.ErrorValue());
            return Output::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    Result<void> DecisionNodeGeneration::AppendGeneration(const std::shared_ptr<const DecisionNodeGeneration> &generation,
                                                          std::vector<DecisionNodeRegistration> &declarations) {
        if (!generation || !generation->CanAdmit())
            return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (!generation->parents_.empty())
            return Invalid<void>();
        if (generation->contracts_.size() > MaximumNodes - declarations.size())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
        for (std::size_t index = 0; index < generation->contracts_.size(); ++index)
            declarations.push_back({generation->contracts_[index], generation->factories_[index]});
        parents_.push_back(generation);
        return Result<void>::Success();
    }

    Result<void> DecisionNodeGeneration::StoreDeclarations(std::vector<DecisionNodeRegistration> declarations) {
        std::ranges::sort(declarations, {}, [](const auto &entry) {
            return entry.contract.descriptor.type;
        });
        for (const auto &entry : declarations) {
            if (!contracts_.empty() && contracts_.back().descriptor.type == entry.contract.descriptor.type)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorAmbiguous));
            contracts_.push_back(entry.contract);
            factories_.push_back(entry.factory);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::AI
