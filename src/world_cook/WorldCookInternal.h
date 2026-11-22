#pragma once

#include "Horo/WorldCook/WorldCookPublication.h"

namespace Horo::WorldCook::Detail {
    /** @brief Returns the exact optional Navigation membership in a validated source capture. */
    [[nodiscard]] const CellNavigationBinding *FindNavigationCell(const SourceCapture &source, WorldStreaming::StreamingCellId cell);
    /** @brief Rejects duplicate, out-of-order, or foreign cell memberships before writing or admitting artifacts. */
    [[nodiscard]] Result<void> ValidateNavigationCells(const SourceCapture &source);

    /** @brief Detached complete provider outputs, with no filesystem or cache publication authority. */
    struct EncodedWorld final {
        WorldStreaming::EncodedWorldIndex index;
        std::vector<std::byte> dependencies;
        std::vector<WorldStreaming::EncodedStreamingCell> cells;
    };

    /** @brief Reconstructs the existing sole manifest authority from parsed metadata and independently verified hard edges. */
    [[nodiscard]] Result<WorldStreaming::CookedWorldIndexManifest> MakeManifest(
        const WorldStreaming::WorldPartitionDescriptor &partition, std::span<const WorldStreaming::CookedWorldCellManifestEntry> cells,
        std::span<const WorldStreaming::WorldDependencyCellProjection> dependencies, const PublicationLimits &limits);

    /** @brief Produces canonical artifacts exclusively through the owning Scene and WorldStreaming writers. */
    [[nodiscard]] Result<EncodedWorld> EncodeWorld(const SourceCapture &source, const Runtime::SceneCellCookReport &prepared,
                                                   const PublicationLimits &limits, const CancellationToken &cancellation);

    /** @brief Copies verified base envelopes and wraps generated payloads in canonical Assets envelopes. */
    [[nodiscard]] Result<Assets::AssetCookGenerationContents> PrepareInventory(const SourceCapture &source, const ReleaseCapture &release,
                                                                               const EncodedWorld &world, const PublicationLimits &limits,
                                                                               const CancellationToken &cancellation);

    /** @brief Validates complete exact package membership and provider-owned nonspatial content evidence. */
    [[nodiscard]] Result<void> ValidateInventory(const SourceCapture &source, const ReleaseCapture &release,
                                                 const Assets::AssetCookGenerationContents &inventory, const PublicationLimits &limits);

    /** @brief Validates all borrowed raw outputs and provider envelopes before copying or durable staging. */
    [[nodiscard]] Result<void> ValidateReleaseCoverage(const SourceCapture &source, const ReleaseCapture &release,
                                                       const ArtifactViews &artifacts, const PublicationLimits &limits,
                                                       const CancellationToken &cancellation);
}  // namespace Horo::WorldCook::Detail
