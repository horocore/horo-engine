#include "FoliagePaintProducer.h"

#include "TerrainBrushGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::Editor {
    namespace {
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Reject stale source and foreign workspace publications before producing any intent. */
        Result<void> AdmitDocument(const TerrainAuthoringDocument &document, const TerrainEditFence &fence) {
            if (document.IsClosed())
                return Fail<void>(TerrainEditErrors::Closed);
            const auto current = document.Fence();
            if (fence.session != current.session || fence.dataset != current.dataset)
                return Fail<void>(TerrainEditErrors::WrongDocument);
            if (fence.expectedRevision != current.expectedRevision || fence.capabilityRevision != current.capabilityRevision)
                return Fail<void>(TerrainEditErrors::StaleRevision);
            return Result<void>::Success();
        }

        /** @brief Require an actual captured type binding rather than selecting a default silently. */
        bool MatchesType(const FoliagePaletteItem *active) {
            if (!active)
                return false;
            return active->source.asset.IsValid() && active->source.type == active->definition.Data().type &&
                   active->source.revision == active->definition.Data().revision;
        }

        /** @brief Check transient transform/density policy independently of source-region admission. */
        bool ValidBrush(const FoliagePaintBrush &brush) {
            if (brush.mode > FoliagePaintMode::Erase || brush.densityPermille == 0 || brush.densityPermille > 1'000)
                return false;
            return std::isfinite(brush.scale) && brush.scale > 0 && std::isfinite(brush.offsetMeters) && std::isfinite(brush.yawRadians);
        }

        /** @brief Charge existing-ID lookup, complete brush membership and footprint work before allocating output. */
        Result<TerrainBrushFootprint> Prepare(const TerrainAuthoringDocument &document, const FoliagePaintBrush &brush,
                                              std::span<const TerrainSculptStamp> stamps, const FoliageToolLimits &limits) {
            if (!limits.maximumPlacementEdits || limits.maximumPlacementEdits > 4'096 ||
                document.Placements().size() > limits.maximumExistingPlacements || limits.maximumExistingPlacements > 65'536)
                return Fail<TerrainBrushFootprint>(TerrainEditErrors::LimitExceeded);
            const auto fixed = document.Placements().size() * (stamps.size() + 24U);
            return PrepareTerrainBrushFootprint(document.Source(), brush.radiusMeters, stamps, limits.stroke, 24, 1, fixed);
        }

        /** @brief Evaluate bounded physical-space discs without using display or native terrain state. */
        bool Inside(const Terrain::TerrainCanonicalSource &source, std::uint32_t x, std::uint32_t z, double radius,
                    std::span<const TerrainSculptStamp> stamps) {
            for (const auto &stamp : stamps) {
                const auto dx = (double(x) - stamp.sampleX) * source.coordinates.spacingX;
                const auto dz = (double(z) - stamp.sampleZ) * source.coordinates.spacingZ;
                if (std::hypot(dx, dz) <= radius)
                    return true;
            }
            return false;
        }

        /** @brief Resolve the actual world-space tile before converting to its finite stable coordinate domain. */
        Result<std::int32_t> WorldTile(double origin, double spacing, std::uint32_t sample, std::uint32_t tileCells) {
            const double position = origin + double(sample) * spacing;
            const double extent = double(tileCells) * spacing;
            if (!std::isfinite(position) || !std::isfinite(extent) || extent <= 0)
                return Fail<std::int32_t>(TerrainEditErrors::Invalid);
            const double tile = std::floor(position / extent);
            if (!std::isfinite(tile) || tile < std::numeric_limits<std::int32_t>::min() || tile > std::numeric_limits<std::int32_t>::max())
                return Fail<std::int32_t>(TerrainEditErrors::LimitExceeded);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(tile));
        }

        /** @brief Name one new authored placement from an explicitly issued gesture plus dataset-local sample. */
        Result<Terrain::FoliageInstanceId> Identity(const Terrain::TerrainCanonicalSource &source, const FoliagePaletteItem &active,
                                                    const FoliagePaintBrush &brush, std::uint32_t x, std::uint32_t z,
                                                    std::uint32_t tileCells) {
            std::array<std::byte, 32> key{};
            constexpr std::array<std::byte, 8> domain{std::byte{'a'}, std::byte{'u'}, std::byte{'t'}, std::byte{'h'},
                                                      std::byte{'o'}, std::byte{'r'}, std::byte{1},   std::byte{0}};
            std::ranges::copy(domain, key.begin());
            std::ranges::copy(brush.gestureKey, key.begin() + 8);
            for (std::size_t i = 0; i < 4; ++i) {
                key[24 + i] = std::byte(x >> (i * 8));
                key[28 + i] = std::byte(z >> (i * 8));
            }
            auto tileX = WorldTile(source.coordinates.originX, source.coordinates.spacingX, x, tileCells);
            if (tileX.HasError())
                return Result<Terrain::FoliageInstanceId>::Failure(tileX.ErrorValue());
            auto tileZ = WorldTile(source.coordinates.originZ, source.coordinates.spacingZ, z, tileCells);
            if (tileZ.HasError())
                return Result<Terrain::FoliageInstanceId>::Failure(tileZ.ErrorValue());
            Terrain::TerrainTileId tile{source.dataset, {tileX.Value(), tileZ.Value(), 0}};
            return Terrain::DeriveFoliageInstanceId(tile, active.source.type, key);
        }

        /** @brief Deterministic density thinning uses stable authored identity bits, never ambient random state. */
        bool DensityAllows(Terrain::FoliageInstanceId id, std::uint16_t density) {
            std::uint64_t seed{};
            for (auto byte : id.Bytes())
                seed = seed * 257 + byte;
            return seed % 1'000 < density;
        }

        /** @brief Respect the exact authored type scale policy before asking the document to accept a paint stroke. */
        bool ScaleAllowed(const FoliagePaletteItem &active, float scale) {
            const double permille = double(scale) * 1'000;
            return permille >= active.definition.Data().minimumScalePermille && permille <= active.definition.Data().maximumScalePermille;
        }

        /** @brief Paint always requires one chosen type and a nonzero explicitly issued gesture identity. */
        bool AdmitsPaint(const FoliagePaletteItem *active, const FoliagePaintBrush &brush) {
            if (!MatchesType(active))
                return false;
            return ScaleAllowed(*active, brush.scale) && brush.gestureKey != std::array<std::byte, 16>{};
        }

        /** @brief Append one exact new placement or reject a collision/finite output ceiling without changing the document. */
        Result<void> PaintSample(const TerrainAuthoringDocument &document, const FoliagePaletteItem &active, const FoliagePaintBrush &brush,
                                 std::uint32_t x, std::uint32_t z, std::uint32_t tileCells, const FoliageToolLimits &limits,
                                 std::vector<TerrainPlacementEdit> &edits) {
            const auto &source = document.Source();
            const auto index = std::size_t(z) * source.width + x;
            if (source.HasHoles() && source.holes[index])
                return Result<void>::Success();
            auto id = Identity(source, active, brush, x, z, tileCells);
            if (id.HasError())
                return Result<void>::Failure(id.ErrorValue());
            if (!DensityAllows(id.Value(), brush.densityPermille))
                return Result<void>::Success();
            TerrainAuthoredPlacement value{id.Value(), active.source.type, x, z, brush.offsetMeters, brush.scale, brush.yawRadians};
            const auto prior = document.Placements().find(value.id);
            if (prior != document.Placements().end()) {
                if (prior->second != value)
                    return Fail<void>(TerrainEditErrors::Invalid);
                return Result<void>::Success();
            }
            if (edits.size() >= limits.maximumPlacementEdits)
                return Fail<void>(TerrainEditErrors::LimitExceeded);
            edits.push_back({value.id, value});
            return Result<void>::Success();
        }

        /** @brief Build the full gesture in canonical Z/X order with cooperative row and sample checks. */
        Result<void> PaintRegion(const TerrainAuthoringDocument &document, const FoliagePaletteItem &active, const FoliagePaintBrush &brush,
                                 std::span<const TerrainSculptStamp> stamps, const TerrainPatchRect &rect, std::uint32_t tileCells,
                                 const FoliageToolLimits &limits, const CancellationToken &cancel,
                                 std::vector<TerrainPlacementEdit> &edits) {
            for (std::uint32_t z = rect.z; z < rect.z + rect.height; ++z) {
                for (std::uint32_t x = rect.x; x < rect.x + rect.width; ++x) {
                    if (cancel.IsCancellationRequested())
                        return Fail<void>(TerrainEditErrors::Cancelled);
                    if (!Inside(document.Source(), x, z, brush.radiusMeters, stamps))
                        continue;
                    if (auto result = PaintSample(document, active, brush, x, z, tileCells, limits, edits); result.HasError())
                        return result;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Shared bounded ID query for selection and erase, preserving map/identity order. */
        Result<std::vector<Terrain::FoliageInstanceId>> Query(const TerrainAuthoringDocument &document, const FoliagePaletteItem *active,
                                                              const FoliagePaintBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                              const FoliageToolLimits &limits, const CancellationToken &cancel) {
            std::vector<Terrain::FoliageInstanceId> ids;
            for (const auto &[id, value] : document.Placements()) {
                if (cancel.IsCancellationRequested())
                    return Fail<std::vector<Terrain::FoliageInstanceId>>(TerrainEditErrors::Cancelled);
                if (!brush.allTypes && value.type != active->source.type)
                    continue;
                if (!Inside(document.Source(), value.sampleX, value.sampleZ, brush.radiusMeters, stamps))
                    continue;
                if (ids.size() >= limits.maximumPlacementEdits)
                    return Fail<std::vector<Terrain::FoliageInstanceId>>(TerrainEditErrors::LimitExceeded);
                ids.push_back(id);
            }
            return Result<std::vector<Terrain::FoliageInstanceId>>::Success(std::move(ids));
        }

        /** @brief Admit captured owner/type and common gesture policy before any collection traversal. */
        Result<TerrainBrushFootprint> Admit(const TerrainAuthoringDocument &document, const TerrainEditFence &fence,
                                            const FoliagePaletteItem *active, const FoliagePaintBrush &brush,
                                            std::span<const TerrainSculptStamp> stamps, const FoliageToolLimits &limits,
                                            const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Fail<TerrainBrushFootprint>(TerrainEditErrors::Cancelled);
            if (auto valid = AdmitDocument(document, fence); valid.HasError())
                return Result<TerrainBrushFootprint>::Failure(valid.ErrorValue());
            if (!ValidBrush(brush))
                return Fail<TerrainBrushFootprint>(TerrainEditErrors::Invalid);
            if (!brush.allTypes && !MatchesType(active))
                return Fail<TerrainBrushFootprint>(TerrainEditErrors::CapabilityUnavailable);
            return Prepare(document, brush, stamps, limits);
        }
    }  // namespace

    /** @copydoc ProduceFoliagePaint */
    Result<TerrainEditOperation> ProduceFoliagePaint(const TerrainAuthoringDocument &document, const TerrainEditFence &fence,
                                                     TerrainEditOperationId operation, const FoliagePaletteItem *active,
                                                     const FoliagePaintBrush &brush, std::span<const TerrainSculptStamp> stamps,
                                                     std::uint32_t tileCells, const FoliageToolLimits &limits,
                                                     const CancellationToken &cancel) {
        auto admitted = Admit(document, fence, active, brush, stamps, limits, cancel);
        if (admitted.HasError())
            return Result<TerrainEditOperation>::Failure(admitted.ErrorValue());
        if (!operation.IsValid() || !tileCells)
            return Fail<TerrainEditOperation>(TerrainEditErrors::Invalid);
        TerrainEditOperation result{fence, operation, {}, {}};
        if (brush.mode == FoliagePaintMode::Erase) {
            auto ids = Query(document, active, brush, stamps, limits, cancel);
            if (ids.HasError())
                return Result<TerrainEditOperation>::Failure(ids.ErrorValue());
            for (auto id : ids.Value())
                result.placements.push_back({id, {}});
        } else {
            if (!AdmitsPaint(active, brush))
                return Fail<TerrainEditOperation>(TerrainEditErrors::Invalid);
            if (auto painted =
                    PaintRegion(document, *active, brush, stamps, admitted.Value().rect, tileCells, limits, cancel, result.placements);
                painted.HasError())
                return Result<TerrainEditOperation>::Failure(painted.ErrorValue());
        }
        return Result<TerrainEditOperation>::Success(std::move(result));
    }

    /** @copydoc SelectFoliageInBrush */
    Result<std::vector<Terrain::FoliageInstanceId>> SelectFoliageInBrush(const TerrainAuthoringDocument &document,
                                                                         const TerrainEditFence &fence, const FoliagePaletteItem *active,
                                                                         const FoliagePaintBrush &brush,
                                                                         std::span<const TerrainSculptStamp> stamps,
                                                                         const FoliageToolLimits &limits, const CancellationToken &cancel) {
        auto admitted = Admit(document, fence, active, brush, stamps, limits, cancel);
        if (admitted.HasError())
            return Result<std::vector<Terrain::FoliageInstanceId>>::Failure(admitted.ErrorValue());
        return Query(document, active, brush, stamps, limits, cancel);
    }
}  // namespace Horo::Editor
