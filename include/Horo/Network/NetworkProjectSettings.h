#pragma once

/**
 * @file NetworkProjectSettings.h
 * @brief Backend-neutral network project authority and user-preview preference contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/StrongId.h"
#include "Horo/Network/NetworkAddress.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ProtocolIdentity.h"
#include "Horo/Network/TransportCapabilities.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Horo::Network {
    namespace Detail {
        struct NetworkProjectSettingsIdTag;
        struct NetworkProjectSettingsRevisionTag;
        struct NetworkProjectSettingsFingerprintTag;
        struct NetworkProjectProfileIdTag;
        struct NetworkProjectProfileRevisionTag;
    }  // namespace Detail

    /** @brief Stable identity of one portable project network-settings authority. */
    using NetworkProjectSettingsId = Foundation::Detail::NonZeroId64<Detail::NetworkProjectSettingsIdTag, NetworkErrors::IdentityInvalid>;
    /** @brief Monotonic revision of one project network-settings publication. */
    using NetworkProjectSettingsRevision =
        Foundation::Detail::NonZeroId64<Detail::NetworkProjectSettingsRevisionTag, NetworkErrors::IdentityInvalid>;
    /** @brief Deterministic fingerprint of one complete project network-settings snapshot. */
    using NetworkProjectSettingsFingerprint =
        Foundation::Detail::NonZeroId64<Detail::NetworkProjectSettingsFingerprintTag, NetworkErrors::IdentityInvalid>;
    /** @brief Stable identity of one network scheduling profile referenced by project settings. */
    using NetworkProjectProfileId = Foundation::Detail::NonZeroId64<Detail::NetworkProjectProfileIdTag, NetworkErrors::IdentityInvalid>;
    /** @brief Monotonic revision of one network scheduling profile. */
    using NetworkProjectProfileRevision =
        Foundation::Detail::NonZeroId64<Detail::NetworkProjectProfileRevisionTag, NetworkErrors::IdentityInvalid>;

    /**
     * @brief User-only local preview controls; these values are never project or packaged runtime authority.
     *
     * This type intentionally does not appear in NetworkProjectSettingsInput. A preview adapter may use it
     * to resolve a local editor session, but it cannot change a project profile, release artifact, or server
     * configuration.
     */
    struct NetworkPreviewPreferences final {
        static constexpr std::uint32_t MinimumPreviewClients = 1;
        static constexpr std::uint32_t MaximumPreviewClients = 16;
        static constexpr std::uint32_t MaximumSimulatedLatencyMilliseconds = 500;

        std::uint32_t maxPreviewClients{4};           /**< Concurrent local preview clients. */
        std::uint32_t simulatedLatencyMilliseconds{}; /**< Artificial one-way loopback delay. */

        /** @brief Checks the bounded user-preview representation. @return True when all values are admissible. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maxPreviewClients >= MinimumPreviewClients && maxPreviewClients <= MaximumPreviewClients &&
                   simulatedLatencyMilliseconds <= MaximumSimulatedLatencyMilliseconds;
        }

        constexpr auto operator<=>(const NetworkPreviewPreferences &) const noexcept = default;
    };

    /** @brief Positive bounded count used by a project network profile. */
    struct NetworkCountLimit final {
        static constexpr std::uint32_t MaximumValue = 1'000'000;

        std::uint32_t value{};

        /** @brief Checks the finite count bound. @return True when the count is positive and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value <= MaximumValue;
        }

        constexpr auto operator<=>(const NetworkCountLimit &) const noexcept = default;
    };

    /** @brief Positive bounded network tick frequency in Hertz. */
    struct NetworkTickRate final {
        static constexpr std::uint32_t MaximumValue = 1'000;

        std::uint32_t value{};

        /** @brief Checks the finite tick-rate bound. @return True when the rate is positive and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value <= MaximumValue;
        }

        constexpr auto operator<=>(const NetworkTickRate &) const noexcept = default;
    };

    /** @brief Positive bounded byte-per-second rate used by a project network profile. */
    struct NetworkByteRate final {
        static constexpr std::uint64_t MaximumValue = 1'000'000'000ULL;

        std::uint64_t value{};

        /** @brief Checks the finite byte-rate bound. @return True when the rate is positive and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value <= MaximumValue;
        }

        constexpr auto operator<=>(const NetworkByteRate &) const noexcept = default;
    };

    /** @brief Positive bounded byte count used by a project network profile. */
    struct NetworkByteCount final {
        static constexpr std::uint64_t MaximumValue = 1'000'000'000ULL;

        std::uint64_t value{};

        /** @brief Checks the finite byte-count bound. @return True when the count is positive and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value <= MaximumValue;
        }

        constexpr auto operator<=>(const NetworkByteCount &) const noexcept = default;
    };

    /** @brief Positive bounded work budget in microseconds. */
    struct NetworkMicroseconds final {
        static constexpr std::uint64_t MaximumValue = 60'000'000ULL;

        std::uint64_t value{};

        /** @brief Checks the finite work-time bound. @return True when the duration is positive and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value <= MaximumValue;
        }

        constexpr auto operator<=>(const NetworkMicroseconds &) const noexcept = default;
    };

    /** @brief Positive bounded tick count used by a project scheduling profile. */
    struct NetworkTickCount final {
        static constexpr std::uint32_t MaximumValue = 1'000'000;

        std::uint32_t value{};

        /** @brief Checks the finite tick-count bound. @return True when the count is positive and bounded. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value <= MaximumValue;
        }

        constexpr auto operator<=>(const NetworkTickCount &) const noexcept = default;
    };

    /** @brief Closed runtime roles that a portable network project may declare. */
    enum class NetworkProjectRole : std::uint8_t {
        Standalone,
        Client,
        ListenServer,
        DedicatedServer,
        Count,
    };

    /** @brief Bitset of roles supported by one project artifact. */
    enum class NetworkProjectRoleSet : std::uint32_t {
        None = 0,
        Standalone = 1U << 0U,
        Client = 1U << 1U,
        ListenServer = 1U << 2U,
        DedicatedServer = 1U << 3U,
    };

    /** @brief Combines two declared project roles. @param left First role set. @param right Second role set. @return Combined set. */
    [[nodiscard]] constexpr NetworkProjectRoleSet operator|(const NetworkProjectRoleSet left, const NetworkProjectRoleSet right) noexcept {
        return static_cast<NetworkProjectRoleSet>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
    }

    /** @brief Tests whether a role is declared in a role set. @param roles Declared roles. @param role Role to inspect. @return True when
     * present. */
    [[nodiscard]] constexpr bool ContainsNetworkProjectRole(const NetworkProjectRoleSet roles, const NetworkProjectRole role) noexcept {
        const auto bit = [&]() constexpr {
            switch (role) {
                case NetworkProjectRole::Standalone:
                    return NetworkProjectRoleSet::Standalone;
                case NetworkProjectRole::Client:
                    return NetworkProjectRoleSet::Client;
                case NetworkProjectRole::ListenServer:
                    return NetworkProjectRoleSet::ListenServer;
                case NetworkProjectRole::DedicatedServer:
                    return NetworkProjectRoleSet::DedicatedServer;
                case NetworkProjectRole::Count:
                default:
                    return NetworkProjectRoleSet::None;
            }
        }();
        return bit != NetworkProjectRoleSet::None && (static_cast<std::uint32_t>(roles) & static_cast<std::uint32_t>(bit)) != 0;
    }

    /** @brief Version-one scheduling and transport budgets owned by project policy. */
    struct NetworkProjectProfileV1 final {
        NetworkProjectProfileId id{};                                   /**< Stable profile identity. */
        NetworkProjectProfileRevision revision{};                       /**< Monotonic profile revision. */
        NetworkTickRate networkTickRate{};                              /**< Network simulation frequency. */
        NetworkCountLimit maxActiveConnections{};                       /**< Authority-world connection ceiling. */
        NetworkCountLimit maxNetworkObjects{};                          /**< Authority-world object ceiling. */
        NetworkCountLimit maxCandidatesPerConnectionPerTick{};          /**< Candidate evaluation ceiling. */
        NetworkCountLimit maxRelevantObjectsPerConnection{};            /**< Relevant-object ceiling. */
        NetworkCountLimit maxInterestTransitionsPerConnectionPerTick{}; /**< Interest transition ceiling. */
        NetworkCountLimit maxCapturedObjectsPerTick{};                  /**< Capture ceiling. */
        NetworkCountLimit maxSerializedFieldsPerConnectionPerTick{};    /**< Serialization-field ceiling. */
        NetworkMicroseconds maxGlobalInterestWorkPerTick{};             /**< Shared interest work ceiling. */
        NetworkMicroseconds maxGlobalCaptureWorkPerTick{};              /**< Shared capture work ceiling. */
        NetworkMicroseconds maxGlobalSchedulingWorkPerTick{};           /**< Shared scheduling work ceiling. */
        NetworkByteRate targetBytesPerConnection{};                     /**< Per-connection target throughput. */
        NetworkByteCount burstBytesPerConnection{};                     /**< Per-connection burst ceiling. */
        NetworkCountLimit maxMessagesPerConnectionPerTick{};            /**< Per-tick message ceiling. */
        NetworkByteCount maxReliableQueuedBytesPerConnection{};         /**< Reliable queue ceiling. */
        NetworkByteCount maxUnreliableQueuedBytesPerConnection{};       /**< Unreliable queue ceiling. */
        NetworkTickCount enterDwellTicks{};                             /**< Relevance-enter dwell. */
        NetworkTickCount exitDwellTicks{};                              /**< Relevance-exit dwell. */
        NetworkTickCount maxEligibleStarvationTicks{};                  /**< Fairness starvation ceiling. */
        NetworkTickCount saturationGraceTicks{};                        /**< Overload grace period. */

        constexpr auto operator<=>(const NetworkProjectProfileV1 &) const noexcept = default;
    };

    /** @brief Protocol family and schema fingerprint required by project network policy. */
    struct NetworkProjectProtocolPolicy final {
        ProtocolId protocol{};                    /**< Exact stable protocol identity. */
        ProtocolVersionRange supportedVersions{}; /**< Same-major compatible range. */
        std::uint64_t schemaFingerprint{};        /**< Non-zero exact schema contract fingerprint. */

        constexpr auto operator<=>(const NetworkProjectProtocolPolicy &) const noexcept = default;
    };

    /** @brief Optional or required transport capability policy owned by the project. */
    enum class NetworkProjectTransportRequirement : std::uint8_t {
        Optional,
        Required,
        Count,
    };

    /** @brief Transport requirement projection carried by a project settings snapshot. */
    struct NetworkProjectTransportPolicy final {
        NetworkProjectTransportRequirement requirement{NetworkProjectTransportRequirement::Optional};
        TransportRequirements capabilities{}; /**< Exact delivery and bounded transport requirements. */

        constexpr auto operator<=>(const NetworkProjectTransportPolicy &) const noexcept = default;
    };

    /** @brief Detached candidate used to construct or replace project network authority. */
    struct NetworkProjectSettingsInput final {
        static constexpr std::uint32_t CurrentContractVersion = 2;

        std::uint32_t contractVersion{CurrentContractVersion};             /**< Closed project-settings contract version. */
        NetworkProjectSettingsId settings{};                               /**< Stable settings authority identity. */
        NetworkProjectSettingsRevision revision{};                         /**< Monotonic settings publication revision. */
        NetworkProjectRoleSet supportedRoles{NetworkProjectRoleSet::None}; /**< Explicit package/project roles. */
        NetworkProjectRole defaultRole{NetworkProjectRole::Count};         /**< Default role, which must be declared. */
        NetworkProjectProfileV1 profile{};                                 /**< Complete version-one scheduling profile. */
        NetworkProjectProtocolPolicy protocol{};                           /**< Exact protocol/schema policy. */
        NetworkProjectTransportPolicy transport{};                         /**< Exact transport requirement policy. */
        NetworkAddress defaultEndpoint{};        /**< Optional portable endpoint; host may override within policy. */
        std::uint32_t credentialRequirementId{}; /**< Stable public requirement ID, never a credential reference. */
    };

    /**
     * @brief Constructs documented bounded standalone project defaults for a new identity.
     * @param settings Non-zero project-settings identity chosen by the project owner.
     * @return Complete current-version input or an identity error.
     */
    [[nodiscard]] Result<NetworkProjectSettingsInput> DefaultNetworkProjectSettings(NetworkProjectSettingsId settings);

    /** @brief Immutable validated project network authority. */
    class NetworkProjectSettings final {
    public:
        /**
         * @brief Validates and captures a complete project authority without side effects.
         * @param input Detached project-owned candidate.
         * @return Immutable settings or a typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<NetworkProjectSettings> Create(const NetworkProjectSettingsInput &input);

        /**
         * @brief Validates a successor while preserving the previous authority on failure.
         * @param previous Last-good immutable settings.
         * @param input Complete candidate with the same identity and a newer revision.
         * @return Immutable successor or a typed stale/invalid/capacity failure.
         */
        [[nodiscard]] static Result<NetworkProjectSettings> Replace(const NetworkProjectSettings &previous,
                                                                    const NetworkProjectSettingsInput &input);

        /** @brief Returns the stable settings identity. @return Non-zero settings identity. */
        [[nodiscard]] NetworkProjectSettingsId Settings() const noexcept;
        /** @brief Returns the exact project-settings revision. @return Non-zero monotonic revision. */
        [[nodiscard]] NetworkProjectSettingsRevision Revision() const noexcept;
        /** @brief Returns the deterministic snapshot fingerprint. @return Non-zero fingerprint. */
        [[nodiscard]] NetworkProjectSettingsFingerprint Fingerprint() const noexcept;
        /** @brief Returns the portable contract version. @return Exact version number. */
        [[nodiscard]] std::uint32_t ContractVersion() const noexcept;
        /** @brief Returns declared runtime roles. @return Explicit role set. */
        [[nodiscard]] NetworkProjectRoleSet SupportedRoles() const noexcept;
        /** @brief Returns the default declared role. @return Exact default role. */
        [[nodiscard]] NetworkProjectRole DefaultRole() const noexcept;
        /** @brief Returns the immutable scheduling profile. @return Complete version-one profile. */
        [[nodiscard]] const NetworkProjectProfileV1 &Profile() const noexcept;
        /** @brief Returns the immutable protocol policy. @return Exact protocol/schema policy. */
        [[nodiscard]] const NetworkProjectProtocolPolicy &Protocol() const noexcept;
        /** @brief Returns the immutable transport policy. @return Exact transport requirement policy. */
        [[nodiscard]] const NetworkProjectTransportPolicy &Transport() const noexcept;
        /** @brief Returns the optional portable default endpoint. @return Canonical endpoint or invalid when absent. */
        [[nodiscard]] const NetworkAddress &DefaultEndpoint() const noexcept;
        /** @brief Returns the public credential requirement identity. @return Zero when no credential is required. */
        [[nodiscard]] std::uint32_t CredentialRequirementId() const noexcept;

    private:
        explicit NetworkProjectSettings(const NetworkProjectSettingsInput &input, NetworkProjectSettingsFingerprint fingerprint) noexcept;

        NetworkProjectSettingsInput input_;
        NetworkProjectSettingsFingerprint fingerprint_;
    };

    /**
     * @brief Encodes one validated portable project policy as canonical bounded JSON.
     * @param settings Last-good project policy.
     * @return Deterministic JSON with public requirement IDs only.
     */
    [[nodiscard]] std::string SerializeNetworkProjectSettings(const NetworkProjectSettings &settings);

    /**
     * @brief Parses version two or migrates the documented version-one policy before validation.
     * @param document Untrusted portable JSON, limited to 64 KiB and strict known fields.
     * @return Complete validated candidate or a typed malformed/capacity failure. No state is applied.
     */
    [[nodiscard]] Result<NetworkProjectSettingsInput> ParseNetworkProjectSettings(std::string_view document);

    /**
     * @brief Preflights a selected role and exact transport evidence for configure, cook, start or automation.
     * @param settings Validated immutable policy.
     * @param role Product role selected by the caller.
     * @param capabilities Exact available transport evidence; ignored for standalone.
     * @return Success or a typed capability/role error without side effects.
     */
    [[nodiscard]] Result<void> PreflightNetworkProjectSettings(const NetworkProjectSettings &settings, NetworkProjectRole role,
                                                               const TransportCapabilities &capabilities);

    /** @brief Typed replacement command submitted to the project-settings authority. */
    struct NetworkProjectSettingsCommand final {
        NetworkProjectSettingsRevision expectedRevision{}; /**< Revision captured by the command producer. */
        NetworkProjectSettingsInput candidate{};           /**< Complete replacement candidate. */
    };

    /** @brief Explicit owner lifecycle for the project-settings publication gate. */
    enum class NetworkProjectSettingsLifecycle : std::uint8_t {
        Active,
        Closed,
        Count,
    };

    /** @brief Immutable projection returned after an authority snapshot or successful replacement. */
    struct NetworkProjectSettingsSnapshot final {
        NetworkProjectSettings settings; /**< Last-good complete project authority. */
    };

    /** @brief Owner-scoped typed command gate for atomic network project-settings publication. */
    class NetworkProjectSettingsAuthority final {
    public:
        /** @brief Constructs an active authority from one complete validated candidate. @param input Initial candidate. @return Authority
         * or typed validation failure. */
        [[nodiscard]] static Result<NetworkProjectSettingsAuthority> Create(const NetworkProjectSettingsInput &input);

        NetworkProjectSettingsAuthority(const NetworkProjectSettingsAuthority &) = delete;
        NetworkProjectSettingsAuthority(NetworkProjectSettingsAuthority &&) noexcept = default;
        NetworkProjectSettingsAuthority &operator=(const NetworkProjectSettingsAuthority &) = delete;
        NetworkProjectSettingsAuthority &operator=(NetworkProjectSettingsAuthority &&) noexcept = default;

        /**
         * @brief Applies one complete replacement only when its expected revision is current.
         * @param command Typed command carrying the captured revision and complete candidate.
         * @return New immutable snapshot or a typed stale, invalid, or shutdown failure.
         */
        [[nodiscard]] Result<NetworkProjectSettingsSnapshot> Apply(const NetworkProjectSettingsCommand &command);

        /** @brief Closes the authority gate and rejects later commands. */
        void Shutdown() noexcept;
        /** @brief Returns the current lifecycle. @return Active or Closed. */
        [[nodiscard]] NetworkProjectSettingsLifecycle Lifecycle() const noexcept;
        /** @brief Returns the last-good immutable snapshot. @return Current project authority. */
        [[nodiscard]] NetworkProjectSettingsSnapshot Snapshot() const;

    private:
        explicit NetworkProjectSettingsAuthority(NetworkProjectSettings settings) noexcept;

        NetworkProjectSettings settings_;
        NetworkProjectSettingsLifecycle lifecycle_{NetworkProjectSettingsLifecycle::Active};
    };
}  // namespace Horo::Network
