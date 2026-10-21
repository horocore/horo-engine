#pragma once

/**
 * @file WorldSpatialAssignment.h
 * @brief Deterministic authored-object to world-cell cook-stage assignment contract.
 */

#include "Horo/WorldStreaming/WorldSpatialObjectDescriptor.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    /** @brief One immutable authored-object snapshot presented to the spatial cook stage. */
    struct WorldSpatialAssignmentCandidate final {
        WorldAuthoringObjectAddress address{}; /**< Stable page-scoped object address. */
        WorldAuthoringRevision revision{};     /**< Exact immutable authoring revision being cooked. */
        WorldPartitionBounds bounds{};         /**< Inclusive canonical millimeter object bounds. */
        StreamingLayerId layer{};              /**< Descriptor layer that owns the cooked object. */
        std::uint8_t lod{};                    /**< Descriptor LOD used for spatial assignment. */

        [[nodiscard]] constexpr auto operator<=>(const WorldSpatialAssignmentCandidate &) const noexcept = default;
    };

    /** @brief Mandatory host ceilings checked before spatial assignment storage is published. */
    struct WorldSpatialAssignmentLimits final {
        std::uint32_t maximumObjects{};          /**< Maximum non-zero authored-object count. */
        std::uint32_t maximumCellsPerObject{};   /**< Maximum non-zero cells assigned to one object. */
        std::uint32_t maximumTotalAssignments{}; /**< Maximum aggregate object-to-cell assignments. */
    };

    /** @brief Immutable authoring topology; deliberately contains no cooked-cell/package authority. */
    struct WorldSpatialAssignmentAuthoringDomain final {
        WorldPartitionId partition{};
        WorldPartitionBounds bounds{};
        WorldCellQuantizationPolicy grid;
        std::span<const WorldLayerDescriptor> layers; /**< At most 256 unique declared layers, borrowed for the call. */
    };

    /** @brief Canonical assignment metadata for one authored-object revision. */
    struct WorldSpatialAssignmentEntry final {
        WorldAuthoringObjectAddress address{}; /**< Stable page-scoped object address. */
        WorldAuthoringRevision revision{};     /**< Exact immutable authoring revision. */
        std::uint32_t cellOffset{};            /**< Offset into the result-owned flat cell table. */
        std::uint32_t cellCount{};             /**< Non-zero number of canonical assigned cells. */

        [[nodiscard]] constexpr auto operator<=>(const WorldSpatialAssignmentEntry &) const noexcept = default;
    };

    /** @brief Owned immutable output of one pure deterministic spatial-assignment cook pass. */
    class WorldSpatialAssignment final {
    public:
        WorldSpatialAssignment(const WorldSpatialAssignment &) = delete;
        WorldSpatialAssignment &operator=(const WorldSpatialAssignment &) = delete;
        WorldSpatialAssignment(WorldSpatialAssignment &&) noexcept = default;
        WorldSpatialAssignment &operator=(WorldSpatialAssignment &&) = delete;

        /**
         * @brief Validates and assigns authored-object bounds to descriptor cells transactionally.
         * @param descriptor Immutable partition topology and cell-membership authority; never retained or modified.
         * @param candidates Non-empty authored snapshots; input storage is never retained or modified.
         * @param limits Mandatory non-zero storage ceilings applied before result publication.
         * @return Canonically ordered owned assignments, or a stable typed error with no partial result.
         */
        [[nodiscard]] static Result<WorldSpatialAssignment> Create(const WorldPartitionDescriptor &descriptor,
                                                                   std::span<const WorldSpatialAssignmentCandidate> candidates,
                                                                   WorldSpatialAssignmentLimits limits);

        /** @brief Explains source coverage through the same quantization/count/order kernel as cooked assignment.
         * @param domain Source world bounds, validated grid and unique declared layers; no cooked cells are fabricated.
         * @param candidates Exact immutable authored revisions. @param limits Nonzero finite output ceilings.
         * @return Owned canonical coverage or typed invalid, unsupported, conflict or capacity failure.
         * @details Coverage does not prove cooked cell existence, runtime availability or residency. Tool/control path only.
         */
        [[nodiscard]] static Result<WorldSpatialAssignment> CreateForAuthoring(const WorldSpatialAssignmentAuthoringDomain &domain,
                                                                               std::span<const WorldSpatialAssignmentCandidate> candidates,
                                                                               WorldSpatialAssignmentLimits limits);

        /** @brief Returns the partition for which assignments were cooked. @return Stable immutable partition identity. */
        [[nodiscard]] constexpr const WorldPartitionId &Partition() const noexcept {
            return partition_;
        }

        /**
         * @brief Returns assignment entries ordered by page identity and object identity.
         * @return Read-only view valid until this result is moved from or destroyed.
         */
        [[nodiscard]] std::span<const WorldSpatialAssignmentEntry> Objects() const noexcept {
            return objects_;
        }

        /**
         * @brief Returns one object's cells in canonical layer, LOD, Z, Y, then X order.
         * @param objectIndex Index into Objects().
         * @return Read-only result-owned view, or an empty view for an out-of-range index.
         */
        [[nodiscard]] std::span<const StreamingCellId> CellsForObject(std::size_t objectIndex) const noexcept;

    private:
        /** @brief Stores already validated and canonically ordered owned assignment state. */
        WorldSpatialAssignment(WorldPartitionId partition, std::vector<WorldSpatialAssignmentEntry> objects,
                               std::vector<StreamingCellId> cells) noexcept;

        WorldPartitionId partition_{};
        std::vector<WorldSpatialAssignmentEntry> objects_;
        std::vector<StreamingCellId> cells_;
    };
}  // namespace Horo::WorldStreaming
