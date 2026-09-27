#pragma once

/**
 * @file DestructionReplication.h
 * @brief Bounded server-authored destruction replication schema and canonical semantic encoding.
 */

#include "Horo/Destruction/DestructionStateMachine.h"
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

    /** @brief Finite wire and semantic admission bounds, selected by the product before a session starts. */
    struct DestructionReplicationLimits final {
        std::uint32_t maximumChunks{DestructionHardLimits::ChunksPerDestructible};  /**< Artifact-local bit positions. */
        std::uint32_t maximumAnchors{DestructionHardLimits::ChunksPerDestructible}; /**< Stable support-anchor identities. */
        std::size_t maximumBytes{16 * 1024}; /**< Complete canonical payload ceiling, including fixed fields. */
    };

    /** @brief Four content-scoped canonical sets; an ordinal is meaningful only with the exact content identity. */
    struct DestructionReplicationMasks final {
        std::uint32_t chunkCount{};       /**< Exact canonical stable chunk-table length. */
        std::vector<std::byte> broken;    /**< Chunks no longer intact. */
        std::vector<std::byte> active;    /**< Broken chunks with authoritative live motion. */
        std::vector<std::byte> supported; /**< Broken chunks still supported by canonical connectivity. */
        std::vector<std::byte> dormant;   /**< Broken chunks durably dormant without a live body. */

        bool operator==(const DestructionReplicationMasks &) const = default;
    };

    /** @brief Semantic seed/cursor, not a native RNG or worker checkpoint. */
    struct DestructionReplicationSeed final {
        std::uint32_t version{}; /**< Non-zero deterministic algorithm/seed schema version. */
        std::uint64_t value{};   /**< Stable seed value; zero is a valid seed. */
        std::uint64_t cursor{};  /**< Next semantic draw position. */

        constexpr auto operator<=>(const DestructionReplicationSeed &) const noexcept = default;
    };

    /** @brief Owned authority-produced semantics; Physics motion is paired separately, never embedded here. */
    struct DestructionReplicationState final {
        DestructionHandle target;                                   /**< Exact world/destructible/runtime generation. */
        FractureArtifactContentIdentity content;                    /**< Exact chunk-table/content fingerprint. */
        DestructionConfigurationRevision configuration;             /**< Exact admitted policy generation. */
        DestructionFeatureSet effectiveFeatures;                    /**< Resolved capabilities, including authority replication. */
        Network::ReplicationAuthorityEpoch authority;               /**< Server authority generation. */
        DestructionStateRevision revision;                          /**< Monotonic semantic revision. */
        DestructionStatePhase phase{DestructionStatePhase::Intact}; /**< Canonical phase. */
        std::uint32_t healthQ16{};                                  /**< Health in unsigned Q16.16 units. */
        DestructionReplicationSeed seed;                            /**< Future deterministic decision state. */
        DestructionReplicationMasks masks;                          /**< Four content-scoped sets. */
        std::vector<DestructionChunkId> supportAnchors;             /**< Sorted stable anchor identities, never ordinals. */
        std::uint32_t supportCursor{};                              /**< Bounded canonical support-progress cursor. */

        bool operator==(const DestructionReplicationState &) const = default;
    };

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

    /** @brief Borrowed immutable exact cooked chunk-table evidence; neither encoder nor decoder retains it. */
    struct DestructionReplicationArtifactView final {
        FractureArtifactContentIdentity content;             /**< Exact published artifact identity. */
        std::span<const DestructionChunkId> canonicalChunks; /**< Unique stable IDs in canonical artifact order. */
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
