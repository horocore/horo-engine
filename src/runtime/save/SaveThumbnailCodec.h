#pragma once

#include "Horo/Runtime/Save/SaveThumbnailArchive.h"

namespace Horo::Runtime::SaveThumbnailDetail {
    /** @brief Encodes fixed-size little-endian presentation-only provenance. @param artifact Valid CPU artifact.
     * @return 104-byte schema-1 metadata record. */
    [[nodiscard]] std::vector<std::byte> Encode(const SaveThumbnailArtifact &artifact);
    /** @brief Decodes bounded provenance and CPU image through the same capture admission.
     * @param metadata Exact fixed-size metadata. @param image Bounded PNG bytes.
     * @param publication Expected committed identity. @param limits Product ceilings.
     * @return Detached artifact or typed invalid/stale error. */
    [[nodiscard]] Result<std::shared_ptr<const SaveThumbnailArtifact>> Decode(std::span<const std::byte> metadata,
                                                                              std::vector<std::byte> image,
                                                                              const SaveSlotPublicationMetadata &publication,
                                                                              const SaveThumbnailLimits &limits);
}  // namespace Horo::Runtime::SaveThumbnailDetail
