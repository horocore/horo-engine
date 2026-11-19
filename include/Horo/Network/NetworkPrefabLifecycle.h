#pragma once

/** @file NetworkPrefabLifecycle.h @brief Bounded canonical authority Prefab spawn and despawn payloads. */

#include "Horo/Network/ReplicationWorldLifecycle.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Network {
    inline constexpr std::size_t MaximumNetworkPrefabEntities = 256;
    inline constexpr std::size_t MinimumNetworkPrefabSpawnBytes = 114;
    inline constexpr std::size_t MaximumNetworkPrefabLifecycleBytes = 7254;

    /** @brief Exact mapping for a replicated member in prepared cooked entity order. */
    struct NetworkPrefabMemberRecord final {
        std::uint32_t entitySlot{};
        NetworkObjectId object;
        ReplicationSchemaId schema;
        ReplicationSchemaVersion version;
        bool operator==(const NetworkPrefabMemberRecord &) const noexcept = default;
    };

    /** @brief Immutable cooked source and sparse replicated member declarations. */
    struct NetworkPrefabSpawnRecord final {
        NetworkPrefabTemplateIdentity recipe;
        std::vector<NetworkPrefabMemberRecord> members;
        bool operator==(const NetworkPrefabSpawnRecord &) const noexcept = default;
    };

    /** @brief One authority lifecycle payload scoped to a published world session. */
    struct NetworkPrefabLifecycleRecord final {
        NetworkSessionGeneration session;
        NetworkObjectId occurrence;
        std::optional<NetworkPrefabSpawnRecord> spawn; /**< Absent for despawn of the exact occurrence. */
        bool operator==(const NetworkPrefabLifecycleRecord &) const noexcept = default;
    };

    /**
     * @brief Builds a spawn payload from one authority mapping snapshot.
     * @param world Owned authority read lease acquired for the intended committed capture.
     * @param occurrence Exact first mapped member's identity, as stored in the mapping.
     * @return A complete spawn declaration or a typed invalid, unknown, revoked or capacity error.
     * @details The caller owns the capture's tick and dispatch ordering. This does not read mutable Scene state.
     */
    [[nodiscard]] Result<NetworkPrefabLifecycleRecord> CaptureNetworkPrefabSpawn(const ReplicationWorldReadLease &world,
                                                                                 NetworkObjectId occurrence);

    /**
     * @brief Encodes a validated canonical lifecycle payload in big-endian byte order.
     * @param record Complete spawn or exact despawn occurrence.
     * @return Owned payload or a typed invalid or capacity error.
     * @details The sender must also enforce the authenticated channel's negotiated payload budget.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeNetworkPrefabLifecycle(const NetworkPrefabLifecycleRecord &record);

    /**
     * @brief Reads a complete canonical v1 payload, rejecting malformed framing and identities.
     * @param bytes Exact envelope payload without trailing storage.
     * @return Owned protocol data or a typed malformed or capacity error.
     * @details The receiver must independently verify authenticated direction, the current world and its cooked catalog.
     */
    [[nodiscard]] Result<NetworkPrefabLifecycleRecord> DecodeNetworkPrefabLifecycle(std::span<const std::byte> bytes);
}  // namespace Horo::Network
