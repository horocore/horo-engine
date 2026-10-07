#include "Horo/Editor/TerrainAuthoringDocument.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace Horo::Editor {
    namespace TerrainEditErrors {
        const ErrorCodeDescriptor Invalid{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.invalid"}, ErrorSeverity::Error,
                                          "Invalid terrain source or edit."};
        const ErrorCodeDescriptor WrongDocument{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.wrong_document"},
                                                ErrorSeverity::Error, "The terrain session or dataset was replaced."};
        const ErrorCodeDescriptor StaleRevision{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.stale_revision"},
                                                ErrorSeverity::Error, "The terrain source revision advanced."};
        const ErrorCodeDescriptor CapabilityUnavailable{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.capability_unavailable"},
                                                        ErrorSeverity::Error,
                                                        "Terrain editing permission or capability revision is unavailable."};
        const ErrorCodeDescriptor LimitExceeded{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.limit_exceeded"},
                                                ErrorSeverity::Error, "Terrain edit exceeds its finite transaction or history ceiling."};
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.cancelled"}, ErrorSeverity::Info,
                                            "Terrain edit was cancelled before publication."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.closed"}, ErrorSeverity::Error,
                                         "Terrain document no longer admits work."};
        const ErrorCodeDescriptor Exhausted{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.exhausted"}, ErrorSeverity::Error,
                                            "Terrain revision or content state identity is exhausted."};
        const ErrorCodeDescriptor HistoryEmpty{ErrorDomainId{"editor.terrain"}, ErrorCode{"editor.terrain.history_empty"},
                                               ErrorSeverity::Error, "No terrain history entry is available."};
    }  // namespace TerrainEditErrors

    namespace {
        /** @brief Produces a typed transaction failure without publishing owner state. */
        template <typename T> Result<T> Failed(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Checks finite nonzero ceilings against fixed engine work/allocation caps. */
        bool ValidLimits(const TerrainEditLimits &limits) noexcept {
            return limits.maximumPatches > 0 && limits.maximumPatches <= 4'096 && limits.maximumSamples > 0 &&
                   limits.maximumSamples <= Terrain::TerrainDescriptorHardLimits::WorkItems && limits.maximumPlacementEdits > 0 &&
                   limits.maximumPlacementEdits <= 65'536 && limits.maximumTransactionBytes > 0 &&
                   limits.maximumTransactionBytes <= Terrain::TerrainDescriptorHardLimits::StagingBytes && limits.maximumHistoryBytes > 0 &&
                   limits.maximumHistoryBytes <= Terrain::TerrainDescriptorHardLimits::StagingBytes && limits.maximumHistoryItems > 0 &&
                   limits.maximumHistoryItems <= 4'096 && limits.maximumPlacements > 0 && limits.maximumPlacements <= 1'048'576 &&
                   limits.maximumDirtyTiles > 0 && limits.maximumDirtyTiles <= 65'536;
        }

        /** @brief Recognizes only the bounded projected-coordinate provenance accepted by the importer. */
        bool ValidProjectedCrs(std::string_view crs) noexcept {
            return crs.starts_with("EPSG:") && crs.size() >= 6 && crs.size() <= 16 && std::ranges::all_of(crs.substr(5), [](const char c) {
                return c >= '0' && c <= '9';
            });
        }

        /** @brief Checks finite meter-space provenance without selecting a projection or format fallback. */
        bool ValidCoordinates(const Terrain::TerrainSourceCoordinates &coordinates, std::uint32_t width, std::uint32_t height) noexcept {
            switch (coordinates.space) {
                case Terrain::TerrainCoordinateSpace::LocalMeters:
                    if (!coordinates.projectedCrs.empty())
                        return false;
                    break;
                case Terrain::TerrainCoordinateSpace::ProjectedMeters:
                    if (!ValidProjectedCrs(coordinates.projectedCrs))
                        return false;
                    break;
                default:
                    return false;
            }
            return std::isfinite(coordinates.originX) && std::isfinite(coordinates.originZ) && std::isfinite(coordinates.spacingX) &&
                   std::isfinite(coordinates.spacingZ) && coordinates.spacingX > 0 && coordinates.spacingZ > 0 &&
                   std::isfinite(coordinates.heightScale) && coordinates.heightScale > 0 && std::isfinite(coordinates.heightOffset) &&
                   std::isfinite(coordinates.maximumPrecisionError) && coordinates.maximumPrecisionError >= 0 &&
                   std::isfinite(coordinates.originX + (width - 1U) * coordinates.spacingX) &&
                   std::isfinite(coordinates.originZ + (height - 1U) * coordinates.spacingZ);
        }

        /** @brief Verifies canonical UNORM16 weight sums for every complete authored pixel. */
        bool ValidWeights(const std::vector<std::uint16_t> &weights, std::uint8_t layers) noexcept {
            if (layers == 0)
                return weights.empty();
            for (std::size_t pixel = 0; pixel < weights.size(); pixel += layers) {
                std::uint32_t sum = 0;
                for (std::size_t layer = 0; layer < layers; ++layer)
                    sum += weights[pixel + layer];
                if (sum != 65'535)
                    return false;
            }
            return true;
        }

        /** @brief Checks externally supplied canonical storage before it becomes editor-owned. */
        bool ValidSource(const Terrain::TerrainCanonicalSource &source) noexcept {
            if (!source.dataset.IsValid() || !source.sourceAsset.IsValid() || !source.revision.IsValid() || !source.capability.IsValid() ||
                source.width < 2 || source.height < 2 || source.width > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
                source.height > Terrain::TerrainDescriptorHardLimits::SamplesPerAxis ||
                source.layerCount > Terrain::TerrainDescriptorHardLimits::LayersPerTile)
                return false;
            const std::uint64_t count = static_cast<std::uint64_t>(source.width) * source.height;
            if (count > Terrain::TerrainDescriptorHardLimits::WorkItems / (1U + source.layerCount + (!source.holes.empty() ? 1U : 0U)) ||
                source.heightsMeters.size() != count || source.weights.size() != count * source.layerCount ||
                (!source.holes.empty() && source.holes.size() != count))
                return false;
            if (!ValidCoordinates(source.coordinates, source.width, source.height))
                return false;
            for (const float value : source.heightsMeters)
                if (!std::isfinite(value))
                    return false;
            for (const auto value : source.holes)
                if (value > 1)
                    return false;
            return ValidWeights(source.weights, source.layerCount);
        }

        /** @brief Validates exact bounded channel payloads and source rectangle membership. */
        bool ValidPatch(const TerrainRasterPatch &patch, const Terrain::TerrainCanonicalSource &source) noexcept {
            const auto &r = patch.rect;
            if (r.width == 0 || r.height == 0 || r.x >= source.width || r.z >= source.height || r.width > source.width - r.x ||
                r.height > source.height - r.z)
                return false;
            const std::uint64_t count = static_cast<std::uint64_t>(r.width) * r.height;
            if (patch.heightsMeters.empty() && patch.weights.empty() && patch.holes.empty())
                return false;
            if ((!patch.heightsMeters.empty() && patch.heightsMeters.size() != count) ||
                (!patch.weights.empty() && (source.layerCount == 0 || patch.weights.size() != count * source.layerCount)) ||
                (!patch.holes.empty() && (source.holes.empty() || patch.holes.size() != count)))
                return false;
            for (float v : patch.heightsMeters)
                if (!std::isfinite(v))
                    return false;
            for (auto v : patch.holes)
                if (v > 1)
                    return false;
            return patch.weights.empty() || ValidWeights(patch.weights, source.layerCount);
        }

        /** @brief Identifies writes to the same canonical element, permitting distinct channels. */
        bool Conflicts(const TerrainRasterPatch &a, const TerrainRasterPatch &b) noexcept {
            const bool sameChannel = (!a.heightsMeters.empty() && !b.heightsMeters.empty()) || (!a.weights.empty() && !b.weights.empty()) ||
                                     (!a.holes.empty() && !b.holes.empty());
            return sameChannel && a.rect.x < b.rect.x + b.rect.width && b.rect.x < a.rect.x + a.rect.width &&
                   a.rect.z < b.rect.z + b.rect.height && b.rect.z < a.rect.z + a.rect.height;
        }

        /** @brief Validates authored placement membership and finite transform values. */
        bool ValidPlacement(const TerrainAuthoredPlacement &value, const Terrain::TerrainCanonicalSource &source) noexcept {
            return value.id.IsValid() && value.type.IsValid() && value.sampleX < source.width && value.sampleZ < source.height &&
                   std::isfinite(value.offsetMeters) && std::isfinite(value.scale) && value.scale > 0 && std::isfinite(value.yawRadians);
        }

        /** @brief Accumulates checked byte charge without overflow. */
        bool Charge(std::size_t &bytes, std::size_t add, std::size_t ceiling) noexcept {
            if (add > ceiling - bytes)
                return false;
            bytes += add;
            return true;
        }

        /** @brief Matches the existing tile cook world-address quantization and rejects signed overflow. */
        bool ValidTileOrigin(double origin, double spacing, std::uint32_t cells, std::uint32_t samples) noexcept {
            const double first = std::floor(origin / (spacing * cells));
            const std::uint64_t count = (static_cast<std::uint64_t>(samples) - 2) / cells + 1;
            return std::isfinite(first) && first >= std::numeric_limits<std::int32_t>::min() &&
                   first <= std::numeric_limits<std::int32_t>::max() &&
                   count - 1 <= static_cast<std::uint64_t>(static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) -
                                                           static_cast<std::int64_t>(first));
        }

        /** @brief Adds clipped one-sample dependency aprons and all shared seam tile owners. */
        bool AddDirtyTiles(std::set<Terrain::TerrainTileId> &tiles, const TerrainPatchRect &r,
                           const Terrain::TerrainCanonicalSource &source, std::uint32_t cells, std::size_t ceiling) {
            const std::uint32_t x0 = r.x == 0 ? 0 : r.x - 1;
            const std::uint32_t z0 = r.z == 0 ? 0 : r.z - 1;
            const std::uint32_t x1 = std::min(source.width - 1, r.x + r.width);
            const std::uint32_t z1 = std::min(source.height - 1, r.z + r.height);
            const auto originX = static_cast<std::int64_t>(std::floor(source.coordinates.originX / (source.coordinates.spacingX * cells)));
            const auto originZ = static_cast<std::int64_t>(std::floor(source.coordinates.originZ / (source.coordinates.spacingZ * cells)));
            const auto first = [cells](std::uint32_t value) {
                return value == 0 ? 0 : (value - 1) / cells;
            };
            const auto lastX = std::min(x1 / cells, (source.width - 2) / cells);
            const auto lastZ = std::min(z1 / cells, (source.height - 2) / cells);
            for (auto z = first(z0); z <= lastZ; ++z)
                for (auto x = first(x0); x <= lastX; ++x) {
                    tiles.insert({source.dataset, {static_cast<std::int32_t>(originX + x), static_cast<std::int32_t>(originZ + z), 0}});
                    if (tiles.size() > ceiling)
                        return false;
                }
            return true;
        }

        /** @brief Captures only the exact requested canonical regions. */
        TerrainRasterPatch Capture(const TerrainRasterPatch &after, const Terrain::TerrainCanonicalSource &source) {
            TerrainRasterPatch before{.rect = after.rect};
            before.heightsMeters.reserve(after.heightsMeters.size());
            before.weights.reserve(after.weights.size());
            before.holes.reserve(after.holes.size());
            for (std::uint32_t z = 0; z < after.rect.height; ++z)
                for (std::uint32_t x = 0; x < after.rect.width; ++x) {
                    const std::size_t index = static_cast<std::size_t>(z + after.rect.z) * source.width + x + after.rect.x;
                    if (!after.heightsMeters.empty())
                        before.heightsMeters.push_back(source.heightsMeters[index]);
                    if (!after.holes.empty())
                        before.holes.push_back(source.holes[index]);
                    if (!after.weights.empty())
                        for (std::size_t layer = 0; layer < source.layerCount; ++layer)
                            before.weights.push_back(source.weights[index * source.layerCount + layer]);
                }
            return before;
        }

        /** @brief Tests semantic equality after exact region capture, avoiding no-op history. */
        bool Equal(const TerrainRasterPatch &a, const TerrainRasterPatch &b) noexcept {
            return a.heightsMeters == b.heightsMeters && a.weights == b.weights && a.holes == b.holes;
        }

        /** @brief Transfers already allocated placement nodes; publication cannot allocate or fail. */
        void PublishPlacements(std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> &current,
                               std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> &staged,
                               const std::vector<TerrainPlacementSnapshot> &changes) noexcept {
            for (const auto &change : changes)
                current.erase(change.id);
            current.merge(staged);
        }

        /** @brief Immutable owner-thread admission facts; retained only for synchronous preparation. */
        struct EditContext final {
            const Terrain::TerrainCanonicalSource &source;
            const TerrainEditLimits &limits;
            TerrainDocumentStateId state;
            std::uint64_t nextState;
            std::uint32_t tileCells;
            const CancellationToken &cancellation;
        };

        using PlacementMap = std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement>;

        /** @brief Detached exact semantic record plus preallocated placement publication nodes. */
        struct PreparedEdit final {
            TerrainEditRecord record;
            PlacementMap staged;
        };

        /** @brief Admits complete channel shapes, overlap and byte/work bounds before source capture. */
        Result<std::size_t> AdmitOperation(const TerrainEditOperation &operation, const EditContext &context) {
            const auto ceiling = std::min(context.limits.maximumTransactionBytes, context.limits.maximumHistoryBytes);
            std::size_t bytes = sizeof(TerrainEditRecord), samples = 0;
            if (bytes > ceiling)
                return Failed<std::size_t>(TerrainEditErrors::LimitExceeded);
            // Charge payloads and duplicate snapshot metadata before capturing any source values.
            for (std::size_t i = 0; i < operation.patches.size(); ++i) {
                const auto &patch = operation.patches[i];
                const std::uint64_t count = static_cast<std::uint64_t>(patch.rect.width) * patch.rect.height;
                if (count > context.limits.maximumSamples - samples)
                    return Failed<std::size_t>(TerrainEditErrors::LimitExceeded);
                samples += static_cast<std::size_t>(count);
                const std::uint64_t payload = static_cast<std::uint64_t>(patch.heightsMeters.size()) * sizeof(float) +
                                              static_cast<std::uint64_t>(patch.weights.size()) * sizeof(std::uint16_t) + patch.holes.size();
                if (payload > ceiling / 2 ||
                    !Charge(bytes, 2 * sizeof(TerrainRasterPatch) + 2 * static_cast<std::size_t>(payload), ceiling))
                    return Failed<std::size_t>(TerrainEditErrors::LimitExceeded);
                if (!ValidPatch(patch, context.source))
                    return Failed<std::size_t>(TerrainEditErrors::Invalid);
                for (std::size_t j = 0; j < i; ++j)
                    if (Conflicts(patch, operation.patches[j]))
                        return Failed<std::size_t>(TerrainEditErrors::Invalid);
                if (context.cancellation.IsCancellationRequested())
                    return Failed<std::size_t>(TerrainEditErrors::Cancelled);
            }
            if (operation.placements.size() > (ceiling - bytes) / sizeof(TerrainPlacementSnapshot))
                return Failed<std::size_t>(TerrainEditErrors::LimitExceeded);
            bytes += operation.placements.size() * sizeof(TerrainPlacementSnapshot);
            return Result<std::size_t>::Success(bytes);
        }

        /** @brief Captures canonically ordered exact before/after regions and dependency closure. */
        Result<void> PrepareRasterSnapshots(const TerrainEditOperation &operation, const EditContext &context, TerrainEditRecord &record,
                                            std::set<Terrain::TerrainTileId> &dirtyTiles) {
            std::vector<const TerrainRasterPatch *> ordered;
            ordered.reserve(operation.patches.size());
            for (const auto &patch : operation.patches)
                ordered.push_back(&patch);
            std::ranges::sort(ordered, [](const auto *a, const auto *b) {
                if (a->rect != b->rect)
                    return a->rect < b->rect;
                const auto channels = [](const auto &patch) {
                    return (!patch.heightsMeters.empty() ? 1 : 0) + (!patch.weights.empty() ? 2 : 0) + (!patch.holes.empty() ? 4 : 0);
                };
                return channels(*a) < channels(*b);
            });
            for (const auto *input : ordered) {
                const auto &patch = *input;
                auto before = Capture(patch, context.source);
                if (Equal(before, patch))
                    continue;
                if (!AddDirtyTiles(dirtyTiles, patch.rect, context.source, context.tileCells, context.limits.maximumDirtyTiles))
                    return Failed<void>(TerrainEditErrors::LimitExceeded);
                record.invalidation.visual = true;
                record.invalidation.collision |= !patch.heightsMeters.empty() || !patch.holes.empty();
                record.invalidation.navigation |= !patch.heightsMeters.empty() || !patch.holes.empty();
                record.invalidation.foliage = true;
                record.before.push_back(std::move(before));
                record.after.push_back(patch);
            }
            return Result<void>::Success();
        }

        /** @brief Captures stable placement deltas, validates capacity and allocates publication nodes. */
        Result<PlacementMap> PreparePlacementSnapshots(const TerrainEditOperation &operation, const EditContext &context,
                                                       const PlacementMap &current, TerrainEditRecord &record,
                                                       std::set<Terrain::TerrainTileId> &dirtyTiles) {
            std::set<Terrain::FoliageInstanceId> placementIds;
            std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> staged;
            std::size_t placementCount = current.size();
            for (const auto &change : operation.placements) {
                if (!change.id.IsValid() || !placementIds.insert(change.id).second ||
                    (change.value && (change.value->id != change.id || !ValidPlacement(*change.value, context.source))))
                    return Failed<PlacementMap>(TerrainEditErrors::Invalid);
                const auto found = current.find(change.id);
                const std::optional<TerrainAuthoredPlacement> before = found == current.end() ? std::nullopt : std::optional{found->second};
                if (!before && !change.value)
                    return Failed<PlacementMap>(TerrainEditErrors::Invalid);
                if (before == change.value)
                    continue;
                if (!before)
                    ++placementCount;
                if (!change.value)
                    --placementCount;
                for (const auto &value : {before, change.value})
                    if (value && !AddDirtyTiles(dirtyTiles, {value->sampleX, value->sampleZ, 1, 1}, context.source, context.tileCells,
                                                context.limits.maximumDirtyTiles))
                        return Failed<PlacementMap>(TerrainEditErrors::LimitExceeded);
                if (change.value)
                    staged.emplace(change.id, *change.value);
                record.placements.push_back({change.id, before, change.value});
                record.invalidation.foliage = true;
            }
            std::ranges::sort(record.placements, {}, &TerrainPlacementSnapshot::id);
            if (placementCount > context.limits.maximumPlacements)
                return Failed<PlacementMap>(TerrainEditErrors::LimitExceeded);
            return Result<PlacementMap>::Success(std::move(staged));
        }

        /** @brief Freezes one admitted semantic record without mutating canonical storage or history. */
        Result<PreparedEdit> PrepareEdit(const TerrainEditOperation &operation, const EditContext &context, const PlacementMap &current) {
            auto admission = AdmitOperation(operation, context);
            if (admission.HasError())
                return Result<PreparedEdit>::Failure(admission.ErrorValue());
            PreparedEdit prepared;
            auto &record = prepared.record;
            record.operation = operation.operation;
            record.baseRevision = context.source.revision;
            record.beforeState = context.state;
            record.before.reserve(operation.patches.size());
            record.after.reserve(operation.patches.size());
            record.placements.reserve(operation.placements.size());
            std::set<Terrain::TerrainTileId> dirtyTiles;
            auto raster = PrepareRasterSnapshots(operation, context, record, dirtyTiles);
            if (raster.HasError())
                return Result<PreparedEdit>::Failure(raster.ErrorValue());
            auto placements = PreparePlacementSnapshots(operation, context, current, record, dirtyTiles);
            if (placements.HasError())
                return Result<PreparedEdit>::Failure(placements.ErrorValue());
            prepared.staged = std::move(placements).Value();
            if (record.after.empty() && record.placements.empty())
                return Result<PreparedEdit>::Success(std::move(prepared));
            auto bytes = admission.Value();
            if (!Charge(bytes, dirtyTiles.size() * sizeof(Terrain::TerrainTileId),
                        std::min(context.limits.maximumTransactionBytes, context.limits.maximumHistoryBytes)))
                return Failed<PreparedEdit>(TerrainEditErrors::LimitExceeded);
            auto revision = Terrain::AdvanceTerrainRevision(context.source.revision);
            if (revision.HasError() || context.nextState == std::numeric_limits<std::uint64_t>::max())
                return Failed<PreparedEdit>(TerrainEditErrors::Exhausted);
            record.afterState = TerrainDocumentStateId::Create(context.nextState).Value();
            record.committedRevision = revision.Value();
            record.dirtyTiles.assign(dirtyTiles.begin(), dirtyTiles.end());
            record.bytes = bytes;
            return Result<PreparedEdit>::Success(std::move(prepared));
        }
    }  // namespace

    /** @copydoc TerrainAuthoringDocument::Open */
    Result<TerrainAuthoringDocument> TerrainAuthoringDocument::Open(TerrainDocumentSessionId session,
                                                                    Terrain::TerrainCanonicalSource source, std::uint32_t tileCells,
                                                                    TerrainAuthoringCapability capability, TerrainEditLimits limits,
                                                                    std::vector<TerrainAuthoredPlacement> placements) {
        if (!session.IsValid() || !ValidLimits(limits) || !std::has_single_bit(tileCells) ||
            tileCells > Terrain::TerrainDescriptorHardLimits::TileInteriorQuads ||
            (capability != TerrainAuthoringCapability::ReadOnly && capability != TerrainAuthoringCapability::Edit) ||
            !ValidSource(source) || !ValidTileOrigin(source.coordinates.originX, source.coordinates.spacingX, tileCells, source.width) ||
            !ValidTileOrigin(source.coordinates.originZ, source.coordinates.spacingZ, tileCells, source.height))
            return Failed<TerrainAuthoringDocument>(TerrainEditErrors::Invalid);
        if (placements.size() > limits.maximumPlacements)
            return Failed<TerrainAuthoringDocument>(TerrainEditErrors::LimitExceeded);
        std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> initialPlacements;
        try {
            for (const auto &placement : placements) {
                if (!ValidPlacement(placement, source) || !initialPlacements.emplace(placement.id, placement).second)
                    return Failed<TerrainAuthoringDocument>(TerrainEditErrors::Invalid);
            }
        } catch (const std::bad_alloc &) {
            return Failed<TerrainAuthoringDocument>(TerrainEditErrors::LimitExceeded);
        }
        TerrainAuthoringDocument document;
        document.placements_ = std::move(initialPlacements);
        document.session_ = session;
        document.source_ = std::move(source);
        document.tileCells_ = tileCells;
        document.capability_ = capability;
        document.limits_ = limits;
        document.state_ = TerrainDocumentStateId::Create(1).Value();
        document.savedState_ = document.state_;
        return Result<TerrainAuthoringDocument>::Success(std::move(document));
    }

    /** @copydoc TerrainAuthoringDocument::Apply */
    void TerrainAuthoringDocument::Apply(const std::vector<TerrainRasterPatch> &patches) noexcept {
        for (const auto &patch : patches) {
            std::size_t pixel = 0;
            for (std::uint32_t z = 0; z < patch.rect.height; ++z)
                for (std::uint32_t x = 0; x < patch.rect.width; ++x, ++pixel) {
                    const std::size_t index = static_cast<std::size_t>(z + patch.rect.z) * source_.width + x + patch.rect.x;
                    if (!patch.heightsMeters.empty())
                        source_.heightsMeters[index] = patch.heightsMeters[pixel];
                    if (!patch.holes.empty())
                        source_.holes[index] = patch.holes[pixel];
                    if (!patch.weights.empty())
                        for (std::size_t layer = 0; layer < source_.layerCount; ++layer)
                            source_.weights[index * source_.layerCount + layer] = patch.weights[pixel * source_.layerCount + layer];
                }
        }
    }

    /** @copydoc TerrainAuthoringDocument::Execute */
    Result<TerrainEditChange> TerrainAuthoringDocument::Execute(const TerrainEditOperation &operation,
                                                                const CancellationToken &cancellation) {
        if (auto admitted = Admit(operation.fence, cancellation); admitted.HasError())
            return Result<TerrainEditChange>::Failure(admitted.ErrorValue());
        if (!operation.operation.IsValid() || (operation.patches.empty() && operation.placements.empty()))
            return Failed<TerrainEditChange>(TerrainEditErrors::Invalid);
        if (operation.patches.size() > limits_.maximumPatches || operation.placements.size() > limits_.maximumPlacementEdits)
            return Failed<TerrainEditChange>(TerrainEditErrors::LimitExceeded);
        try {
            auto result = PrepareEdit(operation, {source_, limits_, state_, nextState_, tileCells_, cancellation}, placements_);
            if (result.HasError())
                return Result<TerrainEditChange>::Failure(result.ErrorValue());
            auto prepared = std::move(result).Value();
            if (prepared.record.after.empty() && prepared.record.placements.empty())
                return Result<TerrainEditChange>::Success({operation.operation, source_.revision, state_, {}, {}, false, IsDirty()});
            return Publish(std::move(prepared.record), std::move(prepared.staged), cancellation);
        } catch (const std::bad_alloc &) {
            return Failed<TerrainEditChange>(TerrainEditErrors::LimitExceeded);
        }
    }

    /** @copydoc TerrainAuthoringDocument::Publish */
    Result<TerrainEditChange> TerrainAuthoringDocument::Publish(TerrainEditRecord record,
                                                                std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> staged,
                                                                const CancellationToken &cancellation) {
        TerrainEditChange receipt{record.operation,
                                  record.committedRevision,
                                  record.afterState,
                                  record.dirtyTiles,
                                  record.invalidation,
                                  true,
                                  record.afterState != savedState_};
        undo_.reserve(undo_.size() + 1);
        if (cancellation.IsCancellationRequested())
            return Failed<TerrainEditChange>(TerrainEditErrors::Cancelled);
        undo_.push_back(std::move(record));
        const auto &committed = undo_.back();
        Apply(committed.after);
        PublishPlacements(placements_, staged, committed.placements);
        source_.revision = committed.committedRevision;
        state_ = committed.afterState;
        ++nextState_;
        for (const auto &entry : redo_)
            historyBytes_ -= entry.bytes;
        redo_.clear();
        historyBytes_ += committed.bytes;
        while (undo_.size() > limits_.maximumHistoryItems || historyBytes_ > limits_.maximumHistoryBytes) {
            historyBytes_ -= undo_.front().bytes;
            undo_.erase(undo_.begin());
        }
        return Result<TerrainEditChange>::Success(std::move(receipt));
    }

    /** @copydoc TerrainAuthoringDocument::Replay */
    Result<TerrainEditChange> TerrainAuthoringDocument::Replay(const TerrainEditFence &fence, const CancellationToken &cancellation,
                                                               bool redo) {
        if (auto admitted = Admit(fence, cancellation); admitted.HasError())
            return Result<TerrainEditChange>::Failure(admitted.ErrorValue());
        auto &from = redo ? redo_ : undo_;
        if (from.empty())
            return Failed<TerrainEditChange>(TerrainEditErrors::HistoryEmpty);
        const auto revision = Terrain::AdvanceTerrainRevision(source_.revision);
        if (revision.HasError())
            return Failed<TerrainEditChange>(TerrainEditErrors::Exhausted);
        try {
            const auto &record = from.back();
            std::map<Terrain::FoliageInstanceId, TerrainAuthoredPlacement> staged;
            for (const auto &change : record.placements) {
                const auto &value = redo ? change.after : change.before;
                if (value)
                    staged.emplace(change.id, *value);
            }
            const auto state = redo ? record.afterState : record.beforeState;
            TerrainEditChange receipt{record.operation,    revision.Value(), state, record.dirtyTiles, record.invalidation, true,
                                      state != savedState_};
            auto &to = redo ? undo_ : redo_;
            to.reserve(to.size() + 1);
            if (cancellation.IsCancellationRequested())
                return Failed<TerrainEditChange>(TerrainEditErrors::Cancelled);
            to.push_back(std::move(from.back()));
            const auto &committed = to.back();
            Apply(redo ? committed.after : committed.before);
            PublishPlacements(placements_, staged, committed.placements);
            source_.revision = revision.Value();
            state_ = state;
            from.pop_back();
            return Result<TerrainEditChange>::Success(std::move(receipt));
        } catch (const std::bad_alloc &) {
            return Failed<TerrainEditChange>(TerrainEditErrors::LimitExceeded);
        }
    }

    /** @copydoc TerrainAuthoringDocument::Undo */
    Result<TerrainEditChange> TerrainAuthoringDocument::Undo(const TerrainEditFence &fence, const CancellationToken &cancellation) {
        return Replay(fence, cancellation, false);
    }

    /** @copydoc TerrainAuthoringDocument::Redo */
    Result<TerrainEditChange> TerrainAuthoringDocument::Redo(const TerrainEditFence &fence, const CancellationToken &cancellation) {
        return Replay(fence, cancellation, true);
    }

}  // namespace Horo::Editor
