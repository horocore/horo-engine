#pragma once

/**
 * @file DestructionReplication.h
 * @brief Bounded server-authored destruction replication schema and canonical semantic encoding.
 */

#include "Horo/Destruction/DestructionCanonicalState.h"
#include "Horo/Network/ReplicationDescriptor.h"
#include "Horo/Network/ReplicationRoles.h"
#include "Horo/Network/ReplicationSerializer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Destruction {
    /** @brief Exact version of the destruction semantic replication payload; no implicit downgrade is admitted. */
    inline constexpr Network::ReplicationSchemaVersion DestructionReplicationVersion{1, 0};

    /** @brief Compatibility spellings of the single Destruction-owned canonical semantic model. */
    using DestructionReplicationLimits = DestructionCanonicalLimits;
    using DestructionReplicationMasks = DestructionCanonicalMasks;
    using DestructionReplicationSeed = DestructionCanonicalSeed;
    using DestructionReplicationState = CanonicalDestructionSnapshot;
    using DestructionReplicationArtifactView = DestructionCanonicalArtifactView;

    /** @brief One complete Network-owned field set, sorted by stable FieldId, not C++ member order. */
    struct DestructionReplicationField final {
        Network::FieldId id;              /**< Stable schema-scoped field identity. */
        std::vector<std::byte> canonical; /**< Complete bounded canonical bytes for this field. */

        bool operator==(const DestructionReplicationField &) const = default;
    };

    /** @brief Detached payload; outer NetworkRuntime framing supplies session, object, tick, and baseline. */
    struct DestructionReplicationPayload final {
        Network::ReplicationSchemaId schema;             /**< Exact DFR schema identity. */
        Network::ReplicationSchemaVersion version;       /**< Negotiated exact schema version. */
        std::vector<DestructionReplicationField> fields; /**< Complete required field set in ascending identity order. */
    };

    /** @brief Immutable admission fence supplied by the Network/DFR owner, not inferred from a client packet. */
    struct DestructionReplicationFence final {
        Network::NetworkSessionGeneration session;      /**< Active Network session generation. */
        Network::NetworkObjectId object;                /**< Exact replicated-object occurrence. */
        DestructionHandle target;                       /**< Exact DFR runtime generation. */
        FractureArtifactContentIdentity content;        /**< Exact published artifact generation. */
        DestructionConfigurationRevision configuration; /**< Exact current policy publication. */
        DestructionFeatureSet effectiveFeatures;        /**< Exact current admitted capabilities. */
        Network::ReplicationAuthorityEpoch authority;   /**< Current server authority epoch. */
        DestructionStateRevision currentRevision;       /**< Last committed semantic revision. */
    };

    /**
     * @brief Declares the exact inert Network-owned DFR schema with stable field IDs and finite bounds.
     * @return Complete descriptor; no registry, callbacks, transport, or ambient state are touched.
     */
    [[nodiscard]] Result<Network::ReplicationSchemaDescriptor> MakeDestructionReplicationDescriptor();

    /**
     * @brief Creates exact bounded byte-sequence serializers for every DFR field in the descriptor.
     * @return Host-owned contributions for a pinned Network serializer registry or typed capacity failure.
     */
    [[nodiscard]] Result<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>> MakeDestructionReplicationSerializers();

    /**
     * @brief Canonically encodes one owner-captured semantic state for a server-authored record.
     * @param state Owned post-commit DFR state; never native Physics or cosmetic state.
     * @param artifact Trusted exact-content chunk table borrowed for this call only.
     * @param writer Exact Network role binding; only AuthorityServer may originate fields.
     * @param limits Explicit finite product bounds.
     * @return Complete deterministic required-field payload or typed authority/compatibility/bounds failure.
     */
    [[nodiscard]] Result<DestructionReplicationPayload> EncodeDestructionReplication(const DestructionReplicationState &state,
                                                                                     const DestructionReplicationArtifactView &artifact,
                                                                                     const Network::ReplicationRoleBinding &writer,
                                                                                     const DestructionReplicationLimits &limits);

    /**
     * @brief Validates and decodes one detached server payload before any owner mutation or large allocation.
     * @param payload Complete NetworkRuntime-delivered required-field set.
     * @param fence Exact current session/object/DFR/content/configuration/capability/authority/revision evidence.
     * @param artifact Trusted exact-content chunk table borrowed only during decode.
     * @param receiver Exact client role binding; a client cannot originate authoritative state.
     * @param limits Explicit finite product bounds.
     * @return Owned semantic candidate for later aggregate owner-safe apply; failure changes no live state.
     */
    [[nodiscard]] Result<DestructionReplicationState> DecodeDestructionReplication(const DestructionReplicationPayload &payload,
                                                                                   const DestructionReplicationFence &fence,
                                                                                   const DestructionReplicationArtifactView &artifact,
                                                                                   const Network::ReplicationRoleBinding &receiver,
                                                                                   const DestructionReplicationLimits &limits);
}  // namespace Horo::Destruction
