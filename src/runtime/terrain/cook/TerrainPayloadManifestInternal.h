#pragma once

#include "Horo/Terrain/TerrainPayloadManifest.h"

namespace Horo::Terrain::Detail {
    class CanonicalWriter;
    /** @brief Writes inclusive finite bounds. @param writer Admitted sink. @param bounds Exact envelope. */
    void WriteBounds(CanonicalWriter &writer, const TerrainPayloadBounds &bounds);
    /** @brief Writes explicit consumer requirements. @param writer Admitted sink. @param requirements Captured policy. */
    void WriteRequirements(CanonicalWriter &writer, const TerrainPayloadRequirements &requirements);
    /** @brief Writes HTPM v1 tile membership. @param writer Admitted sink. @param entry Verified descriptor. */
    void WriteTile(CanonicalWriter &writer, const TerrainTilePayloadEntry &entry);
    /** @brief Writes HTPM v1 cluster provenance. @param writer Admitted sink. @param entry Verified descriptor. */
    void WriteCluster(CanonicalWriter &writer, const FoliageClusterPayloadEntry &entry);
    /** @brief Tests the shared same-source, non-wrapping generation successor invariant.
     * @param current Exact retained predecessor provenance.
     * @param candidate Proposed detached successor provenance.
     * @return True only for monotonic revisions without unversioned source drift.
     */
    [[nodiscard]] bool PayloadManifestSuccessor(const TerrainPayloadProvenance &current, const TerrainPayloadProvenance &candidate);
}  // namespace Horo::Terrain::Detail
