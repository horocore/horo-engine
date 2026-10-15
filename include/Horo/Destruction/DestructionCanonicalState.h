#pragma once

/** @file DestructionCanonicalState.h
 * @brief One backend-neutral semantic destruction state shared by Save and Network adapters.
 */
#include "Horo/Destruction/DestructionStateMachine.h"

#include <span>
#include <vector>

namespace Horo::Destruction {
    struct DestructionAuthorityEpochTag;
    /** @brief Exact product authority epoch, independent from a Network session. */
    using DestructionAuthorityEpoch = DestructionStableIdentity<DestructionAuthorityEpochTag>;

    /** @brief Finite canonical and semantic admission bounds, selected by the product before a capture starts. */
    struct DestructionCanonicalLimits final {
        std::uint32_t maximumChunks{DestructionHardLimits::ChunksPerDestructible};  /**< Artifact-local bit positions. */
        std::uint32_t maximumAnchors{DestructionHardLimits::ChunksPerDestructible}; /**< Stable support-anchor identities. */
        std::size_t maximumBytes{16 * 1024}; /**< Complete semantic payload ceiling, including fixed fields. */
    };

    /** @brief Four content-scoped canonical sets; an ordinal is meaningful only with the exact content identity. */
    struct DestructionCanonicalMasks final {
        std::uint32_t chunkCount{};       /**< Exact canonical stable chunk-table length. */
        std::vector<std::byte> broken;    /**< Chunks no longer intact. */
        std::vector<std::byte> active;    /**< Broken chunks with authoritative live motion. */
        std::vector<std::byte> supported; /**< Broken chunks still supported by canonical connectivity. */
        std::vector<std::byte> dormant;   /**< Broken chunks durably dormant without a live body. */

        bool operator==(const DestructionCanonicalMasks &) const = default;
    };

    /** @brief Semantic seed/cursor, not a native RNG or worker checkpoint. */
    struct DestructionCanonicalSeed final {
        std::uint32_t version{}; /**< Non-zero deterministic algorithm/seed schema version. */
        std::uint64_t value{};   /**< Stable seed value; zero is a valid seed. */
        std::uint64_t cursor{};  /**< Next semantic draw position. */

        constexpr auto operator<=>(const DestructionCanonicalSeed &) const noexcept = default;
    };

    /** @brief Owned authority-produced semantics; Physics motion is paired separately, never embedded here. */
    struct CanonicalDestructionSnapshot final {
        DestructionHandle target;                                   /**< Exact world/destructible/runtime generation. */
        FractureArtifactContentIdentity content;                    /**< Exact chunk-table/content fingerprint. */
        DestructionConfigurationRevision configuration;             /**< Exact admitted policy generation. */
        DestructionFeatureSet effectiveFeatures;                    /**< Resolved capabilities, without inferred replication. */
        DestructionAuthorityEpoch authority;                        /**< Product authority generation. */
        DestructionStateRevision revision;                          /**< Monotonic semantic revision. */
        DestructionStatePhase phase{DestructionStatePhase::Intact}; /**< Canonical phase. */
        std::uint32_t healthQ16{};                                  /**< Health in unsigned Q16.16 units. */
        DestructionCanonicalSeed seed;                              /**< Future deterministic decision state. */
        DestructionCanonicalMasks masks;                            /**< Four content-scoped sets. */
        std::vector<DestructionChunkId> supportAnchors;             /**< Sorted stable anchor identities, never ordinals. */
        std::uint32_t supportCursor{};                              /**< Bounded canonical support-progress cursor. */

        std::uint32_t schemaVersion{1}; /**< Exact semantic schema; no implicit migration. */

        bool operator==(const CanonicalDestructionSnapshot &) const = default;
    };

    /** @brief Borrowed immutable exact cooked chunk-table evidence; neither encoder nor decoder retains it. */
    struct DestructionCanonicalArtifactView final {
        FractureArtifactContentIdentity content;             /**< Exact published artifact identity. */
        std::span<const DestructionChunkId> canonicalChunks; /**< Unique stable IDs in canonical artifact order. */
    };

    /** @brief Validate complete bounded semantic values against the exact immutable chunk table.
     * @param state Complete owner-captured semantics. @param artifact Exact cooked stable-ID table.
     * @param limits Finite host profile. @return Success or typed malformed/content/capacity failure.
     */
    [[nodiscard]] Result<void> ValidateCanonicalDestructionSnapshot(const CanonicalDestructionSnapshot &state,
                                                                    const DestructionCanonicalArtifactView &artifact,
                                                                    const DestructionCanonicalLimits &limits = {});
}  // namespace Horo::Destruction
