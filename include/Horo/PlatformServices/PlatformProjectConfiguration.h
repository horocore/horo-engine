#pragma once

/**
 * @file PlatformProjectConfiguration.h
 * @brief Typed project policy and inert provider-module contributions for Platform Services.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::PlatformServices {
    inline constexpr std::uint32_t PlatformProjectConfigurationSchemaVersion = 1;

    /** @brief Host/product boundary at which project Platform Services policy is consumed. */
    enum class PlatformServicesHostProfile : std::uint8_t {
        InteractiveDevelopment,
        HeadlessServer,
        Cook,
        Certification
    };

    /** @brief Profiles for which an inert provider contribution is eligible. */
    enum class PlatformServicesHostProfileMask : std::uint8_t {
        None = 0,
        InteractiveDevelopment = 1U << 0U,
        HeadlessServer = 1U << 1U,
        Cook = 1U << 2U,
        Certification = 1U << 3U
    };

    [[nodiscard]] constexpr PlatformServicesHostProfileMask operator|(const PlatformServicesHostProfileMask left,
                                                                      const PlatformServicesHostProfileMask right) noexcept {
        return static_cast<PlatformServicesHostProfileMask>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /** @brief Project policy for one backend-neutral service capability. */
    enum class PlatformServiceRequirement : std::uint8_t {
        Disabled,
        Optional,
        Required
    };

    /** @brief Exact provider selection; `Null` is explicit and never a fallback. */
    enum class PlatformProviderSelectionMode : std::uint8_t {
        Null,
        ExactProvider
    };

    /** @brief Provider-neutral project selection using a stable manifest key. */
    struct PlatformProviderSelection final {
        PlatformProviderSelectionMode mode{PlatformProviderSelectionMode::Null};
        std::string providerKey; /**< Empty for Null; exact canonical manifest key otherwise. */
    };

    /**
     * @brief Inert contribution copied from one already trusted module manifest.
     * @details Construction and validation never load a module, inspect global state, invoke lifecycle code, or activate a provider.
     */
    struct PlatformProviderModuleContribution final {
        ModuleId module;                                          /**< Trusted contributing module identity. */
        std::string providerKey;                                  /**< Canonical stable selection key. */
        PlatformProviderId provider;                              /**< Horo identity, never a native provider value. */
        PlatformServicesBackendInterfaceVersion interfaceVersion; /**< Horo backend contract implemented by the adapter. */
        PlatformServicesHostProfileMask allowedProfiles{};        /**< Explicit product/profile eligibility. */
        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> supportedServices{}; /**< Manifest capability claims. */
    };

    /** @brief Detached, complete project policy awaiting composition-time validation. */
    struct PlatformProjectConfigurationCandidate final {
        std::uint32_t schemaVersion{PlatformProjectConfigurationSchemaVersion};
        std::string projectId; /**< Stable project identity owning this policy. */
        PlatformServicesHostProfile profile{PlatformServicesHostProfile::InteractiveDevelopment};
        PlatformProviderSelection provider;
        std::array<PlatformServiceRequirement, static_cast<std::size_t>(PlatformServiceKind::Count)> services{};
        std::optional<Sha256Digest> baseFingerprint; /**< Required only for replacement to fence stale drafts. */
    };

    /** @brief Hard validation limits for project policy and contribution input. */
    struct PlatformProjectConfigurationLimits final {
        std::uint32_t maximumContributions{64};
        std::uint32_t maximumTrustedModules{128};
        std::uint32_t maximumProjectIdBytes{128};
        std::uint32_t maximumProviderKeyBytes{128};
        std::uint32_t maximumModuleIdBytes{128};
    };

    /** @brief Stable failures emitted by project policy and inert contribution validation. */
    namespace PlatformProjectConfigurationErrors {
        extern const ErrorCodeDescriptor UnsupportedVersion;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor InvalidConfiguration;
        extern const ErrorCodeDescriptor InvalidContribution;
        extern const ErrorCodeDescriptor DuplicateContribution;
        extern const ErrorCodeDescriptor UntrustedContribution;
        extern const ErrorCodeDescriptor ProviderNotFound;
        extern const ErrorCodeDescriptor ProfileDenied;
        extern const ErrorCodeDescriptor RequiredCapabilityUnsupported;
        extern const ErrorCodeDescriptor StaleReplacement;
    }  // namespace PlatformProjectConfigurationErrors

    /** @brief Immutable validated project policy consumed identically by GUI, CLI, headless and cook hosts. */
    class PlatformProjectConfiguration final {
    public:
        /** @brief Returns the owning project identity. @return Snapshot-owned stable identity. */
        [[nodiscard]] std::string_view ProjectId() const noexcept;
        /** @brief Returns the exact selected host profile. @return Validated profile. */
        [[nodiscard]] PlatformServicesHostProfile Profile() const noexcept;
        /** @brief Reports explicit Null selection. @return True only when Null was authored. */
        [[nodiscard]] bool UsesNullProvider() const noexcept;
        /** @brief Returns the selected Horo provider identity. @return Empty for explicit Null. */
        [[nodiscard]] std::optional<PlatformProviderId> SelectedProvider() const noexcept;
        /** @brief Returns the selected provider manifest key. @return Empty for explicit Null. */
        [[nodiscard]] std::string_view SelectedProviderKey() const noexcept;
        /** @brief Returns the trusted module owning the exact selected provider. @return Empty for explicit Null. */
        [[nodiscard]] const std::optional<ModuleId> &SelectedModule() const noexcept;
        /** @brief Returns typed service policy in exhaustive service order. @return Immutable policy span. */
        [[nodiscard]] std::span<const PlatformServiceRequirement> ServiceRequirements() const noexcept;
        /** @brief Returns the exact selected manifest service claims. @return All false for Null. */
        [[nodiscard]] std::span<const bool> SelectedServices() const noexcept;
        /** @brief Projects required services into backend activation policy. @return Backend-neutral exact requirements. */
        [[nodiscard]] PlatformServicesBackendConfig BackendConfig() const noexcept;
        /** @brief Returns deterministic policy/contribution provenance. @return Immutable SHA-256 fingerprint. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;

    private:
        friend Result<PlatformProjectConfiguration> BuildPlatformProjectConfiguration(const PlatformProjectConfigurationCandidate &,
                                                                                      std::span<const PlatformProviderModuleContribution>,
                                                                                      std::span<const ModuleId>,
                                                                                      const PlatformProjectConfigurationLimits &);
        friend Result<PlatformProjectConfiguration> BuildPlatformProjectConfigurationReplacement(
            const PlatformProjectConfiguration &, const PlatformProjectConfigurationCandidate &,
            std::span<const PlatformProviderModuleContribution>, std::span<const ModuleId>, const PlatformProjectConfigurationLimits &);

        std::string projectId_;
        PlatformServicesHostProfile profile_{PlatformServicesHostProfile::InteractiveDevelopment};
        bool usesNullProvider_{true};
        std::optional<PlatformProviderId> selectedProvider_;
        std::string selectedProviderKey_;
        std::optional<ModuleId> selectedModule_;
        std::array<PlatformServiceRequirement, static_cast<std::size_t>(PlatformServiceKind::Count)> services_{};
        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> selectedServices_{};
        Sha256Digest fingerprint_{};
    };

    /**
     * @brief Validates one complete project policy and trusted inert provider contribution set.
     * @param candidate Detached project-authored policy; `baseFingerprint` must be absent.
     * @param contributions Inert selected-module manifest contributions in arbitrary order.
     * @param trustedModules Exact module identities approved by package/trust composition.
     * @param limits Finite validation bounds.
     * @return Immutable deterministic policy or typed failure before provider activation.
     */
    [[nodiscard]] Result<PlatformProjectConfiguration> BuildPlatformProjectConfiguration(
        const PlatformProjectConfigurationCandidate &candidate, std::span<const PlatformProviderModuleContribution> contributions,
        std::span<const ModuleId> trustedModules, const PlatformProjectConfigurationLimits &limits = {});

    /**
     * @brief Builds a replacement fenced to an exact prior fingerprint.
     * @param previous Previously published immutable policy.
     * @param candidate Detached replacement whose `baseFingerprint` must equal `previous.Fingerprint()`.
     * @param contributions Complete inert selected-module contribution set.
     * @param trustedModules Exact module identities approved by package/trust composition.
     * @param limits Finite validation bounds.
     * @return Replacement policy or typed failure; `previous` is never mutated.
     */
    [[nodiscard]] Result<PlatformProjectConfiguration> BuildPlatformProjectConfigurationReplacement(
        const PlatformProjectConfiguration &previous, const PlatformProjectConfigurationCandidate &candidate,
        std::span<const PlatformProviderModuleContribution> contributions, std::span<const ModuleId> trustedModules,
        const PlatformProjectConfigurationLimits &limits = {});
}  // namespace Horo::PlatformServices
