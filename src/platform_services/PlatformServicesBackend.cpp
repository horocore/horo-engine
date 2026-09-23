#include "Horo/PlatformServices/PlatformServicesBackend.h"

namespace Horo::PlatformServices {
    namespace {
        constexpr std::uint32_t MaxConcurrentRequests = 1U << 20U;
        constexpr std::uint32_t MaxPageEntries = 1U << 20U;
        constexpr std::uint64_t MaxPayloadBytes = 1ULL << 40U;

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceKind value) noexcept {
            return value < PlatformServiceKind::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceAvailability value) noexcept {
            return value == PlatformServiceAvailability::Available || value == PlatformServiceAvailability::Unavailable;
        }

        [[nodiscard]] constexpr bool IsKnown(const PlatformServiceUnavailableReason value) noexcept {
            return value <= PlatformServiceUnavailableReason::ProviderInitializationFailed;
        }

        [[nodiscard]] bool ValidateLimits(const PlatformServiceLimits &limits, const bool available) noexcept {
            if (limits.maxConcurrentRequests > MaxConcurrentRequests || limits.maxPageEntries > MaxPageEntries ||
                limits.maxPayloadBytes > MaxPayloadBytes)
                return false;
            return !available || limits.maxConcurrentRequests != 0;
        }

        [[nodiscard]] bool ValidateCapability(const PlatformServiceCapability &capability) noexcept {
            if (!IsKnown(capability.service) || !IsKnown(capability.availability))
                return false;
            const bool available = capability.availability == PlatformServiceAvailability::Available;
            if (!ValidateLimits(capability.limits, available))
                return false;
            if (available)
                return capability.binding && capability.binding->IsValid() && !capability.unavailableReason;
            return !capability.binding && capability.unavailableReason && IsKnown(*capability.unavailableReason);
        }
    }  // namespace

    namespace BackendErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.backend"};
            const ErrorDomainId FrontendDomain{"horo.platform.frontend"};
        }  // namespace

        const ErrorCodeDescriptor InvalidCapabilitySnapshot{Domain,
                                                            ErrorCode{"platform.backend.invalid_capabilities"},
                                                            ErrorSeverity::Error,
                                                            "Platform provider capabilities are invalid.",
                                                            "Reject the candidate and correct its complete capability snapshot.",
                                                            false,
                                                            false};
        const ErrorCodeDescriptor IncompatibleInterfaceVersion{Domain,
                                                               ErrorCode{"platform.backend.incompatible_version"},
                                                               ErrorSeverity::Error,
                                                               "Platform provider interface is incompatible.",
                                                               "Install a provider implementing the supported Horo interface version.",
                                                               false,
                                                               false};
        const ErrorCodeDescriptor RequiredServiceUnavailable{Domain,
                                                             ErrorCode{"platform.capability.required_unavailable"},
                                                             ErrorSeverity::Error,
                                                             "A required platform service is unavailable.",
                                                             "Select a provider satisfying the frozen product policy.",
                                                             false,
                                                             true};
        const ErrorCodeDescriptor ServiceUnavailable{Domain,
                                                     ErrorCode{"platform.capability.unavailable"},
                                                     ErrorSeverity::Error,
                                                     "The selected provider does not expose this platform service.",
                                                     "Disable optional use or select a compatible provider.",
                                                     false,
                                                     true};
        const ErrorCodeDescriptor NullProvider{FrontendDomain,
                                               ErrorCode{"platform.provider.null"},
                                               ErrorSeverity::Error,
                                               "The Null platform provider cannot accept remote service work.",
                                               "Select an available provider or explicitly suppress the optional intent before submission.",
                                               false,
                                               false};
    }  // namespace BackendErrors

    /** @copydoc ValidatePlatformServiceCapabilitySnapshot */
    Result<void> ValidatePlatformServiceCapabilitySnapshot(const PlatformServiceCapabilitySnapshot &snapshot,
                                                           const PlatformServicesBackendConfig &config) {
        if (snapshot.interfaceVersion !=
            PlatformServicesBackendInterfaceVersion{PlatformServicesBackendInterfaceMajor, PlatformServicesBackendInterfaceMinor})
            return Result<void>::Failure(MakeError(BackendErrors::IncompatibleInterfaceVersion));
        if (!snapshot.provider.IsValid() || !snapshot.providerGeneration.IsValid())
            return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));

        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> seen{};
        for (const auto &capability : snapshot.services) {
            if (!ValidateCapability(capability))
                return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
            const auto index = static_cast<std::size_t>(capability.service);
            if (seen[index])
                return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
            seen[index] = true;
            const bool available = capability.availability == PlatformServiceAvailability::Available;
            if (config.requiredServices[index] && !available)
                return Result<void>::Failure(MakeError(BackendErrors::RequiredServiceUnavailable));
        }
        return Result<void>::Success();
    }

    /** @copydoc ActivatePlatformServicesBackend */
    Result<PlatformServiceCapabilitySnapshot> ActivatePlatformServicesBackend(IPlatformServicesBackend &backend,
                                                                              const PlatformServicesBackendConfig &config) {
        auto inspected = backend.InspectCapabilities();
        if (inspected.HasError())
            return Result<PlatformServiceCapabilitySnapshot>::Failure(inspected.ErrorValue());
        auto snapshot = std::move(inspected).Value();
        if (const auto validated = ValidatePlatformServiceCapabilitySnapshot(snapshot, config); validated.HasError())
            return Result<PlatformServiceCapabilitySnapshot>::Failure(validated.ErrorValue());
        if (const auto activated = backend.Activate(config); activated.HasError()) {
            static_cast<void>(backend.Shutdown());
            return Result<PlatformServiceCapabilitySnapshot>::Failure(activated.ErrorValue());
        }
        return Result<PlatformServiceCapabilitySnapshot>::Success(std::move(snapshot));
    }
}  // namespace Horo::PlatformServices
