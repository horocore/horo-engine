#pragma once

/** @file StreamingCellEncoding.h
 * @brief Bounded canonical HOROCELL production from already cooked provider-owned bytes.
 */
#include "Horo/WorldStreaming/StreamingCellCandidate.h"

namespace Horo::WorldStreaming {
    /** @brief Borrowed immutable provider output; no provider invocation or native serialization occurs here. */
    struct StreamingCellPayloadBytes final {
        StreamingCellProvider provider{};
        StreamingCellPayloadRequirement requirement{StreamingCellPayloadRequirement::Required};
        std::uint32_t version{};
        std::span<const std::byte> bytes;
    };

    /** @brief Mandatory aggregate bounds and explicitly selected encoding capability. */
    struct StreamingCellEncodingLimits final {
        std::uint32_t maximumPayloads{};
        std::size_t maximumArtifactBytes{};
        std::size_t maximumDecodedBytes{};
        StreamingCellCompression compression{StreamingCellCompression::None};
    };

    /** @brief Complete owned artifact and matching manifest integrity facts, independent of caller storage. */
    struct EncodedStreamingCell final {
        std::vector<std::byte> bytes;
        CookedWorldCellManifestEntry integrity;
    };

    /**
     * @brief Encodes one immutable cell in canonical ADR-023 version-one layout.
     * @param cell Exact validated manifest cell identity.
     * @param payloads Complete provider outputs; order is canonicalized and duplicate providers are rejected.
     * @param limits Mandatory byte/count bounds and explicit codec; this producer supports None only.
     * @param cancellation Borrowed observer checked throughout bounded copying and hashing.
     * @return Complete artifact with matching CRC/SHA facts, or existing typed cell invalid/capacity/unsupported/cancelled error.
     * @details Requires exactly one required CoreEcs payload. Source bytes are never retained or modified. Unsupported
     * codecs are rejected rather than substituted. Padding, reserved fields and digest-zeroing follow ADR-023;
     * no live world, provider, registry or filesystem is touched. Hard dependencies remain with the manifest owner.
     * @throws std::bad_alloc on bounded ownership allocation; no output or external state is published.
     */
    [[nodiscard]] Result<EncodedStreamingCell> EncodeStreamingCellArtifact(StreamingCellId cell,
                                                                           std::span<const StreamingCellPayloadBytes> payloads,
                                                                           const StreamingCellEncodingLimits &limits,
                                                                           const CancellationToken &cancellation = {});
}  // namespace Horo::WorldStreaming
