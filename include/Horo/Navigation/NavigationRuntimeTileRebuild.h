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
        friend Result<NavigationRuntimeTileRebuildCandidate> PrepareRuntimeNavigationTileRebuild(
            const NavigationRuntimeTileRebuildRequest &, const INavigationMeshBuilder &, const INavigationRuntimeRebuildBackendFactory &,
            const CancellationToken &);

        /** @brief Takes ownership only after all detached builds and provider preparation succeed. */
        NavigationRuntimeTileRebuildCandidate(const NavigationRuntimeRebuildFence &expected, NavigationGeneration next,
                                              std::vector<std::shared_ptr<const NavigationCookedTile>> tiles,
                                              std::unique_ptr<INavigationQueryBackend> backend, std::size_t rebuiltTiles,
                                              std::size_t reservedBytes) noexcept;

        NavigationRuntimeRebuildFence expected_;
        NavigationGeneration next_;
        std::vector<std::shared_ptr<const NavigationCookedTile>> tiles_;
        std::unique_ptr<INavigationQueryBackend> backend_;
        std::size_t rebuiltTiles_{};
        std::size_t reservedBytes_{};
    };

    /** @brief Rebuilds changed dependency keys and prepares a complete immutable replacement without publishing it.
     * @param request Host-authorized geometry, closed layout, exact predecessor tiles and explicit resource limits.
     * @param builder Host-selected grounded builder.
     * @param factory Host-selected query materializer for nonempty output.
     * @param cancellation Token observed between bounded units and before returning the candidate.
     * @return Detached candidate or typed failure. No cache, asset or live topology is modified.
     * @note A changed tile containing old traversal links requires separately qualified link reprojection.
     */
    [[nodiscard]] Result<NavigationRuntimeTileRebuildCandidate> PrepareRuntimeNavigationTileRebuild(
        const NavigationRuntimeTileRebuildRequest &request, const INavigationMeshBuilder &builder,
        const INavigationRuntimeRebuildBackendFactory &factory, const CancellationToken &cancellation = {});
}  // namespace Horo::Navigation
