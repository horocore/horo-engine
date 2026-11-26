#pragma once

/** @file NavigationRuntimeTileRebuild.h
 * @brief Detached runtime tile rebuild and exact owner-lane publication.
 */

#include "Horo/Navigation/NavigationTileArtifact.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"

namespace Horo::Navigation {
    /** @brief Host-observed authority for one immutable post-change geometry capture. */
    struct NavigationRuntimeRebuildFence final {
        NavigationWorldActivationDescriptor activation; /**< Exact predecessor Scene, world and topology. */
        NavigationBakeInputRevisions sources;           /**< Exact captured geometry, semantics and request revisions. */
        std::uint64_t overlay{};                        /**< Nonzero applied overlay revision. */
        std::uint64_t origin{};                         /**< Nonzero coordinate-origin revision. */
        std::uint64_t worldChanges{};                   /**< Nonzero terrain, destruction and command authority revision. */

        constexpr auto operator<=>(const NavigationRuntimeRebuildFence &) const noexcept = default;
    };

    /** @brief Exclusive Scene admission held through worker execution and any unpublished result. */
    class NavigationRuntimeRebuildReservation final {
    public:
        /** @brief Leaves the request under standalone caller-owned budgeting. */
        NavigationRuntimeRebuildReservation() noexcept = default;
        NavigationRuntimeRebuildReservation(NavigationRuntimeRebuildReservation &&) noexcept = default;
        NavigationRuntimeRebuildReservation &operator=(NavigationRuntimeRebuildReservation &&) noexcept = default;
        NavigationRuntimeRebuildReservation(const NavigationRuntimeRebuildReservation &) = delete;
        NavigationRuntimeRebuildReservation &operator=(const NavigationRuntimeRebuildReservation &) = delete;

    private:
        friend class NavigationAssetSceneActivationParticipant;
        friend class NavigationRuntimeRebuildJobs;
        friend class NavigationRuntimeTileRebuildCandidate;
        struct State;

        /** @brief Checks whether the request may use the detached budget. */
        [[nodiscard]] bool Matches(const NavigationRuntimeRebuildFence &expected, std::size_t bytes) const noexcept {
            return !state_ || (expected_ == expected && maximumOwnedBytes_ == bytes);
        }

        std::shared_ptr<const State> state_;
        NavigationRuntimeRebuildFence expected_;
        std::size_t maximumOwnedBytes_{};
    };

    /** @brief Host-qualified projection of existing authored tile-local links against detached replacement polygons. */
    class INavigationRuntimeLinkProjector {
    public:
        virtual ~INavigationRuntimeLinkProjector() = default;

        /** @brief Projects both endpoints and verifies traversal clearance under the exact post-change source capture.
         * @param input Immutable post-change geometry and modifier authority.
         * @param tile Exact scope, surface, profile and coordinates owning the link.
         * @param topology Detached link-free topology. Polygon identities are local to this replacement tile.
         * @param previous Authored link whose identity, direction and area must be preserved.
         * @param maximumScratchBytes Hard ceiling for projection and evidence validation allocations.
         * @param cancellation Observed around bounded provider calls and work units.
         * @return One revalidated tile-local link or a typed failure that rejects the complete rebuild.
         */
        [[nodiscard]] virtual Result<NavMeshOffMeshLink> Project(const NavigationBakeInputSnapshot &input,
                                                                 const NavigationBakeTileKey &tile,
                                                                 const NavigationTileBuildResult &topology,
                                                                 const NavMeshOffMeshLink &previous, std::size_t maximumScratchBytes,
                                                                 const CancellationToken &cancellation) const = 0;
    };

    /** @brief Immutable closed grid and captured post-change inputs borrowed for one detached rebuild. */
    struct NavigationRuntimeTileRebuildRequest final {
        NavigationRuntimeRebuildFence expected;
        NavigationGeneration nextTopology;
        const NavigationBakeInputSnapshot &input;
        std::span<const NavigationBakeTile> layout; /**< Exact sorted active keys, including empty tiles. */
        std::span<const std::shared_ptr<const NavigationCookedTile>> activeTiles;
        NavigationTileBakeCompatibility compatibility;
        NavigationTileBuildLimits tileLimits{};
        std::size_t maximumRebuiltTiles{32};
        std::size_t maximumOwnedBytes{64U * 1024U * 1024U};
        const INavigationRuntimeLinkProjector *linkProjector{}; /**< Required when the predecessor contains authored off-mesh links. */
    };

    /** @brief Detached provider and its retained storage reservation, separate from the portable tiles. */
    struct NavigationPreparedRuntimeRebuildBackend final {
        std::unique_ptr<INavigationQueryBackend> backend;
        std::size_t reservedProviderBytes{}; /**< Positive and within the factory's admitted budget for nonempty coverage. */
    };

    /** @brief Host-selected materializer for a complete detached, nonempty tile closure. */
    class INavigationRuntimeRebuildBackendFactory {
    public:
        virtual ~INavigationRuntimeRebuildBackendFactory() = default;

        /** @brief Prepares one immutable query provider without publishing it.
         * @param activation The exact successor Scene, world and topology.
         * @param tiles Complete sorted closure pinned for this call.
         * @param maximumOwnedBytes Remaining budget for provider-owned resources and temporary preparation.
         * @param cancellation Cancellation observed at bounded work boundaries.
         * @return Owned provider with its verified storage reservation, or a typed failure.
         */
        [[nodiscard]] virtual Result<NavigationPreparedRuntimeRebuildBackend> Prepare(
            const NavigationWorldActivationDescriptor &activation, std::span<const std::shared_ptr<const NavigationCookedTile>> tiles,
            std::size_t maximumOwnedBytes, const CancellationToken &cancellation) const = 0;
    };

    /** @brief Owns a complete successor and relinquishes it on the owner safe point. */
    class NavigationRuntimeTileRebuildCandidate final {
    public:
        NavigationRuntimeTileRebuildCandidate(NavigationRuntimeTileRebuildCandidate &&) noexcept = default;
        NavigationRuntimeTileRebuildCandidate(const NavigationRuntimeTileRebuildCandidate &) = delete;
        NavigationRuntimeTileRebuildCandidate &operator=(const NavigationRuntimeTileRebuildCandidate &) = delete;

        /** @brief Inspects the detached tile closure. @return Borrowed tiles before publication. */
        [[nodiscard]] std::span<const std::shared_ptr<const NavigationCookedTile>> Tiles() const noexcept {
            return tiles_;
        }

        /** @brief Reports how many tile builds the complete successor required. @return Rebuilt tile count. */
        [[nodiscard]] std::size_t RebuiltTileCount() const noexcept {
            return rebuiltTiles_;
        }

        /** @brief Reports the conservative retained tile and provider charge for aggregate owner admission.
         * @return Owned storage reservation including shared tile pins.
         */
        [[nodiscard]] std::size_t ReservedBytes() const noexcept {
            return reservedBytes_;
        }

        /** @brief Rechecks authority and commits the complete candidate once at the owner safe point.
         * @param world Owner of the exact active predecessor and its read leases.
         * @param current Freshly observed source, origin, overlay and command authority.
         * @param cancellation Cancellation observed before commit.
         * @return Committed tile closure for the host's next rebuild, or a typed failure preserving the active world.
         */
        [[nodiscard]] Result<std::vector<std::shared_ptr<const NavigationCookedTile>>> PublishAtSafePoint(
            NavigationWorldLifecycle &world, const NavigationRuntimeRebuildFence &current, const CancellationToken &cancellation = {}) &&;

    private:
        friend class NavigationAssetSceneActivationParticipant;
        friend class NavigationRuntimeRebuildJobs;
        friend Result<NavigationRuntimeTileRebuildCandidate> PrepareRuntimeNavigationTileRebuild(
            const NavigationRuntimeTileRebuildRequest &, const INavigationMeshBuilder &, const INavigationRuntimeRebuildBackendFactory &,
            const CancellationToken &);

        /** @brief Takes ownership only after all detached builds and provider preparation succeed. */
        NavigationRuntimeTileRebuildCandidate(const NavigationRuntimeRebuildFence &expected, NavigationGeneration next,
                                              std::vector<std::shared_ptr<const NavigationCookedTile>> tiles,
                                              std::unique_ptr<INavigationQueryBackend> backend, std::size_t rebuiltTiles,
                                              std::size_t reservedBytes) noexcept;

        /** @brief Verifies the Scene owner still holds the exact tile content used by this rebuild. */
        [[nodiscard]] bool MatchesPredecessor(std::span<const std::shared_ptr<const NavigationCookedTile>> tiles) const noexcept;

        NavigationRuntimeRebuildFence expected_;
        NavigationGeneration next_;
        NavigationRuntimeRebuildReservation reservation_; /**< Destroyed after detached provider and tiles. */
        std::vector<std::shared_ptr<const NavigationCookedTile>> tiles_;
        std::unique_ptr<INavigationQueryBackend> backend_;
        std::size_t rebuiltTiles_{};
        std::size_t reservedBytes_{};
        Sha256Digest predecessorTiles_{};
        bool preparedByJob_{};
    };

    /** @brief Rebuilds changed dependency keys and prepares a complete immutable replacement without publishing it.
     * @param request Host-authorized geometry, closed layout, exact predecessor tiles and explicit resource limits.
     * @param builder Host-selected grounded builder.
     * @param factory Host-selected query materializer for nonempty output.
     * @param cancellation Token observed between bounded units and before returning the candidate.
     * @return Detached candidate or typed failure. No cache, asset or live topology is modified.
     * @note A changed tile containing authored links requires host-qualified reprojection. Legacy links fail closed.
     */
    [[nodiscard]] Result<NavigationRuntimeTileRebuildCandidate> PrepareRuntimeNavigationTileRebuild(
        const NavigationRuntimeTileRebuildRequest &request, const INavigationMeshBuilder &builder,
        const INavigationRuntimeRebuildBackendFactory &factory, const CancellationToken &cancellation = {});
}  // namespace Horo::Navigation
