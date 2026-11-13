#include "Horo/AI/PerceptionSenseProvider.h"

#include <algorithm>
#include <new>

namespace Horo::AI {
    namespace {
        constexpr PerceptionCapabilitySet AllCapabilities{(1U << static_cast<unsigned>(PerceptionCapability::Count)) - 1U};

        /** @brief Reports exhausted registry storage without allocating another diagnostic in the failure handler. */
        Error StorageFailure() noexcept {
            const auto &descriptor = AIErrors::PerceptionRegistryStorageUnavailable;
            return {.code = descriptor.code, .domain = descriptor.domain, .severity = descriptor.defaultSeverity};
        }

        /** @brief Native and package metadata retain actual native-owner admission; raw script callbacks are not admitted. */
        bool NativeOrigin(const PerceptionDescriptorSourceKind kind) noexcept {
            return kind == PerceptionDescriptorSourceKind::Native || kind == PerceptionDescriptorSourceKind::Package;
        }

        /** @brief Keeps diagnostic source metadata bounded and free of embedded terminators. */
        bool ValidSource(const SourceLocation &source) noexcept {
            return source.source.size() <= PerceptionSenseGeneration::MaximumSourceBytes && source.source.find('\0') == std::string::npos &&
                   (source.source.empty() || (source.line > 0 && source.column > 0));
        }

        /** @brief Rejects reserved event identities and executable contracts outside the native bounded spatial tier. */
        bool CustomDescriptors(const PerceptionSenseContract &contract, const bool vmAuthority) noexcept {
            return contract.sense.identity.Value() > SenseTypeIds::Team.Value() &&
                   contract.stimulus.identity.Value() > StimulusTypeIds::Team.Value() &&
                   (vmAuthority ? contract.sense.origin.kind == PerceptionDescriptorSourceKind::Script
                                : NativeOrigin(contract.sense.origin.kind)) &&
                   contract.stimulus.origin.kind == contract.sense.origin.kind &&
                   contract.sense.origin.provider == contract.stimulus.origin.provider && contract.stimulus.payloadVersion == 1;
        }

        /** @brief Validates declared private storage, bounded work and the matching module-owned factory pair. */
        bool ValidResources(const PerceptionSenseRegistration &registration) noexcept {
            const auto &contract = registration.contract;
            return contract.instanceBytes >= sizeof(IPerceptionSenseProvider) &&
                   contract.instanceBytes <= PerceptionSenseGeneration::MaximumInstanceBytes && contract.workUnits > 0 &&
                   contract.workUnits <= PerceptionSenseGeneration::MaximumWorkUnits && registration.factory.create &&
                   registration.factory.destroy && contract.maximumCandidates > 0 &&
                   contract.maximumCandidates <= PerceptionSenseResult::MaximumCandidates && ValidSource(contract.source);
        }

        /** @brief Checks exact agent/listener representation before looking up its immutable metadata. */
        bool ValidBinding(const PerceptionSenseBinding &binding) noexcept {
            return binding.agent.IsValid() && binding.listener.IsValid() && binding.listenerType.IsValid();
        }

        /** @brief Checks the selected listener's declared stimulus version without granting any event authority. */
        bool AcceptsStimulus(const PerceptionListenerDescriptor &listener, const StimulusTypeDescriptor &stimulus) noexcept {
            for (const auto &required : std::span{listener.stimuli}.first(listener.stimulusCount))
                if (required.identity == stimulus.identity && required.minimumPayloadVersion <= stimulus.payloadVersion &&
                    required.maximumPayloadVersion >= stimulus.payloadVersion)
                    return true;
            return false;
        }

        /** @brief Compares execution metadata while keeping presentation names outside authority checks. */
        bool MatchesDescriptors(const PerceptionSenseContract &contract, const SenseTypeDescriptor &sense,
                                const StimulusTypeDescriptor &stimulus) noexcept {
            return contract.sense.identity == sense.identity && contract.sense.origin == sense.origin &&
                   contract.sense.requiredCapabilities == sense.requiredCapabilities && contract.stimulus.identity == stimulus.identity &&
                   contract.stimulus.origin == stimulus.origin && contract.stimulus.payloadVersion == stimulus.payloadVersion &&
                   contract.stimulus.requiredCapabilities == stimulus.requiredCapabilities;
        }

        /** @brief Checks one available listener against the actual selected sense and payload. */
        bool MatchesListener(const PerceptionListenerResolution &listener, const PerceptionSenseContract &contract,
                             const SenseTypeDescriptor &sense, const StimulusTypeDescriptor &stimulus) noexcept {
            return listener.availability == PerceptionListenerAvailability::Available &&
                   listener.descriptor.sense == contract.sense.identity && MatchesDescriptors(contract, sense, stimulus) &&
                   AcceptsStimulus(listener.descriptor, contract.stimulus);
        }

        /** @brief Resolves the exact scene metadata before any provider factory is called. */
        Result<void> AdmitListener(const PerceptionSenseContract &contract, const PerceptionDescriptorRegistry &registry,
                                   const PerceptionCapabilitySet available, const PerceptionSenseBinding &binding) {
            if (!ValidBinding(binding) || (available.bits & ~AllCapabilities.bits) != 0)
                return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorInvalid));
            auto listener = registry.FindListener(binding.listenerType);
            auto sense = registry.FindSense(contract.sense.identity);
            auto stimulus = registry.FindStimulus(contract.stimulus.identity);
            if (listener.HasError() || sense.HasError() || stimulus.HasError())
                return Result<void>::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
            const auto &resolved = listener.Value();
            if (resolved.availability == PerceptionListenerAvailability::CapabilityUnavailable)
                return Result<void>::Failure(MakeError(AIErrors::PerceptionCapabilityUnavailable));
            if (!MatchesListener(resolved, contract, sense.Value(), stimulus.Value()))
                return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorIncompatible));
            const auto required = contract.sense.requiredCapabilities.Union(contract.stimulus.requiredCapabilities)
                                      .Union(resolved.descriptor.requiredCapabilities);
            if (!available.Contains(required))
                return Result<void>::Failure(MakeError(AIErrors::PerceptionCapabilityUnavailable));
            return Result<void>::Success();
        }

        /** @brief Validates declarations under either native ownership or the private enforced VM boundary. */
        Result<void> ValidateRegistration(const PerceptionSenseRegistration &registration, const bool vmAuthority) {
            if (!CustomDescriptors(registration.contract, vmAuthority) || !ValidResources(registration))
                return Result<void>::Failure(MakeError(AIErrors::PerceptionDescriptorInvalid));
            const auto &contract = registration.contract;
            try {
                auto registry = PerceptionDescriptorRegistry::Capture({std::span{&contract.sense, 1}, std::span{&contract.stimulus, 1}, {}},
                                                                      AllCapabilities);
                return registry.HasError() ? Result<void>::Failure(registry.ErrorValue()) : Result<void>::Success();
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(StorageFailure());
            }
        }
    }  // namespace

    /** @copydoc ValidatePerceptionSenseRegistration */
    Result<void> ValidatePerceptionSenseRegistration(const PerceptionSenseRegistration &registration) {
        return ValidateRegistration(registration, false);
    }

    /** @copydoc PerceptionSenseGeneration::Capture */
    Result<std::shared_ptr<const PerceptionSenseGeneration>> PerceptionSenseGeneration::Capture(
        const std::span<const PerceptionSenseRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission) {
        return CaptureAdmitted(registrations, std::move(admission), {});
    }

    /** @copydoc PerceptionSenseGeneration::CaptureVm */
    Result<std::shared_ptr<const PerceptionSenseGeneration>> PerceptionSenseGeneration::CaptureVm(
        const std::span<const PerceptionSenseRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
        std::shared_ptr<const PerceptionSenseGeneration> parent) {
        if (!parent || parent->parent_ || !parent->CanAdmit())
            return Result<std::shared_ptr<const PerceptionSenseGeneration>>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        return CaptureAdmitted(registrations, std::move(admission), std::move(parent));
    }

    /** @copydoc PerceptionSenseGeneration::CaptureAdmitted */
    Result<std::shared_ptr<const PerceptionSenseGeneration>> PerceptionSenseGeneration::CaptureAdmitted(
        const std::span<const PerceptionSenseRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
        std::shared_ptr<const PerceptionSenseGeneration> parent) {
        using Output = Result<std::shared_ptr<const PerceptionSenseGeneration>>;
        if (!admission || !admission->load(std::memory_order_acquire))
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        if (registrations.size() > MaximumSenses)
            return Output::Failure(MakeError(AIErrors::PerceptionDescriptorLimitExceeded));
        try {
            auto generation = std::shared_ptr<PerceptionSenseGeneration>{new PerceptionSenseGeneration};
            generation->owner_ = std::this_thread::get_id();
            generation->admission_ = std::move(admission);
            generation->parent_ = std::move(parent);
            generation->contracts_.reserve(registrations.size());
            generation->factories_.reserve(registrations.size());
            std::array<std::size_t, MaximumSenses> order{};
            for (std::size_t index = 0; index < registrations.size(); ++index) {
                if (auto valid = ValidateRegistration(registrations[index], generation->parent_ != nullptr); valid.HasError())
                    return Output::Failure(valid.ErrorValue());
                order[index] = index;
            }
            std::ranges::sort(std::span{order}.first(registrations.size()), {}, [&](const std::size_t index) {
                return registrations[index].contract.sense.identity;
            });
            for (const auto index : std::span{order}.first(registrations.size())) {
                const auto &contract = registrations[index].contract;
                if (std::ranges::any_of(generation->contracts_, [&](const auto &previous) {
                    return previous.sense.identity == contract.sense.identity || previous.stimulus.identity == contract.stimulus.identity;
                }))
                    return Output::Failure(MakeError(AIErrors::PerceptionDescriptorConflict));
                generation->contracts_.push_back(contract);
                generation->factories_.push_back(registrations[index].factory);
            }
            return Output::Success(std::move(generation));
        } catch (const std::bad_alloc &) {
            return Output::Failure(StorageFailure());
        }
    }

    /** @copydoc PerceptionSenseGeneration::Contracts */
    std::span<const PerceptionSenseContract> PerceptionSenseGeneration::Contracts() const noexcept {
        return contracts_;
    }

    /** @copydoc PerceptionSenseGeneration::CanAdmit */
    bool PerceptionSenseGeneration::CanAdmit() const noexcept {
        return std::this_thread::get_id() == owner_ && admission_ && admission_->load(std::memory_order_acquire) &&
               (!parent_ || parent_->CanAdmit());
    }

    /** @copydoc PerceptionSenseGeneration::Create */
    Result<std::unique_ptr<PerceptionSenseInstance>> PerceptionSenseGeneration::Create(
        std::shared_ptr<const PerceptionSenseGeneration> self, const SenseTypeId sense, const PerceptionDescriptorRegistry &registry,
        const PerceptionCapabilitySet capabilities, PerceptionSenseBinding binding) const {
        using Output = Result<std::unique_ptr<PerceptionSenseInstance>>;
        if (self.get() != this || !CanAdmit() || binding.cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(AIErrors::RuntimeUnavailable));
        const auto found = std::ranges::find(contracts_, sense, [](const auto &contract) {
            return contract.sense.identity;
        });
        if (found == contracts_.end())
            return Output::Failure(MakeError(AIErrors::PerceptionDependencyMissing));
        try {
            if (auto admitted = AdmitListener(*found, registry, capabilities, binding); admitted.HasError())
                return Output::Failure(admitted.ErrorValue());
            const auto index = static_cast<std::size_t>(found - contracts_.begin());
            auto instance =
                std::unique_ptr<PerceptionSenseInstance>{new PerceptionSenseInstance{std::move(self), index, std::move(binding)}};
            if (auto initialized = instance->Initialize(); initialized.HasError())
                return Output::Failure(initialized.ErrorValue());
            return Output::Success(std::move(instance));
        } catch (const std::bad_alloc &) {
            return Output::Failure(StorageFailure());
        }
    }
}  // namespace Horo::AI
