#pragma once

/**
 * @file PlatformProviderAdmission.h
 * @brief Host composition bridge for versioned Platform Services provider contributions.
 */

#include "Horo/Extensions/BackendServiceRegistry.h"
#include "Horo/Extensions/ExtensionPlatformProvider.h"
#include "Horo/PlatformServices/PlatformProjectConfiguration.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::PlatformServices {
    struct PlatformProviderCandidateState;
    struct PlatformProviderRetirementState;

    /** @brief Exact provider identity, eligibility, approved permissions and Horo contract versions. */
    struct PlatformProviderContributionDescriptor final {
        Extensions::ApplicationCapabilityProviderIdentity owner;
        std::string providerKey;
        PlatformProviderId provider;
        std::array<bool, static_cast<std::size_t>(Extensions::ExtensionHostPlatform::Count)> platforms{};
        PlatformServicesHostProfileMask profiles{PlatformServicesHostProfileMask::None};
        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> services{};
        std::vector<Extensions::ExtensionPermissionId> permissions;
        PlatformServicesBackendInterfaceVersion interfaceVersion;
        Extensions::ApplicationCapabilityVersion contractVersion;
    };

    /** @brief Result of a non-blocking owner-thread retirement pass. */
    enum class PlatformProviderRetirementDisposition : std::uint8_t {
        Complete,
        Busy,
        OwnerThreadRequired,
        RestartRequired,
    };

    /** @brief Request-scoped ownership of one provider generation and its executable code. */
    class PlatformProviderRequestLease final {
    public:
        ~PlatformProviderRequestLease();
        PlatformProviderRequestLease(const PlatformProviderRequestLease &) = delete;
        PlatformProviderRequestLease &operator=(const PlatformProviderRequestLease &) = delete;
        PlatformProviderRequestLease(PlatformProviderRequestLease &&other) noexcept;
        PlatformProviderRequestLease &operator=(PlatformProviderRequestLease &&other) noexcept;

    private:
        friend class PlatformProviderCandidateLease;
        explicit PlatformProviderRequestLease(std::shared_ptr<PlatformProviderCandidateState> state) noexcept;
        std::shared_ptr<PlatformProviderCandidateState> state_;
    };

    /** @brief Host adapter's backend-generation lease; revocation closes new request admission. */
    class PlatformProviderCandidateLease final {
    public:
        ~PlatformProviderCandidateLease();
        PlatformProviderCandidateLease(const PlatformProviderCandidateLease &) = delete;
        PlatformProviderCandidateLease &operator=(const PlatformProviderCandidateLease &) = delete;
        PlatformProviderCandidateLease(PlatformProviderCandidateLease &&other) noexcept;
        PlatformProviderCandidateLease &operator=(PlatformProviderCandidateLease &&other) noexcept;

        /** @brief Admits one request before revocation and retains native code through its completion. */
        [[nodiscard]] Result<PlatformProviderRequestLease> AcquireRequestLease() const;
        /** @brief Exact immutable Horo provider identity. */
        [[nodiscard]] PlatformProviderId Provider() const noexcept;
        /** @brief Copied typed contribution evidence retained for this backend generation. */
        [[nodiscard]] const PlatformProviderContributionDescriptor &Descriptor() const noexcept;

    private:
        friend class PlatformProviderFactory;
        explicit PlatformProviderCandidateLease(std::shared_ptr<PlatformProviderCandidateState> state) noexcept;
        std::shared_ptr<PlatformProviderCandidateState> state_;
    };

    /**
     * @brief Composition-owned admission using the existing application capability and backend-service registries.
     * @details Registries and owner-thread finalization outlive every publication. The factory is staged first and can only be
     * resolved through the capability publication committed last. Destruction retains any BUSY native candidate in a bounded
     * process-lifetime quarantine; it never unloads code under a live callback.
     */
    class PlatformProviderAdmission final {
    public:
        PlatformProviderAdmission(Extensions::ApplicationCapabilityRegistry &capabilities, Extensions::BackendServiceRegistry &services,
                                  Extensions::ExtensionAdmissionPolicy policy, Extensions::ExtensionHostPlatform platform,
                                  PlatformServicesHostProfile profile);
        ~PlatformProviderAdmission();
        PlatformProviderAdmission(const PlatformProviderAdmission &) = delete;
        PlatformProviderAdmission &operator=(const PlatformProviderAdmission &) = delete;

        /** @brief Validate one copied provider claim and publish factory then capability as one observable generation. */
        [[nodiscard]] Result<Extensions::ExtensionPlatformProviderPublication> Commit(
            Extensions::ExtensionPlatformProviderCandidate candidate);

        /** @brief Resolve and invoke the exact admitted factory through both existing registries. */
        [[nodiscard]] Result<PlatformProviderCandidateLease> Create(const Extensions::ExtensionCapabilityHandle &authority,
                                                                    const Extensions::ApplicationCapabilityVersionRange &versions,
                                                                    std::string_view consumerExtensionId, std::string_view consumerModuleId,
                                                                    std::uint64_t consumerGeneration) const;

        /** @brief Retry native retire/destroy on the recorded owner thread after backend and request leases release. */
        [[nodiscard]] PlatformProviderRetirementDisposition FinalizeOnOwnerThread() noexcept;

    private:
        Extensions::ApplicationCapabilityRegistry &capabilities_;
        Extensions::BackendServiceRegistry &services_;
        Extensions::ExtensionAdmissionPolicy policy_;
        Extensions::ExtensionHostPlatform platform_;
        PlatformServicesHostProfile profile_;
        std::mutex retirementsMutex_;
        std::vector<std::shared_ptr<PlatformProviderRetirementState>> retirements_;
        std::uint64_t nextGeneration_{1};
    };
}  // namespace Horo::PlatformServices
