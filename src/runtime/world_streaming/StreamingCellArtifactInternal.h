#pragma once

#include "Horo/WorldStreaming/StreamingCellCandidate.h"

namespace Horo::WorldStreaming::Detail {
    /** @brief Detached parser storage; the header retains no span across moves. */
    struct ParsedCellArtifact final {
        StreamingCellHeaderView header;
        std::vector<StreamingCellPayloadHeader> payloads;
    };

    /** @brief Validates bounded canonical bytes without invoking providers or mutating runtime state. */
    [[nodiscard]] Result<ParsedCellArtifact> ParseCellArtifactBytes(std::span<const std::byte> artifact,
                                                                    const StreamingCellCandidateContext &context,
                                                                    const Sha256Digest &expectedHash,
                                                                    const CancellationToken &cancellation);
}  // namespace Horo::WorldStreaming::Detail
