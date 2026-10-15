#pragma once

/** @file WorldDependencyArtifact.h
 * @brief Versioned, index-bound inert projection of validated world dependency and placement plans.
 */
#include "Horo/WorldStreaming/WorldDependencyPlan.h"
#include "Horo/WorldStreaming/WorldIndexArtifact.h"
#include "Horo/WorldStreaming/WorldSpanningObjectPlan.h"

namespace Horo::WorldStreaming {
    /** @brief Mandatory aggregate limits checked before dependency-table ownership allocation. */
    struct WorldDependencyArtifactLimits final {
        std::size_t maximumArtifactBytes{};
        std::size_t maximumDecodedAllocationBytes{}; /**< Aggregate logical table storage, including independent closure verification. */
        std::uint32_t maximumObjects{};
        std::uint32_t maximumCells{};
        std::uint32_t maximumCoverageCells{};
        std::uint32_t maximumPlacementCells{};
        std::uint32_t maximumBundleMembers{};
        std::uint32_t maximumSoftReferences{};
        std::uint32_t maximumHardCellEdges{};
        std::uint64_t maximumDerivationSteps{}; /**< Bounds candidate edge work, including duplicates before canonical deduplication. */
    };

    /** @brief Exact captured object revision and both authored coverage and actual cooked placement. */
    struct WorldDependencyObjectProjection final {
        WorldDependencyEndpoint object;
        WorldSpanningObjectPlacement placement{};
        std::vector<StreamingCellId> coverage;
        std::vector<StreamingCellId> cells; /**< Empty only for explicitly nonspatial base content. */
    };

    /** @brief One canonical cell's complete derived hard co-load targets, excluding itself. */
    struct WorldDependencyCellProjection final {
        StreamingCellId cell;
        std::vector<StreamingCellId> hardTargets;
    };

    /**
     * @brief Validated inert dependency snapshot, bound to the final complete world.index bytes.
     * @details This is not a scheduler or a new authoring authority. Placement, bundles and references are projections
     * of the existing validated plans. Decoding recomputes all derived hard-cell edges and rejects omitted targets.
     */
    struct DecodedWorldDependencies final {
        WorldPartitionId partition;
        Sha256Digest indexArtifactHash;
        std::vector<WorldDependencyObjectProjection> objects;
        std::vector<std::vector<WorldDependencyEndpoint>> bundles;
        std::vector<WorldDependencyCandidate> softReferences;
        std::vector<WorldDependencyCellProjection> cells;
    };

    /**
     * @brief Captures validated owner plans and encodes the separate canonical version-one dependency sibling.
     * @param index Previously validated exact cooked index, including its final artifact SHA-256.
     * @param assignments Authoritative immutable authored-object coverage.
     * @param placements Authoritative spanning-object policy projection for the same objects and revisions.
     * @param dependencies Authoritative hard co-load and deferred soft-reference plan.
     * @param limits Mandatory aggregate allocation and wire ceilings.
     * @param cancellation Cooperative observer checked before and throughout encoding.
     * @return Complete bytes or typed invalid/stale/capacity/cancelled failure without publication.
     * @throws std::bad_alloc on bounded ownership allocation.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeWorldDependencyArtifact(
        const DecodedWorldIndex &index, const WorldSpatialAssignment &assignments, const WorldSpanningObjectPlan &placements,
        const WorldDependencyPlan &dependencies, const WorldDependencyArtifactLimits &limits, const CancellationToken &cancellation = {});

    /**
     * @brief Validates the sibling's exact index binding, canonical tables and complete derived hard-cell closure.
     * @param bytes Complete dependency sibling bytes; never retained.
     * @param index Exact independently decoded world.index whose full artifact digest must match.
     * @param limits Mandatory count and wire ceilings admitted before copying.
     * @param cancellation Cooperative observer checked throughout validation.
     * @return Owned inert snapshot or typed malformed/version/stale/capacity/cancelled failure.
     * @details Unresolved soft targets remain explicit. Missing hard targets, stale object revisions, duplicate records,
     * noncanonical order, range overflow, reserved data and cross-generation sibling pairing are rejected.
     * @throws std::bad_alloc on bounded ownership allocation.
     */
    [[nodiscard]] Result<DecodedWorldDependencies> DecodeWorldDependencyArtifact(std::span<const std::byte> bytes,
                                                                                 const DecodedWorldIndex &index,
                                                                                 const WorldDependencyArtifactLimits &limits,
                                                                                 const CancellationToken &cancellation = {});
}  // namespace Horo::WorldStreaming
