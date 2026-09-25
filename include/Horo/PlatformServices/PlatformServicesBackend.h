#pragma once

/**
 * @file PlatformServicesBackend.h
 * @brief Validated capability bundle and lifecycle contract for Platform Services backends.
 */

#include "Horo/PlatformServices/PlatformServiceInterfaces.h"

#include <array>
#include <optional>

namespace Horo::PlatformServices {
    inline constexpr std::uint16_t PlatformServicesBackendInterfaceMajor = 1;
    inline constexpr std::uint16_t PlatformServicesBackendInterfaceMinor = 1;

    /** @brief Horo contract version implemented by a provider adapter. */
    struct PlatformServicesBackendInterfaceVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        [[nodiscard]] constexpr auto operator<=>(const PlatformServicesBackendInterfaceVersion &) const noexcept = default;
    };

    /** @brief Stable Horo provider identity; never a native SDK handle or account value. */
    struct PlatformProviderId final {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformProviderId &) const noexcept = default;
    };

    /** @brief Private binding identity meaningful only inside the selected host adapter. */
    struct PlatformServiceBindingId final {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PlatformServiceBindingId &) const noexcept = default;
    };

    enum class PlatformServiceAvailability : std::uint8_t {
        Available,
        Unavailable
    };
    enum class PlatformServiceUnavailableReason : std::uint8_t {
        NoProviderSelected,
        NullProviderSelected,
        ServiceUnsupported,
        HostPolicyDenied,
        ProviderInitializationFailed
    };

    /** @brief Finite generic limits interpreted by the corresponding typed service contract. */
    struct PlatformServiceLimits final {
        std::uint32_t maxConcurrentRequests{};
        std::uint32_t maxPageEntries{};
        std::uint64_t maxPayloadBytes{};
    };

    /** @brief One service's immutable availability, limits, and private host binding evidence. */
    struct PlatformServiceCapability final {
        PlatformServiceKind service{PlatformServiceKind::Achievements};
        PlatformServiceAvailability availability{PlatformServiceAvailability::Unavailable};
        PlatformServiceLimits limits;
        LeaderboardQueryCapabilities leaderboardQueries;
        std::optional<PlatformServiceBindingId> binding;
        std::optional<PlatformServiceUnavailableReason> unavailableReason;
    };

    /** @brief Exact versioned capability truth copied from one provider candidate. */
    struct PlatformServiceCapabilitySnapshot final {
        PlatformServicesBackendInterfaceVersion interfaceVersion;
        PlatformProviderId provider;
        PlatformProviderGeneration providerGeneration;
        std::array<PlatformServiceCapability, static_cast<std::size_t>(PlatformServiceKind::Count)> services;
    };

    /** @brief Composition policy used only to validate and activate one candidate. */
    struct PlatformServicesBackendConfig final {
        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> requiredServices{};
    };

    /** @brief Errors owned by backend capability validation and activation. */
    namespace BackendErrors {
        /** @brief The candidate omitted, duplicated, contradicted, or exceeded bounded capability evidence. */
        extern const ErrorCodeDescriptor InvalidCapabilitySnapshot;
        /** @brief The provider interface version is not exactly supported by this host. */
        extern const ErrorCodeDescriptor IncompatibleInterfaceVersion;
        /** @brief Frozen product policy requires a service that the candidate marks unavailable. */
        extern const ErrorCodeDescriptor RequiredServiceUnavailable;
        /** @brief A typed service method was called while its capability was unavailable. */
        extern const ErrorCodeDescriptor ServiceUnavailable;
        /** @brief A supported service is available, but this exact leaderboard query kind is not. */
        extern const ErrorCodeDescriptor UnsupportedOperation;
    }  // namespace BackendErrors

    /**
     * @brief Backend-neutral service bundle owned by one process composition.
     * @details Implementations own all service/native state. Every inherited service method returns a typed failure when its
     * capability is unavailable; no nullable service pointer is exposed. Shutdown is explicit and idempotent, closes new
     * admission, requests cancellation, drains callbacks, and releases native state only after borrowed service calls end.
     */
    class IPlatformServicesBackend : public IAchievementService,
                                     public ILeaderboardStatService,
                                     public ICloudService,
                                     public IPresenceService,
                                     public IFriendsService,
                                     public ISessionService {
    public:
        ~IPlatformServicesBackend() override = default;
        /** @brief Returns copied inert capability evidence without activating service admission. @return Candidate snapshot or typed
         * failure. */
        [[nodiscard]] virtual Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const = 0;
        /** @brief Activates a previously validated candidate. @param config Frozen composition requirements. @return Success or typed
         * initialization failure. */
        [[nodiscard]] virtual Result<void> Activate(const PlatformServicesBackendConfig &config) = 0;
        /** @brief Requests cancellation without running completion inline. @param request Frontend-owned request identity. @param
         * generation Owning frontend generation. @return Typed acknowledgement/failure. */
        [[nodiscard]] virtual Result<void> RequestCancel(PlatformRequestId request, PlatformRequestGeneration generation) = 0;
        /** @brief Idempotently closes admission, drains provider callbacks, and releases owned state. @return Success or typed
         * retained-resource failure. */
        [[nodiscard]] virtual Result<void> Shutdown() = 0;
    };

    /**
     * @brief Validates one complete capability snapshot without invoking lifecycle callbacks.
     * @param snapshot Copied provider candidate facts.
     * @param config Frozen required-service policy.
     * @return Success or a typed structural, version, or required-service failure.
     */
    [[nodiscard]] Result<void> ValidatePlatformServiceCapabilitySnapshot(const PlatformServiceCapabilitySnapshot &snapshot,
                                                                         const PlatformServicesBackendConfig &config);

    /**
     * @brief Inspects, validates, then activates one backend candidate transactionally.
     * @param backend Candidate that remains owned by the composition root.
     * @param config Frozen required-service policy.
     * @return The validated immutable snapshot after successful activation, or typed failure.
     * @post Activate is never called when inspection or validation fails.
     * @post Activation failure invokes Shutdown once to roll back partially initialized provider state.
     */
    [[nodiscard]] Result<PlatformServiceCapabilitySnapshot> ActivatePlatformServicesBackend(IPlatformServicesBackend &backend,
                                                                                            const PlatformServicesBackendConfig &config);
}  // namespace Horo::PlatformServices
