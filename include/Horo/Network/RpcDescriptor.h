#pragma once

/**
 * @file RpcDescriptor.h
 * @brief Inert typed RPC declaration, parameter, routing, and permission metadata.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Network/ReplicationIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::Network {
    struct RpcIdentityTag;
    struct RpcParameterIdentityTag;
    struct RpcPermissionIdentityTag;

    /** @brief Globally stable semantic identity of one remotely callable declaration. */
    using RpcId = ReplicationIdentity<RpcIdentityTag, std::uint64_t>;
    /** @brief Stable non-zero parameter identity scoped by one RPC. */
    using RpcParameterId = ReplicationIdentity<RpcParameterIdentityTag, std::uint32_t>;
    /** @brief Stable identity of one host-approved custom caller-permission policy. */
    using RpcPermissionId = ReplicationIdentity<RpcPermissionIdentityTag, std::uint32_t>;

    /** @brief Permitted origin direction; it never implies authority over gameplay state. */
    enum class RpcDirection : std::uint8_t {
        ClientToAuthority,
        AuthorityToClient,
        Count
    };

    /** @brief Transport delivery requirement declared independently of a concrete backend. */
    enum class RpcDelivery : std::uint8_t {
        ReliableOrdered,
        Unreliable,
        Count
    };

    /** @brief Declared recipient class resolved later from an active session and object grant. */
    enum class RpcTarget : std::uint8_t {
        Authority,
        InvokingClient,
        ObjectOwner,
        AllClients,
        Count
    };

    /** @brief Minimum caller evidence required before gameplay dispatch may be considered. */
    enum class RpcCallerPermission : std::uint8_t {
        AuthenticatedPeer,
        ObjectOwner,
        AuthorityOnly,
        Custom,
        Count
    };

    /** @brief Whether a compatible older invocation may omit one parameter. */
    enum class RpcParameterRequirement : std::uint8_t {
        Required,
        Optional,
        Count
    };

    /** @brief Finite per-parameter canonical wire bounds. */
    struct RpcParameterLimits final {
        std::uint32_t maximumEncodedBytes{}; /**< Inclusive encoded byte limit. */
        std::uint32_t maximumElementCount{}; /**< Inclusive decoded element limit. */

        constexpr auto operator<=>(const RpcParameterLimits &) const noexcept = default;
    };

    /** @brief Canonical codec bytes substituted only for an explicitly optional parameter. */
    struct RpcParameterDefault final {
        std::vector<std::byte> canonicalBytes; /**< Owned deterministic codec output for the declared type. */

        bool operator==(const RpcParameterDefault &) const = default;
    };

    /** @brief One stable typed parameter without a C++ address, layout, or callback. */
    struct RpcParameterDescriptor final {
        RpcParameterId id;                                                      /**< Stable identity scoped by the owning RPC. */
        ReplicationValueTypeId valueType;                                       /**< Exact semantic value identity. */
        ReplicationCodecId codec;                                               /**< Exact canonical codec identity. */
        ReplicationSchemaVersion introducedVersion;                             /**< First RPC schema version carrying this parameter. */
        RpcParameterRequirement requirement{RpcParameterRequirement::Required}; /**< Compatibility omission policy. */
        RpcParameterLimits limits;                                              /**< Finite parameter wire bounds. */
        std::optional<RpcParameterDefault> canonicalDefault;                    /**< Required canonical fallback for optional parameters. */

        bool operator==(const RpcParameterDescriptor &) const = default;
    };

    /** @brief Bounded invocation-rate metadata enforced per admitted caller by NetworkRuntime. */
    struct RpcRateLimit final {
        std::uint32_t maximumCallsPerSecond{}; /**< Sustained calls admitted in a one-second window. */
        std::uint32_t maximumBurst{};          /**< Maximum immediately admitted burst within that rate. */

        constexpr auto operator<=>(const RpcRateLimit &) const noexcept = default;
    };

    /** @brief Complete inert RPC declaration; actual dispatch and gameplay callbacks are separate. */
    struct RpcDescriptor final {
        RpcId id;                                                   /**< Globally stable RPC identity. */
        ReplicationSchemaVersion version;                           /**< Current declaration schema version. */
        ReplicationCompatibilityRange compatibility;                /**< Accepted same-major invocation versions. */
        ModuleId owner;                                             /**< Gameplay module owning the semantic declaration. */
        RpcDirection direction{RpcDirection::Count};                /**< Permitted invocation origin. */
        RpcDelivery delivery{RpcDelivery::Count};                   /**< Required transport delivery semantics. */
        RpcTarget target{RpcTarget::Count};                         /**< Recipient class resolved after admission. */
        RpcCallerPermission permission{RpcCallerPermission::Count}; /**< Required active-session caller evidence. */
        std::optional<RpcPermissionId> customPermission;            /**< Host-approved policy identity when permission is Custom. */
        RpcRateLimit rateLimit;                                     /**< Per-caller admission budget. */
        std::size_t maximumPayloadBytes{};                          /**< Hard complete invocation payload bound. */
        std::vector<RpcParameterDescriptor> parameters;             /**< Stable typed parameters, canonicalized by identity. */
        std::vector<RpcParameterId> tombstonedParameters;           /**< Retired identities that can never be reused. */

        bool operator==(const RpcDescriptor &) const = default;
    };

    /** @brief Finite construction envelope for one complete RPC declaration generation. */
    struct RpcDescriptorLimits final {
        std::size_t maximumRpcs{512};                           /**< Maximum declarations in one generation. */
        std::size_t maximumParametersPerRpc{64};                /**< Maximum active plus retired parameter identities. */
        std::size_t maximumOwnerIdentityBytes{160};             /**< Maximum canonical owner identity bytes. */
        std::size_t maximumDefaultBytesPerParameter{16 * 1024}; /**< Maximum canonical default bytes per parameter. */
        std::size_t maximumTotalDefaultBytes{1024 * 1024};      /**< Maximum defaults retained by one generation. */
        std::size_t maximumPayloadBytes{1024 * 1024};           /**< Maximum declared invocation payload. */
    };

    /**
     * @brief Validates one inert RPC declaration against explicit finite limits.
     * @param descriptor Candidate declaration.
     * @param limits Host-owned construction limits.
     * @return Success or a typed malformed, conflict, incompatible, or capacity error.
     */
    [[nodiscard]] Result<void> ValidateRpcDescriptor(const RpcDescriptor &descriptor, const RpcDescriptorLimits &limits = {});
}  // namespace Horo::Network
