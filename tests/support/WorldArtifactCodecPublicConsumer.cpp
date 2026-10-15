#include "Horo/WorldStreaming/StreamingCellEncoding.h"
#include "Horo/WorldStreaming/StreamingCellInspection.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldDependencyArtifact.h"
#include "Horo/WorldStreaming/WorldIndexArtifact.h"

#include <array>

namespace {
    using namespace Horo::WorldStreaming;

    /** @brief Calls the public dependency writer and reader against an actual independently decoded index. */
    bool QualifyDependencies(const DecodedWorldIndex &index) {
        const auto &descriptor = index.descriptor;
        std::array<std::uint8_t, 16> page{};
        page.front() = 7;
        auto revision = WorldAuthoringRevision::Create(1);
        if (revision.HasError())
            return false;
        const std::array candidate{WorldSpatialAssignmentCandidate{{Horo::Assets::AssetId::FromBytes(page), 1},
                                                                   revision.Value(),
                                                                   descriptor.Bounds(),
                                                                   descriptor.Layers().front().id,
                                                                   0}};
        auto assignments = WorldSpatialAssignment::Create(descriptor, candidate, {1, 1, 1});
        if (assignments.HasError())
            return false;
        auto placements = WorldSpanningObjectPlan::Create(assignments.Value(), {}, {1, 1, 1});
        auto dependencies = WorldDependencyPlan::Create(assignments.Value(), {}, {1, 1, 2, 1});
        if (placements.HasError() || dependencies.HasError())
            return false;
        const WorldDependencyArtifactLimits limits{4096, 16384, 1, 1, 1, 1, 2, 1, 1, 4};
        auto encoded = EncodeWorldDependencyArtifact(index, assignments.Value(), placements.Value(), dependencies.Value(), limits);
        if (encoded.HasError())
            return false;
        auto decoded = DecodeWorldDependencyArtifact(encoded.Value(), index, limits);
        return decoded.HasValue() && decoded.Value().objects.size() == 1 && decoded.Value().cells.size() == 1 &&
               decoded.Value().cells.front().hardTargets.empty();
    }

    /** @brief Calls the real canonical index codecs above an actual encoded negative-coordinate cell. */
    bool QualifyIndex(const EncodedStreamingCell &cell) {
        std::array<std::uint8_t, 16> identity{};
        identity.front() = 3;
        auto partition = WorldPartitionId::Create(identity);
        auto grid = WorldCellQuantizationPolicy::Create({}, 100, {-1, -1, 0, 0, 0, 0}, 1);
        if (partition.HasError() || grid.HasError())
            return false;
        const auto chunk = Horo::Assets::AssetId::FromBytes(identity);
        const auto layer = cell.integrity.cell.layer;
        const std::array layers{WorldLayerDescriptor{layer, "base", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::Persistent, 1}};
        const std::array cells{WorldPartitionCellDescriptor{cell.integrity.cell, {chunk}}};
        const WorldPartitionBounds bounds{Horo::Math::WorldCoordinate64::FromMillimeters(-100, 0, 0),
                                          Horo::Math::WorldCoordinate64::FromMillimeters(-1, 99, 99)};
        auto descriptor = WorldPartitionDescriptor::Create({}, partition.Value(), bounds, grid.Value(), layers, cells, {1, 1, 4});
        if (descriptor.HasError())
            return false;
        const auto &integrity = cell.integrity;
        const std::array metadata{CookedWorldCellManifestCandidate{integrity.cell,
                                                                   integrity.uncompressedSize,
                                                                   integrity.compressedSize,
                                                                   integrity.payloadCrc32,
                                                                   integrity.artifactHash,
                                                                   {}}};
        auto manifest = CookedWorldIndexManifest::Create(std::move(descriptor).Value(), metadata, {1, 1, 1, 1024, 1024});
        if (manifest.HasError())
            return false;
        const WorldIndexArtifactLimits limits{4096, 64, 1, {1, 1, 4}, {1, 1, 1, 1024, 1024}};
        auto encoded = EncodeWorldIndexArtifact(manifest.Value(), "World", WorldIndexCoordinateSystem::RightHandedYUp, {}, limits);
        if (encoded.HasError())
            return false;
        auto decoded = DecodeWorldIndexArtifact(encoded.Value().bytes, limits);
        return decoded.HasValue() && QualifyDependencies(decoded.Value());
    }
}  // namespace

int main() {
    using namespace Horo::WorldStreaming;
    const auto layer = StreamingLayerId::Create(0);
    if (layer.HasError())
        return 1;
    const std::array bytes{std::byte{1}};
    const std::array payloads{
        StreamingCellPayloadBytes{StreamingCellProvider::CoreEcs, StreamingCellPayloadRequirement::Required, 1, bytes}};
    const auto encoded = EncodeStreamingCellArtifact({-1, 0, 0, 0, layer.Value()}, payloads, {1, 137, 1});
    if (encoded.HasError() || encoded.Value().bytes.size() != 137 || encoded.Value().integrity.cell.x != -1)
        return 2;
    const auto inspection = InspectStreamingCellArtifact(encoded.Value().bytes, encoded.Value().integrity.artifactHash, {1, 41, 1});
    if (inspection.HasError() || inspection.Value().Header().cell.x != -1)
        return 5;
    if (!QualifyIndex(encoded.Value()))
        return 4;
    static_assert(WorldPartitionSchemaVersion::CurrentMajor == 1);
    return WorldArtifactErrors::Invalid.code.Value().empty() ? 3 : 0;
}
