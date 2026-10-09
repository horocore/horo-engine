#include "TerrainPayloadManifestInternal.h"
#include "TerrainTileCookCodec.h"

namespace Horo::Terrain::Detail {
    /** @copydoc WriteBounds */
    void WriteBounds(CanonicalWriter &writer, const TerrainPayloadBounds &bounds) {
        for (const auto value : bounds.minimum)
            writer.Double(value);
        for (const auto value : bounds.maximum)
            writer.Double(value);
    }

    /** @copydoc WriteRequirements */
    void WriteRequirements(CanonicalWriter &writer, const TerrainPayloadRequirements &requirements) {
        writer.Byte(static_cast<std::uint8_t>(requirements.visual));
        writer.Byte(static_cast<std::uint8_t>(requirements.collision));
        writer.Byte(static_cast<std::uint8_t>(requirements.navigation));
    }

    /** @brief Encodes exact artifact schema, integrity and neutral costs. */
    static void WriteArtifact(CanonicalWriter &writer, const TerrainPayloadArtifact &artifact) {
        writer.Unsigned(artifact.schema, 4);
        writer.Bytes(artifact.digest.bytes);
        writer.Unsigned(artifact.byteSize, 8);
        writer.Unsigned(artifact.decodedBytes, 8);
        writer.Unsigned(artifact.workItems, 8);
    }

    /** @copydoc WriteTile */
    void WriteTile(CanonicalWriter &writer, const TerrainTilePayloadEntry &entry) {
        writer.Bytes(SerializeTerrainTileId(entry.tile));
        writer.Unsigned(entry.samplesX, 4);
        writer.Unsigned(entry.samplesZ, 4);
        WriteBounds(writer, entry.bounds);
        for (const auto &seam : entry.seams)
            writer.Bytes(seam.bytes);
        WriteArtifact(writer, entry.samples);
        for (const auto &consumer : entry.consumers) {
            writer.Byte(consumer ? 1 : 0);
            if (consumer)
                WriteArtifact(writer, *consumer);
        }
        writer.Double(entry.maximumGeometricError);
        writer.Byte(entry.requiresSameLodNeighbors ? 1 : 0);
    }

    /** @copydoc WriteCluster */
    void WriteCluster(CanonicalWriter &writer, const FoliageClusterPayloadEntry &entry) {
        writer.Bytes(SerializeTerrainTileId(entry.tile));
        writer.Bytes(entry.type.Bytes());
        writer.Bytes(entry.cluster.Bytes());
        writer.Unsigned(entry.source.Value(), 8);
        writer.Unsigned(entry.definition.Value(), 8);
        writer.Unsigned(entry.placement.Value(), 8);
        for (const auto &digest : {entry.placementFingerprint, entry.placementDigest, entry.geometryDigest, entry.profileFingerprint})
            writer.Bytes(digest.bytes);
        for (const auto value : entry.bounds.minimum)
            writer.Unsigned(static_cast<std::uint64_t>(value), 8);
        for (const auto value : entry.bounds.maximum)
            writer.Unsigned(static_cast<std::uint64_t>(value), 8);
        writer.Unsigned(entry.geometryRadiusMillimeters, 4);
        writer.Unsigned(entry.instanceCount, 8);
        WriteArtifact(writer, entry.instances);
    }
}  // namespace Horo::Terrain::Detail
