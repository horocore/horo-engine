#pragma once

/**
 * @file TerrainInputSnapshot.h
 * @brief Detached, revision-fenced Terrain height and mask input for headless PCG.
 */

#include "Horo/PCG/PCGSpatialSnapshot.h"
#include "Horo/Terrain/TerrainFoliageRegistry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::PCGTerrain {
    /** @brief One owned, canonical grid sample. Zero mask means not excluded. */
    struct TerrainInputSample final {
        float height{};                      /**< Origin-cell-local height in meters. */
        Math::Vec3 normal{0.0F, 1.0F, 0.0F}; /**< Upward-facing unit normal. */
        float slopeRadians{};                /**< Angle from world up in [0, pi/2]. */
        std::uint8_t materialLayer{};        /**< Index into the dataset's bounded layer set. */
        std::uint8_t exclusionMask{};        /**< 0 permits placement; 255 fully excludes it. */
    };

    /** @brief One grid value with its derived origin-cell-local spatial position. */
    struct TerrainInputPoint final {
        Math::Vec3 position{};     /**< Regular X/Z grid point at the copied height. */
        TerrainInputSample sample; /**< Immutable copied semantic values. */
    };

    /** @brief Fully owned Terrain-owner projection prepared at a coherent owner safe point. */
    struct TerrainInputCandidate final {
        PCG::SpatialSnapshotId snapshot{};                                  /**< Distinct captured-root identity. */
        PCG::PCGSpatialProvenance provenance{};                             /**< Host-composed provider/source/semantic revision. */
        Terrain::TerrainDatasetId dataset{};                                /**< Registered dataset owning these values. */
        Terrain::TerrainSnapshotRevision revision{};                        /**< Exact content/residency/mutation/capability fence. */
        Terrain::TerrainBoundsRevision boundsRevision{};                    /**< Exact descriptor bounds generation. */
        std::array<std::int64_t, 3> originCell{};                           /**< ADR-026 1024-meter cell of the local values. */
        std::uint64_t originEpoch{1};                                       /**< Non-zero origin-rebase generation. */
        Math::Aabb bounds{};                                                /**< Complete local requested region. */
        PCG::PCGSpatialCoverage coverage{PCG::PCGSpatialCoverage::Missing}; /**< Complete or unavailable. */
        std::uint32_t samplesX{};                                           /**< At least two regular-grid columns. */
        std::uint32_t samplesZ{};                                           /**< At least two regular-grid rows. */
        std::vector<TerrainInputSample> samples;                            /**< Row-major Z then X, copied from committed Terrain truth. */
    };

    /** @brief Detached immutable values; no Terrain registry, source array, or mutable lease is retained. */
    class TerrainInputSnapshot final {
    public:
        struct State;

        /** @brief Prevents callers from constructing an unvalidated or empty root. */
        class ConstructionKey final {
            friend Result<TerrainInputSnapshot> CaptureTerrainInput(const Terrain::TerrainFoliageRegistry &registry,
                                                                    TerrainInputCandidate candidate);
            ConstructionKey() = default;
        };

        TerrainInputSnapshot() = delete;

        explicit TerrainInputSnapshot(ConstructionKey, std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        /** @brief Returns this immutable root's identity. */
        [[nodiscard]] PCG::SpatialSnapshotId Id() const noexcept;
        /** @brief Returns provider/source/revision evidence. */
        [[nodiscard]] const PCG::PCGSpatialProvenance &Provenance() const noexcept;
        /** @brief Returns exact Terrain revision evidence. */
        [[nodiscard]] const Terrain::TerrainSnapshotRevision &Revision() const noexcept;
        /** @brief Returns the exact registry publication captured at admission. */
        [[nodiscard]] Terrain::TerrainFoliageRegistryBinding RegistryBinding() const noexcept;
        /** @brief Returns the source dataset identity. */
        [[nodiscard]] Terrain::TerrainDatasetId Dataset() const noexcept;
        /** @brief Returns the local covered region. */
        [[nodiscard]] const Math::Aabb &Bounds() const noexcept;
        /** @brief Returns immutable row-major grid values. */
        [[nodiscard]] std::span<const TerrainInputSample> Samples() const noexcept;
        /**
         * @brief Looks up one bounded grid point without touching the Terrain owner.
         * @param x Zero-based column in the captured regular grid.
         * @param z Zero-based row in the captured regular grid.
         * @return Copied spatial point or SpatialInputInvalid for out-of-bounds coordinates.
         */
        [[nodiscard]] Result<TerrainInputPoint> SampleAt(std::uint32_t x, std::uint32_t z) const;
        /** @brief Returns grid width. */
        [[nodiscard]] std::uint32_t SamplesX() const noexcept;
        /** @brief Returns grid depth. */
        [[nodiscard]] std::uint32_t SamplesZ() const noexcept;
        /** @brief Returns the captured origin generation. */
        [[nodiscard]] std::uint64_t OriginEpoch() const noexcept;

    private:
        std::shared_ptr<const State> state_;
    };

    /**
     * @brief Validates registry query/capability and copies one complete owner-produced grid.
     * @param registry Live Terrain metadata authority; no pointer to it is retained.
     * @param candidate Detached values captured from one committed Terrain generation.
     * @return Immutable snapshot or typed invalid, unavailable, stale, or capacity failure.
     * @pre The Terrain owner prepares candidate values at its publication safe point.
     */
    [[nodiscard]] Result<TerrainInputSnapshot> CaptureTerrainInput(const Terrain::TerrainFoliageRegistry &registry,
                                                                   TerrainInputCandidate candidate);

    /**
     * @brief Captures a strictly newer root without changing an existing reader.
     * @param registry Current Terrain metadata authority.
     * @param previous Retained immutable predecessor of the same provider/source/dataset.
     * @param candidate New complete owner-produced values.
     * @return Detached replacement or typed lineage/revision failure.
     */
    [[nodiscard]] Result<TerrainInputSnapshot> ReplaceTerrainInput(const Terrain::TerrainFoliageRegistry &registry,
                                                                   const TerrainInputSnapshot &previous, TerrainInputCandidate candidate);

    /**
     * @brief Checks logical currentness before PCG reuse or CurrentAtCommit.
     * @param registry Current Terrain metadata authority; closed registries reject admission.
     * @param snapshot Retained root, still memory-safe after replacement.
     * @param current Current Terrain content/residency/mutation/capability revisions from its owner.
     * @param currentSpatial Current provider/source/revision/origin tuple from the host.
     * @return Success only for exact current evidence; never infers currentness from memory lifetime.
     */
    [[nodiscard]] Result<void> ValidateTerrainInputCurrent(const Terrain::TerrainFoliageRegistry &registry,
                                                           const TerrainInputSnapshot &snapshot,
                                                           const Terrain::TerrainSnapshotRevision &current,
                                                           const PCG::PCGSpatialCurrentness &currentSpatial);
}  // namespace Horo::PCGTerrain
