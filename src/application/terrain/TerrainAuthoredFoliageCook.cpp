#include "TerrainAuthoringCookInternal.h"
#include "TerrainAuthoredFoliageGrid.h"

#include <algorithm>
#include <bit>
#include <map>

namespace Horo::Application::TerrainCookDetail {
    namespace {
        using Regions = std::map<Terrain::TerrainTileId, FoliageTileRegion>;
        struct GroupKey final {
            Terrain::TerrainTileId tile;
            Terrain::FoliageTypeId type;
            auto operator<=>(const GroupKey &) const = default;
        };
        using Groups = std::map<GroupKey, std::vector<Terrain::TerrainAuthoredPlacement>>;

        Result<Regions> TileRegions(const Terrain::CookedTerrainTileSet &tiles) {
            using Return = Result<Regions>;
            if (tiles.tiles.empty() || tiles.tiles.front().id.tile.lod != 0)
                return Return::Failure(MakeError(Terrain::FoliagePlacementCookErrors::InvalidInput));
            const auto first = tiles.tiles.front().id.tile;
            Regions regions;
            for (const auto &tile : tiles.tiles) {
                if (tile.id.tile.lod != 0) continue;
                const auto x = std::int64_t{tile.id.tile.x} - first.x;
                const auto z = std::int64_t{tile.id.tile.z} - first.z;
                if (x < 0 || z < 0)
                    return Return::Failure(MakeError(Terrain::FoliagePlacementCookErrors::InvalidInput));
                const auto beginX = std::uint64_t(x) * tiles.profile.interiorQuads;
                const auto beginZ = std::uint64_t(z) * tiles.profile.interiorQuads;
                if (beginX >= tiles.sourceWidth || beginZ >= tiles.sourceHeight)
                    return Return::Failure(MakeError(Terrain::FoliagePlacementCookErrors::InvalidInput));
                regions.emplace(tile.id, FoliageTileRegion{tile.id, static_cast<std::uint32_t>(beginX), static_cast<std::uint32_t>(beginZ),
                    tile.samplesX, tile.samplesZ});
            }
            return Return::Success(std::move(regions));
        }

        /** @brief Each sample has one authored placement owner; the dataset's closed far edge belongs to its last real tile. */
        bool InRegion(const Terrain::TerrainAuthoredPlacement &placement, const FoliageTileRegion &region,
                       const Terrain::TerrainCanonicalSource &source) noexcept {
            const auto endX = region.x + region.width - 1, endZ = region.z + region.height - 1;
            const bool x = placement.sampleX >= region.x &&
                (placement.sampleX < endX || (endX == source.width - 1 && placement.sampleX == endX));
            const bool z = placement.sampleZ >= region.z &&
                (placement.sampleZ < endZ || (endZ == source.height - 1 && placement.sampleZ == endZ));
            return x && z;
        }

        /** @brief Admit all grid conversion, grouping comparisons and retained sealed headers before allocation/traversal. */
        Result<std::uint64_t> WorkBudget(const Capture &capture, const Regions &regions, const TerrainAuthoringCookProfile &profile) {
            constexpr std::uint64_t MaximumPredicateChecks = 16'777'216;
            const auto maximum = profile.foliage.maximumPredicateChecks;
            if (maximum == 0 || maximum > MaximumPredicateChecks ||
                capture.source.foliage.placements.size() > profile.foliage.maximumInstances)
                return Result<std::uint64_t>::Failure(MakeError(Terrain::FoliagePlacementCookErrors::LimitExceeded));
            std::uint64_t visits{};
            for (const auto &[id, region] : regions) {
                const auto gridVisits = std::uint64_t{region.width} * region.height * 32;
                if (gridVisits > maximum - visits)
                    return Result<std::uint64_t>::Failure(MakeError(Terrain::FoliagePlacementCookErrors::LimitExceeded));
                visits += gridVisits;
            }
            const auto perPlacement = regions.size() * 12 + std::bit_width(capture.source.foliage.placements.size()) * 8 + 128;
            if (capture.source.foliage.placements.size() > (maximum - visits) / perPlacement)
                return Result<std::uint64_t>::Failure(MakeError(Terrain::FoliagePlacementCookErrors::LimitExceeded));
            return Result<std::uint64_t>::Success(maximum - visits - capture.source.foliage.placements.size() * perPlacement);
        }

        Result<Groups> GroupPlacements(const Capture &capture, const Regions &regions, const CancellationToken &cancellation) {
            using Return = Result<Groups>;
            Groups groups;
            for (const auto &placement : capture.source.foliage.placements) {
                if (cancellation.IsCancellationRequested()) return Return::Failure(MakeError(Terrain::FoliagePlacementCookErrors::Cancelled));
                const auto region = std::ranges::find_if(regions, [&](const auto &entry) { return InRegion(placement, entry.second, capture.source.source); });
                if (region == regions.end()) return Return::Failure(MakeError(Terrain::FoliagePlacementCookErrors::InvalidInput));
                groups[{region->first, placement.type}].push_back(placement);
            }
            return Return::Success(std::move(groups));
        }

        /** @brief Charge repeated canonical grid validation/fingerprinting and worst-case inter-instance predicates across all proofs. */
        Result<void> GroupBudget(const Groups &groups, const Regions &regions, std::uint64_t remaining) {
            for (const auto &[key, placements] : groups) {
                const auto &region = regions.at(key.tile);
                const auto grid = std::uint64_t{region.width} * region.height * 8;
                if (grid > remaining) return Result<void>::Failure(MakeError(Terrain::FoliagePlacementCookErrors::LimitExceeded));
                remaining -= grid;
                const auto count = std::uint64_t{placements.size()};
                const auto perPlacement = count * 2 + std::bit_width(count) * 8 + 128;
                if (count > remaining / perPlacement)
                    return Result<void>::Failure(MakeError(Terrain::FoliagePlacementCookErrors::LimitExceeded));
                remaining -= count * perPlacement;
            }
            return Result<void>::Success();
        }

        Result<Terrain::FoliageTypeDefinition> Definition(const Capture &capture, Terrain::FoliageTypeId type,
            const TerrainAuthoringCookProfile &profile) {
            using Return = Result<Terrain::FoliageTypeDefinition>;
            const auto found = std::ranges::find(capture.definitions, type, [](const auto &entry) { return entry.definition.type; });
            if (found == capture.definitions.end()) return Return::Failure(MakeError(Terrain::FoliagePlacementCookErrors::InvalidInput));
            return Terrain::FoliageTypeDefinition::Create(found->definition, profile.configuration,
                {Terrain::FoliageDefinitionCapabilityBit<Terrain::FoliageDefinitionCapability::CpuCulling>});
        }

        /** @brief Hole suppression affects only derived placements; source IDs/transforms remain losslessly authored for restore/undo. */
        std::vector<Terrain::TerrainAuthoredPlacement> VisiblePlacements(std::span<const Terrain::TerrainAuthoredPlacement> placements,
            const FoliageTileRegion &region, const FoliageTileGrid &grid) {
            std::vector<Terrain::TerrainAuthoredPlacement> visible;
            visible.reserve(placements.size());
            for (const auto &placement : placements) {
                const auto index = std::size_t(placement.sampleZ - region.z) * region.width + placement.sampleX - region.x;
                if (!grid.samples[index].hole) visible.push_back(placement);
            }
            return visible;
        }

        Result<Terrain::CookedFoliagePlacement> CookGroup(const Capture &capture, const TerrainAuthoringCookProfile &profile,
            const GroupKey &key, const FoliageTileRegion &region, const FoliageTileGrid &grid,
            std::span<const Terrain::TerrainAuthoredPlacement> placements, const CancellationToken &cancellation) {
            using Return = Result<Terrain::CookedFoliagePlacement>;
            auto definition = Definition(capture, key.type, profile);
            if (definition.HasError()) return Return::Failure(std::move(definition).ErrorValue());
            auto visible = VisiblePlacements(placements, region, grid);
            Terrain::FoliagePlacementCookRequest target{grid.view, std::move(definition).Value(),
                {Terrain::FoliageDefinitionCapabilityBit<Terrain::FoliageDefinitionCapability::CpuCulling>},
                {.version = 1, .cellsPerCluster = 16}, profile.foliage,
                Terrain::TerrainContentRevision::Create(capture.source.source.revision.Value()).Value(),
                profile.artifacts.tiles.targetDigest, profile.artifacts.tiles.toolchainDigest,
                profile.artifacts.tiles.tier, capture.source.layers ? capture.source.layers->digest : Sha256Digest{}};
            return Terrain::CookAuthoredFoliagePlacement({target, visible, region.x, region.z}, cancellation);
        }

        Result<std::vector<Terrain::CookedFoliagePlacement>> CookGroups(const Capture &capture, const Regions &regions,
            const Groups &groups, const TerrainAuthoringCookProfile &profile, const CancellationToken &cancellation) {
            using Return = Result<std::vector<Terrain::CookedFoliagePlacement>>;
            std::vector<Terrain::CookedFoliagePlacement> result;
            result.reserve(groups.size());
            std::optional<FoliageTileGrid> grid;
            Terrain::TerrainTileId active;
            for (const auto &[key, placements] : groups) {
                const auto &region = regions.at(key.tile);
                if (!grid || active != key.tile) {
                    auto prepared = BuildFoliageGrid(capture.source.source, region, cancellation);
                    if (prepared.HasError()) return Return::Failure(std::move(prepared).ErrorValue());
                    grid.emplace(std::move(prepared).Value());
                    active = key.tile;
                }
                auto cooked = CookGroup(capture, profile, key, region, *grid, placements, cancellation);
                if (cooked.HasError()) return Return::Failure(std::move(cooked).ErrorValue());
                result.push_back(std::move(cooked).Value());
            }
            return Return::Success(std::move(result));
        }
    }

    /** @copydoc CookAuthoredFoliage */
    Result<std::vector<Terrain::CookedFoliagePlacement>> CookAuthoredFoliage(const Capture &capture,
        const Terrain::CookedTerrainTileSet &tiles, const TerrainAuthoringCookProfile &profile,
        const CancellationToken &cancellation) {
        using Return = Result<std::vector<Terrain::CookedFoliagePlacement>>;
        if (capture.source.foliage.placements.empty()) return Return::Success({});
        if (auto verified = Terrain::VerifyCookedTerrainTiles(tiles, cancellation); verified.HasError())
            return Return::Failure(std::move(verified).ErrorValue());
        auto regions = TileRegions(tiles);
        if (regions.HasError()) return Return::Failure(std::move(regions).ErrorValue());
        auto budget = WorkBudget(capture, regions.Value(), profile);
        if (budget.HasError()) return Return::Failure(std::move(budget).ErrorValue());
        auto groups = GroupPlacements(capture, regions.Value(), cancellation);
        if (groups.HasError()) return Return::Failure(std::move(groups).ErrorValue());
        if (auto admitted = GroupBudget(groups.Value(), regions.Value(), budget.Value()); admitted.HasError())
            return Return::Failure(std::move(admitted).ErrorValue());
        return CookGroups(capture, regions.Value(), groups.Value(), profile, cancellation);
    }
}
