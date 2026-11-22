#include "MountedWorldHost.h"
#include "ProjectSceneNavigationComposition.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <set>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    namespace {
        using Publication = Result<std::shared_ptr<const Runtime::SceneGroupPublication>>;
        using Content = Result<std::optional<Nav::LoadedNavMeshAsset>>;

        /** @brief Retains detached selected tiles until the exact Scene command commits or rolls back. */
        class CellNavigationOwner final : public Runtime::SceneGroupPublication {
        public:
            CellNavigationOwner(Nav::NavigationAssetSceneActivationParticipant &owner,
                                const Nav::NavigationWorldActivationDescriptor &predecessor, Nav::NavigationGeneration next,
                                std::optional<Nav::LoadedNavMeshAsset> content, std::uint64_t surfaceGeneration)
                : owner_(owner), predecessor_(predecessor), next_(next), content_(std::move(content)),
                  surfaceGeneration_(surfaceGeneration) {}

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Runtime::EntityRef>, bool) const override {
                if (scene.RuntimeId().value != predecessor_.scene.Value())
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(Nav::NavigationErrors::StaleSnapshot));
                if (!content_)
                    return owner_.PrepareStreamedPublication(predecessor_, next_, {});
                const std::array surfaces{Nav::NavigationStreamedSurface{*content_, 0, surfaceGeneration_}};
                return owner_.PrepareStreamedPublication(predecessor_, next_, surfaces);
            }

        private:
            Nav::NavigationAssetSceneActivationParticipant &owner_;
            Nav::NavigationWorldActivationDescriptor predecessor_;
            Nav::NavigationGeneration next_;
            std::optional<Nav::LoadedNavMeshAsset> content_;
            std::uint64_t surfaceGeneration_;
        };

        /** @brief Rejects a foreign Scene, profile, definition or stale cook before admitting requested tile storage. */
        Result<void> CollectCellTiles(const MountedWorldNavigationCell &cell, const ProjectSceneNavigationSelection &selected,
                                      const Nav::NavigationAssetProvenance &base, std::set<Nav::NavigationBakeTileKey> &keys) {
            if (cell.selection.definition != selected.definition || cell.selection.cookedEnvelope != base.cookedContentDigest)
                return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
            for (const auto &key : cell.selection.tiles) {
                if (key.scope != selected.surface.scope || key.surface != selected.surface.surface || key.profile != selected.profile)
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
                keys.insert(key);
                if (keys.size() > selected.policy.Capacities().maximumResidentTiles)
                    return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
            }
            return Result<void>::Success();
        }

        /** @brief Loads only the union needed by the proposed resident cell set against the active asset identity. */
        Content PrepareCellContent(const Runtime::RuntimeSceneView &scene, std::span<const MountedWorldNavigationCell> cells,
                                   ProjectSceneNavigationWorld &world, const ProjectSceneNavigationSelection &selected) {
            if (cells.empty())
                return Content::Success(std::nullopt);
            const auto provenance = world.assets->ActiveAssetProvenance();
            const auto base = std::ranges::find(provenance, selected.definition, &Nav::NavigationAssetProvenance::id);
            if (base == provenance.end() || base->registryRevision != scene.AssetRegistryRevision())
                return Content::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
            std::set<Nav::NavigationBakeTileKey> keys;
            for (const auto &cell : cells)
                if (auto collected = CollectCellTiles(cell, selected, *base, keys); collected.HasError())
                    return Content::Failure(collected.ErrorValue());
            const std::vector ordered(keys.begin(), keys.end());
            const auto type = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName);
            if (type.HasError())
                return Content::Failure(type.ErrorValue());
            const auto perTile = static_cast<std::size_t>(selected.policy.Capacities().maximumBytesPerResidentTile);
            auto loaded = Nav::LoadNavMeshAsset({selected.definition, type.Value()}, base->registryRevision, cells.front().envelope,
                                                selected.target, *world.cache,
                                                {.limits = {.maximumBytesPerTile = perTile},
                                                 .scope = selected.surface.scope,
                                                 .selectedTiles = ordered,
                                                 .retainTileBytes = false});
            if (loaded.HasError())
                return Content::Failure(loaded.ErrorValue());
            if (loaded.Value().cookedContentDigest != base->cookedContentDigest || loaded.Value().partitions.size() != 1)
                return Content::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
            if (std::ranges::all_of(loaded.Value().partitions.front().tiles, [](const auto &tile) {
                return tile->Topology().IsEmpty();
            }))
                return Content::Success(std::nullopt);
            return Content::Success(std::move(loaded).Value());
        }

        /** @brief Issues an exact generation for this Scene command even if it is subsequently cancelled. */
        Publication PreparePublication(const Runtime::RuntimeSceneView &scene, std::span<const MountedWorldNavigationCell> desired,
                                       ProjectSceneNavigationWorld &world, const ProjectSceneNavigationSelection &selected,
                                       std::uint64_t &lastIssued) {
            const auto lifecycle = world.assets->ActiveWorld();
            if (!lifecycle)
                return Publication::Failure(MakeError(Nav::NavigationErrors::NoNavigationData));
            const auto predecessor = lifecycle->ActiveDescriptor();
            if (predecessor.HasError())
                return Publication::Failure(predecessor.ErrorValue());
            lastIssued = std::max(lastIssued, predecessor.Value().topology.Value());
            if (lastIssued == std::numeric_limits<std::uint64_t>::max())
                return Publication::Failure(MakeError(Nav::NavigationErrors::GenerationExhausted));
            auto content = PrepareCellContent(scene, desired, world, selected);
            if (content.HasError())
                return Publication::Failure(content.ErrorValue());
            const auto next = Nav::NavigationGeneration::Create(++lastIssued).Value();
            return Publication::Success(std::make_shared<CellNavigationOwner>(*world.assets, predecessor.Value(), next,
                                                                              std::move(content).Value(), selected.surfaceGeneration));
        }
    }  // namespace

    /** @copydoc BindMountedWorldNavigation */
    Result<void> BindMountedWorldNavigation(MountedWorld &mount, ProjectSceneNavigationWorld &world,
                                            std::shared_ptr<const ProjectSceneNavigationSelection> selection) {
        if (!mount.HasLiveNavigationContent() || !world.assets || !world.cache || !selection || !selection->surfaceGeneration)
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapabilityUnavailable));
        try {
            return mount.BindLiveNavigation(
                [&world, selected = std::move(selection),
                 lastIssued = std::uint64_t{}](const Runtime::RuntimeSceneView &scene,
                                               std::span<const MountedWorldNavigationCell> desired) mutable {
                return PreparePublication(scene, desired, world, *selected, lastIssued);
            },
                [&world] {
                const auto lifecycle = world.assets->ActiveWorld();
                if (!lifecycle)
                    return Result<bool>::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
                static_cast<void>(lifecycle->CollectRetired());
                return Result<bool>::Success(lifecycle->RetiredCount() == 0);
            });
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Application::Internal
