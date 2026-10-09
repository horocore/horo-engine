#pragma once

/** @file TerrainProducerSnapshot.h @brief Bounded immutable terrain/foliage inputs for consumer-owned collision and navigation preparation.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Terrain/FoliageClusterCook.h"
#include "Horo/Terrain/TerrainFoliageRegistry.h"
#include "Horo/Terrain/TerrainPayloadManifest.h"
#include "Horo/Terrain/TerrainSourceArtifacts.h"
#include "Horo/WorldStreaming/OriginFrame.h"
#include "Horo/WorldStreaming/WorldStreamingIdentity.h"

#include <memory>

namespace Horo::Terrain {
    namespace TerrainProducerErrors {
        extern const ErrorCodeDescriptor Invalid;     /**< Invalid schema, membership, geometry or provenance. */
        extern const ErrorCodeDescriptor Unavailable; /**< Exact requested consumer capability is absent. */
        extern const ErrorCodeDescriptor Limit;       /**< Count, owned-byte or work reservation exceeded. */
        extern const ErrorCodeDescriptor Cancelled;   /**< Cooperative cancellation prevented capture/publication. */
        extern const ErrorCodeDescriptor Stale;       /**< Captured evidence no longer matches its owner. */
        extern const ErrorCodeDescriptor Closed;      /**< Owner closed admission. */
    }  // namespace TerrainProducerErrors

    /** @brief Consumer identity; neither value grants preparation, readiness or publication authority. */
    enum class TerrainProducerConsumer : std::uint8_t {
        Collision,
        Navigation,
        Count
    };
    struct TerrainProducerRequestTag;
    /** @brief Host-issued non-wrapping producer attempt, independent of content and cell generations. */
    using TerrainProducerRequestGeneration = Foundation::Detail::NonZeroId64<TerrainProducerRequestTag, TerrainErrors::IdentityInvalid>;

    /** @brief Complete source-root and host correlation evidence; never serialized as asset truth. */
    struct TerrainProducerSnapshotHeader final {
        std::uint32_t schema{1};
        TerrainProducerConsumer consumer{TerrainProducerConsumer::Collision};
        TerrainRuntimeHandle terrain{};
        TerrainSnapshotRevision revision{};
        TerrainProducerRequestGeneration request{};
        WorldStreaming::WorldPartitionId world{};
        std::optional<WorldStreaming::StreamingFence> cell{}; /**< Absent for standalone host scope. */
        WorldStreaming::OriginFrameBinding origin{};
        Math::Transform datasetToWorld{}; /**< Explicit authored placement; vertices remain canonical dataset meters. */
        Sha256Digest targetDigest{};      /**< Exact generic Assets target, never a consumer-native target. */
        Sha256Digest manifestDigest{};    /**< Exact manifest selected by the existing Assets/Terrain authority. */
        TerrainFoliageCapabilitySet capabilities{};
        bool operator==(const TerrainProducerSnapshotHeader &) const noexcept = default;
    };

    /** @brief Project-lowered ceilings checked before copies; zero never means unlimited. */
    struct TerrainProducerSnapshotLimits final {
        std::uint32_t maximumMeshes{4'096};
        std::uint32_t maximumClusters{4'096};
        std::uint32_t maximumInstances{262'144};
        std::uint64_t maximumVertices{1'048'576};
        std::uint64_t maximumTriangles{2'097'152};
        std::uint64_t maximumOwnedBytes{128ULL * 1024 * 1024};
        std::uint64_t maximumWorkItems{8'388'608}; /**< Includes examined membership, 4-KiB hash chunks, sorting and copied values. */
    };

    /** @brief Exact neutral artifact selected by an already admitted host/cell request. */
    struct TerrainProducerTileSelection final {
        TerrainTileId tile{};
        Sha256Digest digest{};
    };

    /** @brief Exact neutral cluster selected by an already admitted host/cell request. */
    struct TerrainProducerClusterSelection final {
        FoliageClusterId cluster{};
        Sha256Digest digest{};
    };

    /** @brief Canonical indexed surface with original hole exclusions and source-grid provenance. */
    struct TerrainProducerMesh final {
        TerrainTileId tile{};
        Assets::AssetId sourceAsset{};
        TerrainSourceRevision sourceRevision{};
        Sha256Digest sourceDigest{};
        Sha256Digest cookFingerprint{};
        Sha256Digest artifactDigest{};
        std::array<Sha256Digest, 4> seams{};
        std::uint32_t sourceWidth{}, sourceHeight{}; /**< Original source-grid coverage dimensions. */
        double maximumGeometricError{};
        bool requiresSameLodNeighbors{true};
        std::vector<TerrainSourceVertex> vertices{};
        std::vector<TerrainSourceTriangle> triangles{}; /**< No removed hole triangle is reconstructed. */
    };

    /** @brief Exact analytic foliage contribution, not a native shape, body or NavMesh obstacle. */
    struct TerrainProducerFoliage final {
        TerrainTileId tile{};
        FoliageClusterId cluster{};
        FoliageTypeId type{};
        TerrainSourceRevision sourceRevision{};
        TerrainContentRevision placementRevision{};
        FoliageDefinitionRevision definitionRevision{};
        Sha256Digest clusterDigest{};
        Sha256Digest placementFingerprint{};
        Sha256Digest placementDigest{}, geometryDigest{}, profileFingerprint{};
        FoliageClusterBounds bounds{};
        std::uint32_t geometryRadiusMillimeters{};
        CookedFoliageInstance instance{};      /**< Canonical millimeters, scale, yaw and surface-normal placement. */
        FoliageCollisionDefinition geometry{}; /**< Exact unscaled cylinder/capsule; consumer resolves admitted scale. */
        FoliageSurfaceAlignment alignment{FoliageSurfaceAlignment::Upright};
    };

    /** @brief Invocation-only immutable inputs; capture retains none of these borrows. */
    struct TerrainProducerSnapshotRequest final {
        TerrainProducerSnapshotHeader header{};
        TerrainRuntimeLifecycle lifecycle{TerrainRuntimeLifecycle::Closed};
        TerrainProducerSnapshotLimits limits{};
        const TerrainPayloadManifest
            *manifest{}; /**< Mandatory authoritative membership, selected by the host's existing generation lease. */
        const CookedTerrainSourceArtifacts *terrain{}; /**< Required when tiles are selected; cook-issued root. */
        std::span<const TerrainProducerTileSelection> tiles{};
        const CookedFoliageClusterSet *foliage{}; /**< Required when clusters are selected; cook-issued root. */
        std::span<const TerrainProducerClusterSelection> clusters{};
        std::span<const FoliageTypeDefinition> definitions{}; /**< Exact host-pinned definitions for selected clusters. */
    };

    /** @brief Shared immutable copied neutral data; each consumer reader releases its own lease. */
    class TerrainProducerSnapshot final {
    public:
        /** @brief Checks lease presence, not logical currentness. @return False for a moved-from lease. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns exact correlation evidence. @return Snapshot-owned header, or invalid evidence for a moved-from lease. */
        [[nodiscard]] const TerrainProducerSnapshotHeader &Header() const noexcept;
        /** @brief Returns tile-ordered neutral surfaces. @return Immutable view valid while this snapshot lease is alive. */
        [[nodiscard]] std::span<const TerrainProducerMesh> Meshes() const noexcept;
        /** @brief Returns tile/cluster/instance-ordered analytic contributions. @return Immutable snapshot-owned view. */
        [[nodiscard]] std::span<const TerrainProducerFoliage> Foliage() const noexcept;
        /** @brief Returns the admitted neutral storage charge. @return Copied elements plus root metadata, excluding allocator bookkeeping.
         */
        [[nodiscard]] std::uint64_t OwnedBytes() const noexcept;
        /** @brief Checks logical currentness independently of retained memory lifetime.
         * @param current Complete authoritative owner fence.
         * @return Success for exact equality, or typed stale failure. */
        [[nodiscard]] Result<void> ValidateCurrent(const TerrainProducerSnapshotHeader &current) const;

    private:
        struct State;
        friend Result<TerrainProducerSnapshot> CaptureTerrainProducerSnapshot(const TerrainProducerSnapshotRequest &,
                                                                              const CancellationToken &);

        explicit TerrainProducerSnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    /**
     * @brief Projects exact offline sources into one complete consumer-specific immutable snapshot.
     * @param request Host-admitted bounded selections and one committed Terrain revision/root fence.
     * @param cancellation Cooperative observer checked throughout membership, hash and copy work.
     * @return Complete detached snapshot or contextual typed failure, never partial output or fallback.
     * @pre The Terrain owner captures coherent roots/definitions and complete header at its safe point.
     * @note Synchronous load/preparation work with bounded allocations; no I/O, jobs, native conversion,
     * safe-point calls or consumer readiness. Fully holed tiles remain explicit empty surfaces.
     * @throws std::bad_alloc If admitted storage cannot be allocated; candidates unwind without publication.
     */
    [[nodiscard]] Result<TerrainProducerSnapshot> CaptureTerrainProducerSnapshot(const TerrainProducerSnapshotRequest &request,
                                                                                 const CancellationToken &cancellation = {});

    /** @brief Integration-owned consumer-input cache only; never the Assets/Terrain publication or aggregate activation authority.
     * The host calls this cache on its integration owner lane after validating the existing generation lease.
     * A cached snapshot says nothing about ActivationTicket, consumer preparation, safe points or readiness.
     */
    class TerrainProducerSnapshotOwner final {
    public:
        /**
         * @brief Adopts one complete capture or exact successor without changing existing reader leases.
         * @param candidate Detached validated snapshot.
         * @param expectedCurrent Absent for insertion; exact complete current header for replacement.
         * @param cancellation Cancellation wins before publication.
         * @return Success or typed closed/cancelled/stale failure preserving the prior root.
         * @pre Owner lane only; request advances exactly once and semantic revisions never regress.
         */
        [[nodiscard]] Result<void> Publish(TerrainProducerSnapshot candidate, std::optional<TerrainProducerSnapshotHeader> expectedCurrent,
                                           const CancellationToken &cancellation = {});
        /** @brief Captures a reader lease. @return Immutable current root, or typed closed/unavailable failure. */
        [[nodiscard]] Result<TerrainProducerSnapshot> Snapshot() const;
        /** @brief Idempotently closes admission and drops only the owner's lease; consumer readers remain safe. */
        void Shutdown() noexcept;

    private:
        std::optional<TerrainProducerSnapshot> current_{};
        bool closed_{};
    };
}  // namespace Horo::Terrain
