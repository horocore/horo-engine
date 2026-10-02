#pragma once

/**
 * @file FoliagePlacementCook.h
 * @brief Bounded deterministic foliage placement from canonical integer terrain samples.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Terrain/FoliageDefinition.h"
#include "Horo/Terrain/TerrainSourceImport.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Terrain {
    struct FoliageClusterCookWorker;

    namespace FoliagePlacementCookErrors {
        extern const ErrorCodeDescriptor InvalidInput;  /**< Invalid source, rule, revision, or geometry. */
        extern const ErrorCodeDescriptor LimitExceeded; /**< Work or output exceeds an explicit finite ceiling. */
        extern const ErrorCodeDescriptor Cancelled;     /**< Candidate was discarded before publication. */
        extern const ErrorCodeDescriptor Stale;         /**< Replacement no longer matches the owner revision. */
        extern const ErrorCodeDescriptor Closed;        /**< Owner no longer admits publication. */
    }  // namespace FoliagePlacementCookErrors

    /** @brief One exact integer terrain vertex, in increasing X then increasing Z order. */
    struct FoliagePlacementSample final {
        std::int64_t altitudeMillimeters{};  /**< Canonical altitude; no floating point predicate. */
        std::uint32_t slopeMilliDegrees{};   /**< Quantized slope in [0, 90000]. */
        std::uint16_t density{};             /**< Local density multiplier in [0, 65535]. */
        bool hole{};                         /**< Hole suppresses placements. */
        std::int16_t normalXPermille{};      /**< Quantized surface normal for SurfaceNormal alignment. */
        std::int16_t normalYPermille{1'000}; /**< Positive up component. */
        std::int16_t normalZPermille{};      /**< Quantized surface normal. */
    };

    /** @brief Inclusive axis-aligned exclusion in the same integer coordinate frame as the grid. */
    struct FoliageExclusionRectangle final {
        std::int64_t minimumXMillimeters{};
        std::int64_t minimumZMillimeters{};
        std::int64_t maximumXMillimeters{};
        std::int64_t maximumZMillimeters{};

        [[nodiscard]] constexpr auto operator<=>(const FoliageExclusionRectangle &) const noexcept = default;
    };

    /** @brief Exact canonical source snapshot borrowed for one cook invocation.
     * Cells use their lower-X/lower-Z vertex for density, and the quantized candidate's
     * containing cell for altitude, slope, hole, and normal. The far edges are exclusive.
     */
    struct FoliagePlacementGrid final {
        TerrainTileId tile{};                                    /**< Stable tile identity and LOD. */
        TerrainSourceRevision sourceRevision{};                  /**< Exact source revision. */
        TerrainCapabilityRevision capability{};                  /**< Exact capability publication. */
        std::int64_t originXMillimeters{};                       /**< First sample X. */
        std::int64_t originZMillimeters{};                       /**< First sample Z. */
        std::uint32_t spacingXMillimeters{};                     /**< Positive sample spacing. */
        std::uint32_t spacingZMillimeters{};                     /**< Positive sample spacing. */
        std::uint32_t width{};                                   /**< At least two samples along X. */
        std::uint32_t height{};                                  /**< At least two samples along Z. */
        std::span<const FoliagePlacementSample> samples{};       /**< Exact row-major grid. */
        std::span<const FoliageExclusionRectangle> exclusions{}; /**< Order-independent canonical rectangles. */
    };

    /** @brief Versioned fixed-point cluster behavior; zero radius disables clump displacement.
     * Nonzero radius samples an integer disk about each cluster center before floor
     * quantization, with out-of-grid candidates rejected rather than clamped.
     */
    struct FoliageClusteringRule final {
        std::uint16_t version{1};          /**< Exact supported version. */
        std::uint16_t cellsPerCluster{1};  /**< Grid-cell span per cluster axis. */
        std::uint32_t radiusMillimeters{}; /**< Maximum center-relative candidate displacement. */
    };

    /** @brief Host-selected finite cook limits, independent of rendering backend. */
    struct FoliagePlacementCookLimits final {
        std::uint32_t maximumCells{65'536};               /**< Grid-cell admission ceiling. */
        std::uint32_t maximumCandidates{1'048'576};       /**< Candidate evaluation ceiling. */
        std::uint32_t maximumInstances{262'144};          /**< Accepted output ceiling. */
        std::uint32_t maximumClusters{4'096};             /**< Cluster output ceiling. */
        std::uint32_t maximumExclusions{4'096};           /**< Exclusion input ceiling. */
        std::uint64_t maximumPredicateChecks{16'777'216}; /**< Exclusion, separation and cluster lookup ceiling. */
    };

    /** @brief Immutable placement evidence supplied by one explicit host composition. */
    struct FoliagePlacementCookRequest final {
        FoliagePlacementGrid grid{};
        FoliageTypeDefinition definition;
        FoliageDefinitionCapabilitySet capabilities{};
        FoliageClusteringRule clustering{};
        FoliagePlacementCookLimits limits{};
        TerrainContentRevision contentRevision{};              /**< New nonzero immutable output revision. */
        Sha256Digest targetDigest{};                           /**< Generic target/envelope identity. */
        Sha256Digest toolchainDigest{};                        /**< Pinned cook toolchain identity. */
        TerrainFeatureTier tier{TerrainFeatureTier::Baseline}; /**< Exact captured feature tier. */
        Sha256Digest layerDependencyDigest{};                  /**< Exact authored layer dependency, if present. */
        Sha256Digest splineDependencyDigest{};                 /**< Exact authored spline dependency, if present. */
    };

    /** @brief Stable fixed-point transform and provenance for one baked placement. */
    struct CookedFoliageInstance final {
        FoliageInstanceId id{};
        FoliageClusterId cluster{};
        FoliageTypeId type{};
        std::int64_t xMillimeters{};
        std::int64_t altitudeMillimeters{};
        std::int64_t zMillimeters{};
        std::uint32_t scalePermille{};
        std::uint32_t yawMilliDegrees{};
        std::uint32_t slopeMilliDegrees{};
        std::uint32_t candidateOrdinal{};
        std::int16_t normalXPermille{};
        std::int16_t normalYPermille{1'000};
        std::int16_t normalZPermille{};

        [[nodiscard]] constexpr auto operator<=>(const CookedFoliageInstance &) const noexcept = default;
    };

    /** @brief Cook-issued detached candidate; caller cannot forge or edit published evidence. */
    class CookedFoliagePlacement final {
    public:
        CookedFoliagePlacement(const CookedFoliagePlacement &) = default;
        CookedFoliagePlacement(CookedFoliagePlacement &&) noexcept = default;
        CookedFoliagePlacement &operator=(const CookedFoliagePlacement &) = default;
        CookedFoliagePlacement &operator=(CookedFoliagePlacement &&) noexcept = default;

        /** @brief Returns the exact cooked tile identity. */
        [[nodiscard]] TerrainTileId Tile() const noexcept {
            return tile_;
        }

        /** @brief Returns the exact source revision. */
        [[nodiscard]] TerrainSourceRevision SourceRevision() const noexcept {
            return sourceRevision_;
        }

        /** @brief Returns the exact capability revision. */
        [[nodiscard]] TerrainCapabilityRevision CapabilityRevision() const noexcept {
            return capability_;
        }

        /** @brief Returns the stable foliage type identity. */
        [[nodiscard]] FoliageTypeId Type() const noexcept {
            return type_;
        }

        /** @brief Returns the exact foliage definition revision. */
        [[nodiscard]] FoliageDefinitionRevision DefinitionRevision() const noexcept {
            return definitionRevision_;
        }

        /** @brief Returns the new immutable content revision. */
        [[nodiscard]] TerrainContentRevision ContentRevision() const noexcept {
            return contentRevision_;
        }

        /** @brief Returns the exact generic cook target. @return Captured target/envelope digest. */
        [[nodiscard]] const Sha256Digest &TargetDigest() const noexcept {
            return targetDigest_;
        }

        /** @brief Returns the exact pinned cook toolchain. @return Captured toolchain digest. */
        [[nodiscard]] const Sha256Digest &ToolchainDigest() const noexcept {
            return toolchainDigest_;
        }

        /** @brief Returns the exact captured feature tier. @return Provider-neutral tier, never an inferred fallback. */
        [[nodiscard]] TerrainFeatureTier Tier() const noexcept {
            return tier_;
        }

        /** @brief Returns the complete canonical input fingerprint. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept {
            return fingerprint_;
        }

        /** @brief Returns the canonical output digest. */
        [[nodiscard]] const Sha256Digest &ResultDigest() const noexcept {
            return resultDigest_;
        }

        /** @brief Returns cook-owned placements in canonical candidate order. */
        [[nodiscard]] std::span<const CookedFoliageInstance> Instances() const noexcept {
            return instances_;
        }

    private:
        friend Result<CookedFoliagePlacement> CookFoliagePlacement(const FoliagePlacementCookRequest &, const CancellationToken &);
        friend class FoliagePlacementCookOwner;
        friend struct FoliageClusterCookWorker;
        CookedFoliagePlacement() = default;
        [[nodiscard]] bool IsWellFormed(const CancellationToken &cancellation = {}) const noexcept;

        TerrainTileId tile_{};
        TerrainSourceRevision sourceRevision_{};
        TerrainCapabilityRevision capability_{};
        FoliageTypeId type_{};
        FoliageDefinitionRevision definitionRevision_{};
        TerrainContentRevision contentRevision_{};
        Sha256Digest targetDigest_{};
        Sha256Digest toolchainDigest_{};
        TerrainFeatureTier tier_{TerrainFeatureTier::Baseline};
        Sha256Digest fingerprint_{};
        Sha256Digest resultDigest_{};
        std::vector<CookedFoliageInstance> instances_{};
    };

    /**
     * @brief Evaluates one bounded detached foliage candidate with a versioned integer algorithm.
     * @param request Exact source, definition, capability, cluster, target, and limit snapshot.
     * @param cancellation Cooperative cancellation checked throughout the work.
     * @return Complete immutable candidate or typed failure; no live state is changed.
     */
    [[nodiscard]] Result<CookedFoliagePlacement> CookFoliagePlacement(const FoliagePlacementCookRequest &request,
                                                                      const CancellationToken &cancellation);

    /** @brief Owner-thread publication fence for detached candidates and shutdown; do not call concurrently. */
    class FoliagePlacementCookOwner final {
    public:
        /**
         * @brief Publishes a complete candidate using exact insert or compare-and-swap replacement.
         * @param candidate Detached result to own only on success.
         * @param expectedCurrent Absent for insert, exact current content revision for replacement.
         * @return Success or typed stale/closed/invalid failure; old output survives failure.
         */
        [[nodiscard]] Result<void> Publish(CookedFoliagePlacement candidate, std::optional<TerrainContentRevision> expectedCurrent);

        /** @brief Ends admission and retains the last generation for owner-managed retirement. */
        void Close() noexcept {
            closed_ = true;
        }

        /** @brief Returns an owner-borrowed candidate, invalidated by a successful Publish or owner destruction. */
        [[nodiscard]] const CookedFoliagePlacement *Current() const noexcept {
            return current_ ? &*current_ : nullptr;
        }

        /** @brief Reports whether publication admission is closed. */
        [[nodiscard]] bool IsClosed() const noexcept {
            return closed_;
        }

    private:
        std::optional<CookedFoliagePlacement> current_{};
        bool closed_{};
    };
}  // namespace Horo::Terrain
