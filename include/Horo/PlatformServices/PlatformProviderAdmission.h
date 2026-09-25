#pragma once

/**
 * @file PlatformProviderAdmission.h
 * @brief Host composition bridge for versioned Platform Services provider contributions.
 */

#include "Horo/Extensions/BackendServiceRegistry.h"
#include "Horo/Extensions/ExtensionPlatformProvider.h"
#include "Horo/PlatformServices/PlatformProjectConfiguration.h"
#include "Horo/PlatformServices/PlatformRequest.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::PlatformServices {
    struct PlatformProviderCandidateState;
    struct PlatformProviderRetirementState;
    struct PlatformProviderLifecycleState;
    class PlatformProviderAdmission;

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
        friend class PlatformProviderLifecycleHost;
        explicit PlatformProviderCandidateLease(std::shared_ptr<PlatformProviderCandidateState> state) noexcept;
        std::shared_ptr<PlatformProviderCandidateState> state_;
    };

    /** @brief Copied, monotonic provider session observation with no native identity. */
    struct PlatformProviderSessionObservation final {
        std::uint64_t revision{};
        std::uint32_t phase{};
    };

    /** @brief Stable failures of exact provider lifecycle composition. */
    namespace PlatformProviderLifecycleErrors {
        extern const ErrorCodeDescriptor InvalidSelection;
        extern const ErrorCodeDescriptor UnsupportedProfile;
        extern const ErrorCodeDescriptor InitializationFailed;
        extern const ErrorCodeDescriptor InvalidOperation;
        extern const ErrorCodeDescriptor DrainBusy;
        extern const ErrorCodeDescriptor ShutdownFailed;
    }  // namespace PlatformProviderLifecycleErrors

    /** @brief Finite monotonic timeout for each service, fixed when a host starts. */
    struct PlatformProviderRequestPolicy final {
        /** @brief Per-service admission deadline, default 30 seconds. */
        std::array<std::chrono::milliseconds, static_cast<std::size_t>(PlatformServiceKind::Count)> timeouts = [] {
            std::array<std::chrono::milliseconds, static_cast<std::size_t>(PlatformServiceKind::Count)> values{};
            values.fill(std::chrono::seconds{30});
            return values;
        }();
    };

    /**
     * @brief Narrow owner-lane composition of one exact version-2 provider operation profile.
     * @details Start publishes only after service initialization, session observation and completion ingress all succeed.
     * Native callbacks copy into bounded host queues and never call application observers. Close may report BUSY while
     * native work drains; the host retains its candidate and callback context until a successful retry.
     */
    class PlatformProviderLifecycleHost final {
    public:
        using RequestHandle = PlatformRequestHandle<void>;

        /**
         * @brief Resolves the exact immutable selection and starts each operation stage in declared order.
         * @param configuration Exact selected provider and enabled services.
         * @param admission Composition-owned provider admission.
         * @param identity Exact provider identity and generation.
         * @param authority Capability lease authorizing this consumer.
         * @param versions Accepted provider contract versions.
         * @param consumerExtensionId Requesting extension identity.
         * @param consumerModuleId Requesting module identity.
         * @param consumerGeneration Requesting activation generation.
         * @param requestPolicy Finite per-service timeouts; zero or over 24 hours fails before native creation.
         * @return Started host or typed configuration/provider failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<PlatformProviderLifecycleHost>> Start(
            const PlatformProjectConfiguration &configuration, PlatformProviderAdmission &admission,
            const Extensions::ApplicationCapabilityProviderIdentity &identity, const Extensions::ExtensionCapabilityHandle &authority,
            const Extensions::ApplicationCapabilityVersionRange &versions, std::string_view consumerExtensionId,
            std::string_view consumerModuleId, std::uint64_t consumerGeneration, PlatformProviderRequestPolicy requestPolicy = {});

        ~PlatformProviderLifecycleHost();
        PlatformProviderLifecycleHost(const PlatformProviderLifecycleHost &) = delete;
        PlatformProviderLifecycleHost &operator=(const PlatformProviderLifecycleHost &) = delete;

        /** @brief Admits one typed achievement unlock and retains native code until completion or drain. */
        [[nodiscard]] Result<RequestHandle> UnlockAchievement(AchievementId achievement);
        /**
         * @brief Applies ingress before deadline evaluation, starts queued work, then dispatches observers on the owner lane.
         * @param maximum Maximum copied completions and observer deliveries to process this turn; zero still starts queued work.
         * @param now Owner-lane monotonic time used to evaluate admission deadlines.
         * @return Number of copied completion envelopes consumed.
         * @details The composition owner must call this regularly for both launch and timeout progress.
         */
        [[nodiscard]] std::size_t DispatchCompletions(std::size_t maximum,
                                                      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /**
         * @brief Records caller cancellation once; the next owner turn asks the provider to abort executing work.
         * @param request Current typed request identity.
         * @param now Monotonic time used to distinguish an already expired queued request.
         * @return Success for accepted, repeated or terminal cancellation; typed stale/lifecycle failure otherwise.
         * @details Queued work finalizes immediately without native submission. No observer or provider callback runs here.
         */
        [[nodiscard]] Result<void> RequestCancel(const RequestHandle &request,
                                                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Returns the frontend-owned terminal or active request snapshot. */
        [[nodiscard]] Result<PlatformRequestSnapshot<void>> Query(const RequestHandle &request) const;
        /** @brief Registers one deferred completion observer. */
        [[nodiscard]] Result<PlatformRequestSubscription> OnComplete(const RequestHandle &request,
                                                                     std::function<void(const PlatformRequestSnapshot<void> &)> observer);
        /** @brief Returns the latest copied provider session observation. */
        [[nodiscard]] PlatformProviderSessionObservation Session() const noexcept;
        /** @brief Closes admission and ingress, cancels and drains native work, then releases all service state. */
        [[nodiscard]] Result<void> Close();

    private:
        explicit PlatformProviderLifecycleHost(std::shared_ptr<PlatformProviderLifecycleState> state) noexcept;
        [[nodiscard]] Result<RequestHandle> Submit(PlatformServiceKind service, std::uint32_t operation,
                                                   std::span<const std::byte> payload);
        std::shared_ptr<PlatformProviderLifecycleState> state_;
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

        /** @brief Creates only an exact published provider identity admitted to this consumer. */
        [[nodiscard]] Result<PlatformProviderCandidateLease> CreateExact(const Extensions::ApplicationCapabilityProviderIdentity &identity,
                                                                         const Extensions::ExtensionCapabilityHandle &authority,
                                                                         const Extensions::ApplicationCapabilityVersionRange &versions,
                                                                         std::string_view consumerExtensionId,
                                                                         std::string_view consumerModuleId,
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
