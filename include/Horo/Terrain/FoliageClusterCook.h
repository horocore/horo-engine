#pragma once

/** @file FoliageClusterCook.h
 * @brief Immutable renderer-neutral foliage clusters and canonical instance payloads.
 */

#include "Horo/Terrain/FoliagePlacementCook.h"

#include <array>

namespace Horo::Terrain {
    namespace FoliageClusterCookErrors {
        extern const ErrorCodeDescriptor InvalidInput;  /**< Missing, duplicate, incompatible or damaged evidence. */
        extern const ErrorCodeDescriptor LimitExceeded; /**< Finite count, work, byte or coordinate limit exceeded. */
        extern const ErrorCodeDescriptor Cancelled;     /**< Detached work cancelled before publication. */
        extern const ErrorCodeDescriptor Stale;         /**< Replacement does not match the published generation. */
        extern const ErrorCodeDescriptor Closed;        /**< Publication owner has closed admission. */
    }  // namespace FoliageClusterCookErrors

    /** @brief Inclusive integer-millimetre geometry envelope; zero-width axes are legal. */
    struct FoliageClusterBounds final {
        std::array<std::int64_t, 3> minimum{}; /**< X, Y, Z lower corner. */
        std::array<std::int64_t, 3> maximum{}; /**< X, Y, Z upper corner. */
        [[nodiscard]] constexpr auto operator<=>(const FoliageClusterBounds &) const noexcept = default;
    };

    /** @brief Exact placement and geometry evidence, borrowed only during cook. */
    struct FoliageClusterCookSource final {
        const CookedFoliagePlacement *placement{}; /**< Cook-issued immutable placement; null is invalid. */
        Sha256Digest geometryDigest{};             /**< Verified complete mesh/LOD/wind-envelope artifact identity. */
        std::uint32_t
            geometryRadiusMillimeters{}; /**< Positive conservative unscaled sphere radius enclosing all admitted representations. */
    };

    /** @brief Exact finite target profile; no renderer, capability fallback or implicit tier selection. */
    struct FoliageClusterCookProfile final {
        TerrainConfigurationSnapshot configuration; /**< Validated exact tier, revisions and project-lowered limits. */
        Sha256Digest targetDigest{};                /**< Nonzero generic target/envelope identity. */
        Sha256Digest toolchainDigest{};             /**< Nonzero pinned cooker/toolchain identity. */
    };

    /** @brief One independently verifiable neutral cluster in canonical stable-instance-ID order. */
    struct CookedFoliageCluster final {
        TerrainTileId tile{};
        FoliageTypeId type{};
        FoliageClusterId id{};
        TerrainSourceRevision sourceRevision{};
        FoliageDefinitionRevision definitionRevision{};
        TerrainCapabilityRevision capability{};
        TerrainContentRevision placementRevision{};
        Sha256Digest placementFingerprint{};
        Sha256Digest placementDigest{};
        Sha256Digest geometryDigest{};
        std::uint32_t geometryRadiusMillimeters{};
        Sha256Digest profileFingerprint{};
        FoliageClusterBounds bounds{};
        Sha256Digest digest{};                          /**< SHA-256 of the full canonical header and instance bytes. */
        std::vector<CookedFoliageInstance> instances{}; /**< Neutral typed payload, not a GPU layout or offset identity. */
        std::vector<std::uint8_t> payload{};            /**< Schema-v1 network-order bytes; no struct padding or native handles. */
    };

    class CookedFoliageClusterSet;

    /** @brief Complete source snapshot for detached insertion or incremental generation replacement. */
    struct FoliageClusterCookRequest final {
        TerrainDatasetId dataset{};
        TerrainContentRevision content{}; /**< Aggregate publication revision, separate from per-source provenance. */
        FoliageClusterCookProfile profile;
        std::span<const FoliageClusterCookSource>
            sources{};                             /**< Complete membership, order independent; no inferred directory membership. */
        const CookedFoliageClusterSet *previous{}; /**< Optional same-dataset predecessor; exact non-wrapping successor required. */
    };

    /** @brief Cook-issued complete immutable generation; Assets owns storage/publication outside this value. */
    class CookedFoliageClusterSet final {
    public:
        CookedFoliageClusterSet(const CookedFoliageClusterSet &) = default;
        CookedFoliageClusterSet(CookedFoliageClusterSet &&) noexcept = default;
        CookedFoliageClusterSet &operator=(const CookedFoliageClusterSet &) = default;
        CookedFoliageClusterSet &operator=(CookedFoliageClusterSet &&) noexcept = default;

        /** @brief Returns exact dataset identity. @return Stable dataset. */
        [[nodiscard]] TerrainDatasetId Dataset() const noexcept {
            return dataset_;
        }

        /** @brief Returns the aggregate publication revision. @return Nonzero content revision. */
        [[nodiscard]] TerrainContentRevision ContentRevision() const noexcept {
            return content_;
        }

        /** @brief Returns the exact target profile. @return Borrowed immutable profile. */
        [[nodiscard]] const FoliageClusterCookProfile &Profile() const noexcept {
            return profile_;
        }

        /** @brief Returns the complete source/profile/revision fingerprint. @return Canonical digest. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept {
            return fingerprint_;
        }

        /** @brief Returns manifest integrity. @return Digest binding exact cluster membership, sizes and content. */
        [[nodiscard]] const Sha256Digest &ManifestDigest() const noexcept {
            return manifestDigest_;
        }

        /** @brief Returns tile/type/cluster ordered immutable clusters. @return Views valid while this value remains unmodified and alive.
         */
        [[nodiscard]] std::span<const CookedFoliageCluster> Clusters() const noexcept {
            return clusters_;
        }

        /** @brief Returns exact preparation accounting. @return Counts and peak owned byte/work estimates. */
        [[nodiscard]] const TerrainDescriptorFootprint &Footprint() const noexcept {
            return footprint_;
        }

        /**
         * @brief Revalidates complete cook-issued membership, provenance, payloads and manifest integrity.
         * @param cancellation Cooperative observer checked throughout bounded validation.
         * @return Success or typed invalid/cancelled failure, including moved-from nonempty generations.
         */
        [[nodiscard]] Result<void> Validate(const CancellationToken &cancellation = {}) const;

    private:
        friend struct FoliageClusterCookWorker;
        friend class FoliageClusterCookOwner;

        explicit CookedFoliageClusterSet(const FoliageClusterCookProfile &profile) : profile_(profile) {}

        [[nodiscard]] bool IsWellFormed(const CancellationToken &cancellation) const noexcept;
        TerrainDatasetId dataset_{};
        TerrainContentRevision content_{};
        FoliageClusterCookProfile profile_;
        Sha256Digest fingerprint_{};
        Sha256Digest manifestDigest_{};
        TerrainDescriptorFootprint footprint_{};
        std::vector<CookedFoliageCluster> clusters_{};
    };

    /**
     * @brief Cooks complete deterministic spatial clusters without changing source or live state.
     * @param request Exact target, complete cook-issued placement membership and optional predecessor.
     * @param cancellation Cooperative observer checked through collection, sorting and payload construction.
     * @return Complete candidate or typed error; oversized content is never truncated.
     * Unchanged placement/profile/geometry evidence retains byte-identical cluster payloads and stable IDs.
     * This background/tooling path owns bounded allocations; it performs no I/O, registration or native work.
     */
    [[nodiscard]] Result<CookedFoliageClusterSet> CookFoliageClusters(const FoliageClusterCookRequest &request,
                                                                      const CancellationToken &cancellation);

    /**
     * @brief Verifies an independently read payload against a trusted cook-issued cluster manifest entry.
     * @param expected Cluster metadata from a validated immutable generation.
     * @param payload Borrowed independently loaded bytes.
     * @param cancellation Cooperative observer checked between bounded hash chunks.
     * @return Success only for the exact declared size and digest; never repairs or decodes untrusted sizes.
     */
    [[nodiscard]] Result<void> VerifyFoliageClusterPayload(const CookedFoliageCluster &expected, std::span<const std::uint8_t> payload,
                                                           const CancellationToken &cancellation = {});

    /** @brief Owner-thread-only publication fence; detached values may be retained independently across replacement/close. */
    class FoliageClusterCookOwner final {
    public:
        /**
         * @brief Publishes a complete insertion or exact non-wrapping successor, with optional cancellation at the safe point.
         * @param candidate Cook-issued detached generation to own on success.
         * @param expectedCurrent Absent for insertion, exact published revision for replacement.
         * @param cancellation Owner-safe-point cancellation observer.
         * @return Typed closed/cancelled/stale/invalid error without altering the old publication, or success.
         */
        [[nodiscard]] Result<void> Publish(CookedFoliageClusterSet candidate, std::optional<TerrainContentRevision> expectedCurrent,
                                           const CancellationToken &cancellation = {});

        /** @brief Closes admission idempotently, retaining the last generation for owner-managed retirement. */
        void Close() noexcept {
            closed_ = true;
        }

        /** @brief Returns a borrowed root, invalidated by successful publication or owner destruction. @return Current root or null. */
        [[nodiscard]] const CookedFoliageClusterSet *Current() const noexcept {
            return current_ ? &*current_ : nullptr;
        }

        /** @brief Reports closed admission. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept {
            return closed_;
        }

    private:
        std::optional<CookedFoliageClusterSet> current_{};
        bool closed_{};
    };
}  // namespace Horo::Terrain
