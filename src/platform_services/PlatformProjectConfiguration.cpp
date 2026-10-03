#include "Horo/PlatformServices/PlatformProjectConfiguration.h"

#include "PlatformDefinitionRegistryDetail.h"

#include <algorithm>
#include <format>
#include <memory>

namespace Horo::PlatformServices {
    namespace {
        using DefinitionRegistryDetail::DocumentError;
        using DefinitionRegistryDetail::FingerprintWriter;
        using DefinitionRegistryDetail::MakeDescriptor;

        constexpr std::uint32_t HardMaximumContributions = 1024;
        constexpr std::uint32_t HardMaximumTrustedModules = 4096;
        constexpr std::uint32_t HardMaximumIdentityBytes = 1024;
        constexpr std::size_t ServiceCount = static_cast<std::size_t>(PlatformServiceKind::Count);
        constexpr std::string_view FingerprintDomain = "horo.platform-services.project-configuration.v1";

        [[nodiscard]] constexpr bool IsKnown(const PlatformServicesHostProfile value) noexcept {
            return value <= PlatformServicesHostProfile::Certification;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceRequirement value) noexcept {
            return value <= PlatformServiceRequirement::Required;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformProviderSelectionMode value) noexcept {
            return value <= PlatformProviderSelectionMode::ExactProvider;
        }

        [[nodiscard]] constexpr PlatformServicesHostProfileMask MaskFor(const PlatformServicesHostProfile profile) noexcept {
            return static_cast<PlatformServicesHostProfileMask>(1U << static_cast<std::uint8_t>(profile));
        }

        [[nodiscard]] constexpr bool Contains(const PlatformServicesHostProfileMask mask,
                                              const PlatformServicesHostProfile profile) noexcept {
            const auto maskBits = std::byte{static_cast<std::uint8_t>(mask)};
            const auto profileBits = std::byte{static_cast<std::uint8_t>(MaskFor(profile))};
            return (maskBits & profileBits) != std::byte{};
        }

        [[nodiscard]] constexpr bool IsIdentityCharacter(const char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '-';
        }

        [[nodiscard]] bool IsCanonicalIdentity(const std::string_view value, const std::uint32_t maximumBytes) noexcept {
            return !value.empty() && value.size() <= maximumBytes && value.front() >= 'a' && value.front() <= 'z' &&
                   std::ranges::all_of(value.substr(1), IsIdentityCharacter) && value.find('.') != std::string_view::npos;
        }

        [[nodiscard]] bool HasValidLimits(const PlatformProjectConfigurationLimits &limits) noexcept {
            return limits.maximumContributions > 0 && limits.maximumContributions <= HardMaximumContributions &&
                   limits.maximumTrustedModules > 0 && limits.maximumTrustedModules <= HardMaximumTrustedModules &&
                   limits.maximumProjectIdBytes > 0 && limits.maximumProjectIdBytes <= HardMaximumIdentityBytes &&
                   limits.maximumProviderKeyBytes > 0 && limits.maximumProviderKeyBytes <= HardMaximumIdentityBytes &&
                   limits.maximumModuleIdBytes > 0 && limits.maximumModuleIdBytes <= HardMaximumIdentityBytes;
        }

        [[nodiscard]] Error FieldError(const ErrorCodeDescriptor &descriptor, const std::string_view field,
                                       const std::string_view message) {
            return DocumentError(descriptor, field, message);
        }

        [[nodiscard]] bool HasRecognizedServiceRequirements(const std::array<PlatformServiceRequirement, ServiceCount> &services) noexcept {
            return std::ranges::all_of(services, [](const PlatformServiceRequirement requirement) {
                return IsKnown(requirement);
            });
        }

        [[nodiscard]] bool HasValidProviderSelection(const PlatformProviderSelection &selection,
                                                     const PlatformProjectConfigurationLimits &limits) noexcept {
            if (selection.mode == PlatformProviderSelectionMode::Null)
                return selection.providerKey.empty();
            return IsCanonicalIdentity(selection.providerKey, limits.maximumProviderKeyBytes);
        }

        [[nodiscard]] Result<void> ValidateCandidateShape(const PlatformProjectConfigurationCandidate &candidate,
                                                          const PlatformProjectConfigurationLimits &limits) {
            if (candidate.schemaVersion != PlatformProjectConfigurationSchemaVersion)
                return Result<void>::Failure(FieldError(PlatformProjectConfigurationErrors::UnsupportedVersion, "schemaVersion",
                                                        "Only Platform Services project configuration schema version 1 is supported."));
            if (candidate.projectId.empty() || candidate.projectId.size() > limits.maximumProjectIdBytes)
                return Result<void>::Failure(FieldError(PlatformProjectConfigurationErrors::InvalidConfiguration, "projectId",
                                                        "Project identity must match the bounded nonempty root project identity."));
            if (!IsKnown(candidate.profile) || !IsKnown(candidate.provider.mode))
                return Result<void>::Failure(FieldError(PlatformProjectConfigurationErrors::InvalidConfiguration, "profile",
                                                        "Host profile and provider selection mode must be recognized."));
            if (!HasRecognizedServiceRequirements(candidate.services))
                return Result<void>::Failure(FieldError(PlatformProjectConfigurationErrors::InvalidConfiguration, "services",
                                                        "Every service policy must be disabled, optional, or required."));
            if (!HasValidProviderSelection(candidate.provider, limits))
                return Result<void>::Failure(FieldError(PlatformProjectConfigurationErrors::InvalidConfiguration, "provider",
                                                        "Null uses no key; exact selection requires one canonical provider key."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<std::string_view>> ValidateTrustedModules(std::span<const ModuleId> trustedModules,
                                                                                   const PlatformProjectConfigurationLimits &limits) {
            std::vector<std::string_view> trusted;
            trusted.reserve(trustedModules.size());
            for (const auto &moduleId : trustedModules) {
                if (!IsCanonicalIdentity(moduleId.value, limits.maximumModuleIdBytes))
                    return Result<std::vector<std::string_view>>::Failure(
                        FieldError(PlatformProjectConfigurationErrors::InvalidContribution, "trustedModules",
                                   "Trusted module identities must be canonical and bounded."));
                trusted.emplace_back(moduleId.value);
            }
            std::ranges::sort(trusted);
            if (std::ranges::adjacent_find(trusted) != trusted.end())
                return Result<std::vector<std::string_view>>::Failure(FieldError(PlatformProjectConfigurationErrors::DuplicateContribution,
                                                                                 "trustedModules",
                                                                                 "A trusted module identity appears more than once."));
            return Result<std::vector<std::string_view>>::Success(std::move(trusted));
        }

        [[nodiscard]] bool HasValidContributionShape(const PlatformProviderModuleContribution &contribution,
                                                     const PlatformProjectConfigurationLimits &limits) noexcept {
            const auto mask = std::byte{static_cast<std::uint8_t>(contribution.allowedProfiles)};
            const bool validProfileMask = mask != std::byte{} && (mask & std::byte{0xf0U}) == std::byte{};
            return IsCanonicalIdentity(contribution.module.value, limits.maximumModuleIdBytes) &&
                   IsCanonicalIdentity(contribution.providerKey, limits.maximumProviderKeyBytes) && contribution.provider.IsValid() &&
                   contribution.interfaceVersion == PlatformServicesBackendInterfaceVersion{PlatformServicesBackendInterfaceMajor,
                                                                                            PlatformServicesBackendInterfaceMinor} &&
                   validProfileMask;
        }

        [[nodiscard]] bool HasContributionConflicts(const std::vector<PlatformProviderModuleContribution> &contributions) {
            std::vector<std::string_view> providerKeys;
            std::vector<std::string_view> moduleIds;
            std::vector<std::uint64_t> providerIds;
            providerKeys.reserve(contributions.size());
            moduleIds.reserve(contributions.size());
            providerIds.reserve(contributions.size());
            for (const auto &contribution : contributions) {
                providerKeys.emplace_back(contribution.providerKey);
                moduleIds.emplace_back(contribution.module.value);
                providerIds.emplace_back(contribution.provider.value);
            }
            std::ranges::sort(providerKeys);
            std::ranges::sort(moduleIds);
            std::ranges::sort(providerIds);
            return std::ranges::adjacent_find(providerKeys) != providerKeys.end() ||
                   std::ranges::adjacent_find(moduleIds) != moduleIds.end() || std::ranges::adjacent_find(providerIds) != providerIds.end();
        }

        [[nodiscard]] Result<std::vector<PlatformProviderModuleContribution>> ValidateContributions(
            std::span<const PlatformProviderModuleContribution> contributions, std::span<const ModuleId> trustedModules,
            const PlatformProjectConfigurationLimits &limits) {
            if (!HasValidLimits(limits) || contributions.size() > limits.maximumContributions ||
                trustedModules.size() > limits.maximumTrustedModules)
                return Result<std::vector<PlatformProviderModuleContribution>>::Failure(
                    FieldError(PlatformProjectConfigurationErrors::CapacityExceeded, "limits",
                               "Configuration or contribution input exceeds the finite composition bounds."));

            auto validatedTrusted = ValidateTrustedModules(trustedModules, limits);
            if (validatedTrusted.HasError())
                return Result<std::vector<PlatformProviderModuleContribution>>::Failure(validatedTrusted.ErrorValue());
            const auto trusted = std::move(validatedTrusted).Value();

            std::vector<PlatformProviderModuleContribution> sorted{contributions.begin(), contributions.end()};
            for (std::size_t index = 0; index < sorted.size(); ++index) {
                const auto &contribution = sorted[index];
                const auto field = std::format("contributions[{}]", index);
                if (!HasValidContributionShape(contribution, limits))
                    return Result<std::vector<PlatformProviderModuleContribution>>::Failure(
                        FieldError(PlatformProjectConfigurationErrors::InvalidContribution, field,
                                   "Contribution identity, interface version, or profile eligibility is invalid."));
                if (!std::ranges::binary_search(trusted, std::string_view{contribution.module.value}))
                    return Result<std::vector<PlatformProviderModuleContribution>>::Failure(
                        FieldError(PlatformProjectConfigurationErrors::UntrustedContribution, field + ".module",
                                   "The contribution is not owned by an explicitly trusted selected module."));
            }

            std::ranges::sort(sorted, [](const auto &left, const auto &right) {
                if (left.providerKey != right.providerKey)
                    return left.providerKey < right.providerKey;
                return left.module.value < right.module.value;
            });
            if (HasContributionConflicts(sorted))
                return Result<std::vector<PlatformProviderModuleContribution>>::Failure(
                    FieldError(PlatformProjectConfigurationErrors::DuplicateContribution, "contributions",
                               "Provider keys, Horo provider identities, and contributing module identities must be unique."));
            return Result<std::vector<PlatformProviderModuleContribution>>::Success(std::move(sorted));
        }

        [[nodiscard]] Sha256Digest ComputeFingerprint(const PlatformProjectConfigurationCandidate &candidate,
                                                      const std::vector<PlatformProviderModuleContribution> &contributions) {
            FingerprintWriter writer{128U + contributions.size() * 96U};
            writer.AddText(FingerprintDomain);
            writer.AddU32(candidate.schemaVersion);
            writer.AddText(candidate.projectId);
            writer.AddByte(static_cast<std::uint8_t>(candidate.profile));
            writer.AddByte(static_cast<std::uint8_t>(candidate.provider.mode));
            writer.AddText(candidate.provider.providerKey);
            for (const auto requirement : candidate.services)
                writer.AddByte(static_cast<std::uint8_t>(requirement));
            writer.AddU32(static_cast<std::uint32_t>(contributions.size()));
            for (const auto &contribution : contributions) {
                writer.AddText(contribution.module.value);
                writer.AddText(contribution.providerKey);
                writer.AddU64(contribution.provider.value);
                writer.AddU32((static_cast<std::uint32_t>(contribution.interfaceVersion.major) << 16U) |
                              contribution.interfaceVersion.minor);
                writer.AddByte(static_cast<std::uint8_t>(contribution.allowedProfiles));
                for (const bool supported : contribution.supportedServices)
                    writer.AddByte(supported ? 1U : 0U);
            }
            return writer.Finish();
        }

        struct ValidatedPolicyData final {
            bool usesNullProvider{true};
            std::optional<PlatformProviderId> selectedProvider;
            std::string selectedProviderKey;
            std::optional<ModuleId> selectedModule;
            Sha256Digest fingerprint{};
        };

        [[nodiscard]] Result<const PlatformProviderModuleContribution *> ResolveSelectedContribution(
            const PlatformProjectConfigurationCandidate &candidate, const std::vector<PlatformProviderModuleContribution> &contributions) {
            if (candidate.provider.mode == PlatformProviderSelectionMode::Null)
                return Result<const PlatformProviderModuleContribution *>::Success(nullptr);

            const auto found = std::ranges::lower_bound(contributions, candidate.provider.providerKey, {},
                                                        &PlatformProviderModuleContribution::providerKey);
            if (found == contributions.end() || found->providerKey != candidate.provider.providerKey)
                return Result<const PlatformProviderModuleContribution *>::Failure(
                    FieldError(PlatformProjectConfigurationErrors::ProviderNotFound, "provider.providerKey",
                               "The exact selected provider has no trusted contribution; fallback is forbidden."));
            if (!Contains(found->allowedProfiles, candidate.profile))
                return Result<const PlatformProviderModuleContribution *>::Failure(
                    FieldError(PlatformProjectConfigurationErrors::ProfileDenied, "profile",
                               "The selected provider is not eligible for this exact host/product profile."));
            return Result<const PlatformProviderModuleContribution *>::Success(std::to_address(found));
        }

        [[nodiscard]] Result<void> ValidateRequiredServices(const std::array<PlatformServiceRequirement, ServiceCount> &requirements,
                                                            const PlatformProviderModuleContribution *selected) {
            for (std::size_t index = 0; index < requirements.size(); ++index) {
                if (requirements[index] != PlatformServiceRequirement::Required)
                    continue;
                if (selected == nullptr || !selected->supportedServices[index])
                    return Result<void>::Failure(FieldError(PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported,
                                                            std::format("services[{}]", index),
                                                            "The exact selected provider does not declare this required capability."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ValidatedPolicyData> ValidatePolicy(const PlatformProjectConfigurationCandidate &candidate,
                                                                 std::span<const PlatformProviderModuleContribution> contributions,
                                                                 std::span<const ModuleId> trustedModules,
                                                                 const PlatformProjectConfigurationLimits &limits) {
            if (!HasValidLimits(limits))
                return Result<ValidatedPolicyData>::Failure(
                    FieldError(PlatformProjectConfigurationErrors::CapacityExceeded, "limits",
                               "Configured validation limits must be nonzero and within hard engine bounds."));
            if (const auto valid = ValidateCandidateShape(candidate, limits); valid.HasError())
                return Result<ValidatedPolicyData>::Failure(valid.ErrorValue());
            auto validatedContributions = ValidateContributions(contributions, trustedModules, limits);
            if (validatedContributions.HasError())
                return Result<ValidatedPolicyData>::Failure(validatedContributions.ErrorValue());
            auto sorted = std::move(validatedContributions).Value();

            auto resolved = ResolveSelectedContribution(candidate, sorted);
            if (resolved.HasError())
                return Result<ValidatedPolicyData>::Failure(resolved.ErrorValue());
            const auto *selected = std::move(resolved).Value();
            if (const auto required = ValidateRequiredServices(candidate.services, selected); required.HasError())
                return Result<ValidatedPolicyData>::Failure(required.ErrorValue());

            ValidatedPolicyData result;
            result.usesNullProvider = selected == nullptr;
            if (selected != nullptr) {
                result.selectedProvider = selected->provider;
                result.selectedProviderKey = selected->providerKey;
                result.selectedModule = selected->module;
            }
            result.fingerprint = ComputeFingerprint(candidate, sorted);
            return Result<ValidatedPolicyData>::Success(std::move(result));
        }
    }  // namespace

    namespace PlatformProjectConfigurationErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.project-configuration"};
        }

        const ErrorCodeDescriptor UnsupportedVersion =
            MakeDescriptor(Domain, "platform.configuration.version_unsupported", "Platform project configuration version is unsupported.",
                           "Migrate the project configuration through an explicit supported migration.", true);
        const ErrorCodeDescriptor CapacityExceeded =
            MakeDescriptor(Domain, "platform.configuration.capacity_exceeded", "Platform project configuration exceeds finite bounds.",
                           "Reduce contribution or identity input sizes.", true);
        const ErrorCodeDescriptor InvalidConfiguration =
            MakeDescriptor(Domain, "platform.configuration.invalid", "Platform project configuration is malformed.",
                           "Correct the diagnosed typed project field.", true);
        const ErrorCodeDescriptor InvalidContribution =
            MakeDescriptor(Domain, "platform.configuration.contribution_invalid", "A provider module contribution is malformed.",
                           "Correct the trusted inert module manifest contribution.", true);
        const ErrorCodeDescriptor DuplicateContribution =
            MakeDescriptor(Domain, "platform.configuration.contribution_duplicate", "Provider module contributions conflict.",
                           "Keep provider keys, identities, and module ownership unique.", true);
        const ErrorCodeDescriptor UntrustedContribution =
            MakeDescriptor(Domain, "platform.configuration.contribution_untrusted", "A provider contribution is not trusted.",
                           "Select and trust the exact owning package/module before composition.", true);
        const ErrorCodeDescriptor ProviderNotFound =
            MakeDescriptor(Domain, "platform.configuration.provider_not_found", "The exact selected provider is unavailable.",
                           "Install and trust that provider or explicitly select Null; no fallback is attempted.", true);
        const ErrorCodeDescriptor ProfileDenied =
            MakeDescriptor(Domain, "platform.configuration.profile_denied", "The selected provider is forbidden by the host profile.",
                           "Choose a provider explicitly qualified for this product profile.", true);
        const ErrorCodeDescriptor RequiredCapabilityUnsupported =
            MakeDescriptor(Domain, "platform.configuration.required_capability_unsupported",
                           "The selected provider does not declare a required capability.",
                           "Select an exact compatible provider or revise the explicit requirement.", true);
        const ErrorCodeDescriptor StaleReplacement =
            MakeDescriptor(Domain, "platform.configuration.replacement_stale", "The replacement targets another configuration revision.",
                           "Reload the active fingerprint and review the replacement again.", true);
    }  // namespace PlatformProjectConfigurationErrors

    /** @copydoc PlatformProjectConfiguration::ProjectId */
    std::string_view PlatformProjectConfiguration::ProjectId() const noexcept {
        return projectId_;
    }

    /** @copydoc PlatformProjectConfiguration::Profile */
    PlatformServicesHostProfile PlatformProjectConfiguration::Profile() const noexcept {
        return profile_;
    }

    /** @copydoc PlatformProjectConfiguration::UsesNullProvider */
    bool PlatformProjectConfiguration::UsesNullProvider() const noexcept {
        return usesNullProvider_;
    }

    /** @copydoc PlatformProjectConfiguration::SelectedProvider */
    std::optional<PlatformProviderId> PlatformProjectConfiguration::SelectedProvider() const noexcept {
        return selectedProvider_;
    }

    /** @copydoc PlatformProjectConfiguration::SelectedProviderKey */
    std::string_view PlatformProjectConfiguration::SelectedProviderKey() const noexcept {
        return selectedProviderKey_;
    }

    /** @copydoc PlatformProjectConfiguration::SelectedModule */
    const std::optional<ModuleId> &PlatformProjectConfiguration::SelectedModule() const noexcept {
        return selectedModule_;
    }

    /** @copydoc PlatformProjectConfiguration::ServiceRequirements */
    std::span<const PlatformServiceRequirement> PlatformProjectConfiguration::ServiceRequirements() const noexcept {
        return services_;
    }

    /** @copydoc PlatformProjectConfiguration::SelectedServices */
    std::span<const bool> PlatformProjectConfiguration::SelectedServices() const noexcept {
        return selectedServices_;
    }

    /** @copydoc PlatformProjectConfiguration::BackendConfig */
    PlatformServicesBackendConfig PlatformProjectConfiguration::BackendConfig() const noexcept {
        PlatformServicesBackendConfig config;
        for (std::size_t index = 0; index < services_.size(); ++index)
            config.requiredServices[index] = services_[index] == PlatformServiceRequirement::Required;
        return config;
    }

    /** @copydoc PlatformProjectConfiguration::Fingerprint */
    const Sha256Digest &PlatformProjectConfiguration::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc BuildPlatformProjectConfiguration */
    Result<PlatformProjectConfiguration> BuildPlatformProjectConfiguration(
        const PlatformProjectConfigurationCandidate &candidate, const std::span<const PlatformProviderModuleContribution> contributions,
        const std::span<const ModuleId> trustedModules, const PlatformProjectConfigurationLimits &limits) {
        if (candidate.baseFingerprint)
            return Result<PlatformProjectConfiguration>::Failure(
                FieldError(PlatformProjectConfigurationErrors::StaleReplacement, "baseFingerprint",
                           "Initial construction cannot carry replacement revision evidence."));
        auto validated = ValidatePolicy(candidate, contributions, trustedModules, limits);
        if (validated.HasError())
            return Result<PlatformProjectConfiguration>::Failure(validated.ErrorValue());
        auto data = std::move(validated).Value();
        PlatformProjectConfiguration result;
        result.projectId_ = candidate.projectId;
        result.profile_ = candidate.profile;
        result.usesNullProvider_ = data.usesNullProvider;
        result.selectedProvider_ = data.selectedProvider;
        result.selectedProviderKey_ = std::move(data.selectedProviderKey);
        result.selectedModule_ = std::move(data.selectedModule);
        result.services_ = candidate.services;
        for (const auto &contribution : contributions)
            if (result.selectedProvider_ == contribution.provider)
                result.selectedServices_ = contribution.supportedServices;
        result.fingerprint_ = data.fingerprint;
        return Result<PlatformProjectConfiguration>::Success(std::move(result));
    }

    /** @copydoc BuildPlatformProjectConfigurationReplacement */
    Result<PlatformProjectConfiguration> BuildPlatformProjectConfigurationReplacement(
        const PlatformProjectConfiguration &previous, const PlatformProjectConfigurationCandidate &candidate,
        const std::span<const PlatformProviderModuleContribution> contributions, const std::span<const ModuleId> trustedModules,
        const PlatformProjectConfigurationLimits &limits) {
        if (!candidate.baseFingerprint || *candidate.baseFingerprint != previous.Fingerprint() ||
            candidate.projectId != previous.ProjectId())
            return Result<PlatformProjectConfiguration>::Failure(
                FieldError(PlatformProjectConfigurationErrors::StaleReplacement, "baseFingerprint",
                           "Replacement must target the exact active fingerprint and owning project."));
        PlatformProjectConfigurationCandidate detached = candidate;
        detached.baseFingerprint.reset();
        return BuildPlatformProjectConfiguration(detached, contributions, trustedModules, limits);
    }
}  // namespace Horo::PlatformServices
