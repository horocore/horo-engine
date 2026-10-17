#include "WorldCookInternal.h"

#include <algorithm>
#include <array>

namespace Horo::WorldCook::Detail {
    /** @copydoc MakeManifest */
    Result<WorldStreaming::CookedWorldIndexManifest> MakeManifest(
        const WorldStreaming::WorldPartitionDescriptor &partition,
        const std::span<const WorldStreaming::CookedWorldCellManifestEntry> cells,
        const std::span<const WorldStreaming::WorldDependencyCellProjection> dependencies, const PublicationLimits &limits) {
        using namespace WorldStreaming;
        auto copied = WorldPartitionDescriptor::Create(partition.Version(), partition.Partition(), partition.Bounds(), partition.Grid(),
                                                       partition.Layers(), partition.Cells(), limits.index.partition);
        if (copied.HasError())
            return Result<CookedWorldIndexManifest>::Failure(copied.ErrorValue());
        std::vector<CookedWorldCellManifestCandidate> candidates;
        candidates.reserve(cells.size());
        for (const auto &cell : cells) {
            const auto edges = std::ranges::find(dependencies, cell.cell, &WorldDependencyCellProjection::cell);
            candidates.emplace_back(cell.cell, cell.uncompressedSize, cell.compressedSize, cell.payloadCrc32, cell.artifactHash,
                                    edges == dependencies.end() ? std::span<const StreamingCellId>{} : edges->hardTargets);
        }
        return CookedWorldIndexManifest::Create(std::move(copied).Value(), candidates, limits.index.cooked);
    }

    /** @copydoc EncodeWorld */
    Result<EncodedWorld> EncodeWorld(const SourceCapture &source, const Runtime::SceneCellCookReport &prepared,
                                     const PublicationLimits &limits, const CancellationToken &cancellation) {
        using namespace WorldStreaming;
        EncodedWorld output;
        std::vector<CookedWorldCellManifestEntry> metadata;
        output.cells.reserve(prepared.cells.size());
        metadata.reserve(prepared.cells.size());
        std::size_t bytes{};
        for (const auto &entry : prepared.cells) {
            if (cancellation.IsCancellationRequested())
                return Result<EncodedWorld>::Failure(MakeError(Errors::Cancelled));
            auto core = Runtime::EncodeSceneCellPayload(*entry.payload, limits.scene, cancellation);
            if (core.HasError())
                return Result<EncodedWorld>::Failure(core.ErrorValue());
            const std::array payloads{StreamingCellPayloadBytes{StreamingCellProvider::CoreEcs, StreamingCellPayloadRequirement::Required,
                                                                Runtime::SceneCellPayloadCodecVersion, core.Value()}};
            auto cell = EncodeStreamingCellArtifact(entry.payload->Identity().cell, payloads, limits.cell, cancellation);
            if (cell.HasError())
                return Result<EncodedWorld>::Failure(cell.ErrorValue());
            if (cell.Value().bytes.size() > limits.maximumTotalArtifactBytes - bytes)
                return Result<EncodedWorld>::Failure(MakeError(Errors::CapacityExceeded));
            bytes += cell.Value().bytes.size();
            metadata.push_back(cell.Value().integrity);
            output.cells.push_back(std::move(cell).Value());
        }
        // ADR-023 does not serialize hard edges. Capture the exact final index first, then bind its
        // dependency sibling; package admission reconstructs the same manifest with derived edges.
        auto manifest = MakeManifest(source.partition, metadata, {}, limits);
        if (manifest.HasError())
            return Result<EncodedWorld>::Failure(manifest.ErrorValue());
        auto index =
            EncodeWorldIndexArtifact(manifest.Value(), source.worldName, source.coordinates, source.volumes, limits.index, cancellation);
        if (index.HasError())
            return Result<EncodedWorld>::Failure(index.ErrorValue());
        auto decoded = DecodeWorldIndexArtifact(index.Value().bytes, limits.index, cancellation);
        if (decoded.HasError())
            return Result<EncodedWorld>::Failure(decoded.ErrorValue());
        auto dependencies = EncodeWorldDependencyArtifact(decoded.Value(), source.assignments, source.placements, source.dependencies,
                                                          limits.dependencies, cancellation);
        if (dependencies.HasError())
            return Result<EncodedWorld>::Failure(dependencies.ErrorValue());
        if (index.Value().bytes.size() > limits.maximumTotalArtifactBytes - bytes)
            return Result<EncodedWorld>::Failure(MakeError(Errors::CapacityExceeded));
        bytes += index.Value().bytes.size();
        if (dependencies.Value().size() > limits.maximumTotalArtifactBytes - bytes)
            return Result<EncodedWorld>::Failure(MakeError(Errors::CapacityExceeded));
        output.index = std::move(index).Value();
        output.dependencies = std::move(dependencies).Value();
        return Result<EncodedWorld>::Success(std::move(output));
    }
}  // namespace Horo::WorldCook::Detail
