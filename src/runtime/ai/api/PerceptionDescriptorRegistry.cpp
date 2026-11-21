#include "Horo/AI/PerceptionDescriptorRegistry.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace Horo::AI {
    struct PerceptionDescriptorRegistry::Storage final {
        Storage() : senses(std::size_t{0}), stimuli(std::size_t{0}), listeners(std::size_t{0}) {}

        std::vector<SenseTypeDescriptor> senses;
        std::vector<StimulusTypeDescriptor> stimuli;
        std::vector<PerceptionListenerResolution> listeners;
    };

    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        constexpr std::uint32_t KnownCapabilities = (std::uint32_t{1} << static_cast<std::uint8_t>(PerceptionCapability::Count)) - 1U;

        [[nodiscard]] bool ValidCapabilities(const PerceptionCapabilitySet capabilities) noexcept {
            return (capabilities.bits & ~KnownCapabilities) == 0U;
        }

        [[nodiscard]] bool ValidOrigin(const PerceptionDescriptorOrigin &origin) noexcept {
            return origin.kind < PerceptionDescriptorSourceKind::Count && origin.provider.IsValid() && origin.version != 0;
        }

        [[nodiscard]] bool ValidName(const std::string_view name, const PerceptionDescriptorRegistryLimits &limits) noexcept {
            return !name.empty() && name.size() <= limits.maximumDisplayNameBytes && name.find('\0') == std::string::npos;
        }

        [[nodiscard]] bool ValidLimits(const PerceptionDescriptorRegistryLimits &limits) noexcept {
            return limits.maximumSenseTypes > 0 && limits.maximumSenseTypes <= PerceptionDescriptorRegistryHardLimits::SenseTypes &&
                   limits.maximumStimulusTypes > 0 &&
                   limits.maximumStimulusTypes <= PerceptionDescriptorRegistryHardLimits::StimulusTypes &&
                   limits.maximumListenerTypes > 0 &&
                   limits.maximumListenerTypes <= PerceptionDescriptorRegistryHardLimits::ListenerTypes &&
                   limits.maximumDisplayNameBytes > 0 &&
                   limits.maximumDisplayNameBytes <= PerceptionDescriptorRegistryHardLimits::DisplayNameBytes;
        }

        template <typename Descriptor, typename Identity>
        [[nodiscard]] auto FindDescriptor(const std::span<const Descriptor> descriptors, const Identity identity) noexcept {
            return std::ranges::lower_bound(descriptors, identity, {}, [](const Descriptor &descriptor) {
                return descriptor.identity;
            });
        }

        [[nodiscard]] bool ValidSense(const SenseTypeDescriptor &descriptor, const PerceptionDescriptorRegistryLimits &limits) noexcept {
            return descriptor.identity.IsValid() && ValidOrigin(descriptor.origin) && ValidName(descriptor.displayName, limits) &&
                   ValidCapabilities(descriptor.requiredCapabilities);
        }

        [[nodiscard]] bool ValidStimulus(const StimulusTypeDescriptor &descriptor,
                                         const PerceptionDescriptorRegistryLimits &limits) noexcept {
            return descriptor.identity.IsValid() && ValidOrigin(descriptor.origin) && ValidName(descriptor.displayName, limits) &&
                   descriptor.payloadVersion != 0 && ValidCapabilities(descriptor.requiredCapabilities);
        }

        [[nodiscard]] bool ValidListener(const PerceptionListenerDescriptor &descriptor,
                                         const PerceptionDescriptorRegistryLimits &limits) noexcept {
            if (!descriptor.identity.IsValid() || !ValidOrigin(descriptor.origin) || !ValidName(descriptor.displayName, limits) ||
                !descriptor.sense.IsValid() || descriptor.minimumSenseVersion == 0 ||
                descriptor.minimumSenseVersion > descriptor.maximumSenseVersion || descriptor.stimulusCount == 0 ||
                descriptor.stimulusCount > MaximumStimulusTypesPerListener ||
                descriptor.dependencyPolicy >= PerceptionDependencyPolicy::Count || !ValidCapabilities(descriptor.requiredCapabilities))
                return false;
            const auto active = std::span{descriptor.stimuli}.first(descriptor.stimulusCount);
            if (std::ranges::any_of(active, [](const PerceptionStimulusRequirement &requirement) {
                return !requirement.identity.IsValid() || requirement.minimumPayloadVersion == 0 ||
                       requirement.minimumPayloadVersion > requirement.maximumPayloadVersion;
            }))
                return false;
            std::array<std::uint64_t, MaximumStimulusTypesPerListener> identities{};
            std::ranges::transform(active, identities.begin(), [](const PerceptionStimulusRequirement &requirement) {
                return requirement.identity.Value();
            });
            std::ranges::sort(identities.begin(), identities.begin() + static_cast<std::ptrdiff_t>(descriptor.stimulusCount));
            return std::adjacent_find(identities.begin(), identities.begin() + static_cast<std::ptrdiff_t>(descriptor.stimulusCount)) ==
                   identities.begin() + static_cast<std::ptrdiff_t>(descriptor.stimulusCount);
        }

        [[nodiscard]] PerceptionListenerAvailability ResolveListener(const PerceptionListenerDescriptor &listener,
                                                                     const std::span<const SenseTypeDescriptor> senses,
                                                                     const std::span<const StimulusTypeDescriptor> stimuli,
                                                                     const PerceptionCapabilitySet availableCapabilities) noexcept {
            using enum PerceptionListenerAvailability;
            const auto sense = FindDescriptor(senses, listener.sense);
            if (sense == senses.end() || sense->identity != listener.sense)
                return MissingSense;
            if (sense->origin.version < listener.minimumSenseVersion || sense->origin.version > listener.maximumSenseVersion)
                return IncompatibleSense;
            PerceptionCapabilitySet required = listener.requiredCapabilities.Union(sense->requiredCapabilities);
            for (const PerceptionStimulusRequirement &requirement : std::span{listener.stimuli}.first(listener.stimulusCount)) {
                const auto stimulus = FindDescriptor(stimuli, requirement.identity);
                if (stimulus == stimuli.end() || stimulus->identity != requirement.identity)
                    return MissingStimulus;
                if (stimulus->payloadVersion < requirement.minimumPayloadVersion ||
                    stimulus->payloadVersion > requirement.maximumPayloadVersion)
                    return IncompatibleStimulus;
                required = required.Union(stimulus->requiredCapabilities);
            }
            if (!availableCapabilities.Contains(required))
                return CapabilityUnavailable;
            return Available;
        }

        /** @brief Validates bounded declarations and owns them in identity order without moving checked-library strings. */
        template <std::size_t Capacity, typename Descriptor, typename Entry>
        Result<void> CaptureDescriptors(const std::span<const Descriptor> source, const PerceptionDescriptorRegistryLimits &limits,
                                        bool (*valid)(const Descriptor &, const PerceptionDescriptorRegistryLimits &) noexcept,
                                        std::vector<Entry> &destination) {
            std::array<const Descriptor *, Capacity> ordered{};
            auto active = std::span{ordered}.first(source.size());
            for (std::size_t index = 0; index < source.size(); ++index) {
                if (!valid(source[index], limits))
                    return Failure<void>(AIErrors::PerceptionDescriptorInvalid);
                active[index] = &source[index];
            }
            const auto identity = [](const Descriptor *descriptor) {
                return descriptor->identity;
            };
            std::ranges::sort(active, {}, identity);
            if (std::ranges::adjacent_find(active, {}, identity) != active.end())
                return Failure<void>(AIErrors::PerceptionDescriptorConflict);
            destination.reserve(source.size());
            for (const auto *descriptor : active)
                destination.emplace_back(*descriptor);
            return Result<void>::Success();
        }

        /** @brief Canonicalizes and resolves captured listener descriptors against frozen type metadata. */
        [[nodiscard]] Result<void> ResolveListeners(const std::span<PerceptionListenerResolution> listeners,
                                                    const std::span<const SenseTypeDescriptor> senses,
                                                    const std::span<const StimulusTypeDescriptor> stimuli,
                                                    const PerceptionCapabilitySet availableCapabilities) {
            for (PerceptionListenerResolution &listener : listeners) {
                auto activeStimuli = std::span{listener.descriptor.stimuli}.first(listener.descriptor.stimulusCount);
                std::ranges::sort(activeStimuli, {}, [](const PerceptionStimulusRequirement &requirement) {
                    return requirement.identity;
                });
                listener.availability = ResolveListener(listener.descriptor, senses, stimuli, availableCapabilities);
                if (listener.descriptor.dependencyPolicy != PerceptionDependencyPolicy::Required)
                    continue;
                if (listener.availability == PerceptionListenerAvailability::CapabilityUnavailable)
                    return Failure<void>(AIErrors::PerceptionCapabilityUnavailable);
                if (listener.availability == PerceptionListenerAvailability::IncompatibleSense ||
                    listener.availability == PerceptionListenerAvailability::IncompatibleStimulus)
                    return Failure<void>(AIErrors::PerceptionDescriptorIncompatible);
                if (listener.availability != PerceptionListenerAvailability::Available)
                    return Failure<void>(AIErrors::PerceptionDependencyMissing);
            }
            return Result<void>::Success();
        }
    }  // namespace

    PerceptionDescriptorRegistry::PerceptionDescriptorRegistry(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    PerceptionDescriptorRegistry::~PerceptionDescriptorRegistry() = default;
    PerceptionDescriptorRegistry::PerceptionDescriptorRegistry(PerceptionDescriptorRegistry &&) noexcept = default;

    /** @copydoc PerceptionDescriptorRegistry::Capture */
    Result<PerceptionDescriptorRegistry> PerceptionDescriptorRegistry::Capture(const PerceptionDescriptorContributions &contributions,
                                                                               const PerceptionCapabilitySet availableCapabilities,
                                                                               const PerceptionDescriptorRegistryLimits &limits) {
        if (!ValidLimits(limits) || !ValidCapabilities(availableCapabilities))
            return Failure<PerceptionDescriptorRegistry>(AIErrors::PerceptionDescriptorInvalid);
        if (contributions.senses.size() > limits.maximumSenseTypes || contributions.stimuli.size() > limits.maximumStimulusTypes ||
            contributions.listeners.size() > limits.maximumListenerTypes)
            return Failure<PerceptionDescriptorRegistry>(AIErrors::PerceptionDescriptorLimitExceeded);

        using Output = Result<PerceptionDescriptorRegistry>;
        using Bounds = PerceptionDescriptorRegistryHardLimits;
        try {
            auto storage = std::make_unique<Storage>();
            if (const auto captured = CaptureDescriptors<Bounds::SenseTypes>(contributions.senses, limits, ValidSense, storage->senses);
                captured.HasError())
                return Output::Failure(captured.ErrorValue());
            if (const auto captured =
                    CaptureDescriptors<Bounds::StimulusTypes>(contributions.stimuli, limits, ValidStimulus, storage->stimuli);
                captured.HasError())
                return Output::Failure(captured.ErrorValue());
            if (const auto captured =
                    CaptureDescriptors<Bounds::ListenerTypes>(contributions.listeners, limits, ValidListener, storage->listeners);
                captured.HasError())
                return Output::Failure(captured.ErrorValue());
            if (const auto resolved = ResolveListeners(storage->listeners, storage->senses, storage->stimuli, availableCapabilities);
                resolved.HasError())
                return Output::Failure(resolved.ErrorValue());
            return Output::Success(PerceptionDescriptorRegistry{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<PerceptionDescriptorRegistry>(AIErrors::PerceptionRegistryStorageUnavailable);
        }
    }

    /** @copydoc PerceptionDescriptorRegistry::Senses */
    std::span<const SenseTypeDescriptor> PerceptionDescriptorRegistry::Senses() const noexcept {
        return storage_ == nullptr ? std::span<const SenseTypeDescriptor>{} : std::span<const SenseTypeDescriptor>{storage_->senses};
    }

    /** @copydoc PerceptionDescriptorRegistry::Stimuli */
    std::span<const StimulusTypeDescriptor> PerceptionDescriptorRegistry::Stimuli() const noexcept {
        return storage_ == nullptr ? std::span<const StimulusTypeDescriptor>{} : std::span<const StimulusTypeDescriptor>{storage_->stimuli};
    }

    /** @copydoc PerceptionDescriptorRegistry::Listeners */
    std::span<const PerceptionListenerResolution> PerceptionDescriptorRegistry::Listeners() const noexcept {
        return storage_ == nullptr ? std::span<const PerceptionListenerResolution>{}
                                   : std::span<const PerceptionListenerResolution>{storage_->listeners};
    }

    /** @copydoc PerceptionDescriptorRegistry::FindSense */
    Result<SenseTypeDescriptor> PerceptionDescriptorRegistry::FindSense(const SenseTypeId identity) const {
        if (!identity.IsValid())
            return Failure<SenseTypeDescriptor>(AIErrors::PerceptionDescriptorInvalid);
        const auto senses = Senses();
        const auto found = FindDescriptor(senses, identity);
        if (found == senses.end() || found->identity != identity)
            return Failure<SenseTypeDescriptor>(AIErrors::PerceptionDependencyMissing);
        return Result<SenseTypeDescriptor>::Success(*found);
    }

    /** @copydoc PerceptionDescriptorRegistry::FindStimulus */
    Result<StimulusTypeDescriptor> PerceptionDescriptorRegistry::FindStimulus(const StimulusTypeId identity) const {
        if (!identity.IsValid())
            return Failure<StimulusTypeDescriptor>(AIErrors::PerceptionDescriptorInvalid);
        const auto stimuli = Stimuli();
        const auto found = FindDescriptor(stimuli, identity);
        if (found == stimuli.end() || found->identity != identity)
            return Failure<StimulusTypeDescriptor>(AIErrors::PerceptionDependencyMissing);
        return Result<StimulusTypeDescriptor>::Success(*found);
    }

    /** @copydoc PerceptionDescriptorRegistry::FindListener */
    Result<PerceptionListenerResolution> PerceptionDescriptorRegistry::FindListener(const PerceptionListenerTypeId identity) const {
        if (!identity.IsValid())
            return Failure<PerceptionListenerResolution>(AIErrors::PerceptionDescriptorInvalid);
        const auto listeners = Listeners();
        const auto found = std::ranges::lower_bound(listeners, identity, {}, [](const PerceptionListenerResolution &resolution) {
            return resolution.descriptor.identity;
        });
        if (found == listeners.end() || found->descriptor.identity != identity)
            return Failure<PerceptionListenerResolution>(AIErrors::PerceptionDependencyMissing);
        return Result<PerceptionListenerResolution>::Success(*found);
    }
}  // namespace Horo::AI
