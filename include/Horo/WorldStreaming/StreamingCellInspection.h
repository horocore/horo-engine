#pragma once

/** @file StreamingCellInspection.h
 * @brief Bounded canonical cell inspection without constructing a live streaming operation fence.
 */
#include "Horo/WorldStreaming/StreamingCellCandidate.h"

namespace Horo::WorldStreaming {
    /** @brief Explicit format inspection bounds; encoded-body bytes exclude the fixed 96-byte header. */
    struct StreamingCellInspectionLimits final {
        std::uint32_t maximumPayloads{};
        std::uint64_t maximumCompressedBytes{};
        std::uint64_t maximumUncompressedBytes{};
    };

    /** @brief Owned inert header/TOC projection from the same parser used by runtime candidate preparation. */
    class StreamingCellInspection final {
    public:
        /** @brief Returns a borrowed header view. @return View whose TOC span expires on owner movement or destruction. */
        [[nodiscard]] StreamingCellHeaderView Header() const noexcept;

    private:
        friend Result<StreamingCellInspection> InspectStreamingCellArtifact(std::span<const std::byte>, const Sha256Digest &,
                                                                            const StreamingCellInspectionLimits &,
                                                                            const CancellationToken &);
        /** @brief Captures verified scalar metadata and owned TOC rows. @param header Scalar facts. @param payloads Owned TOC. */
        StreamingCellInspection(StreamingCellHeaderView header, std::vector<StreamingCellPayloadHeader> payloads) noexcept;
        StreamingCellHeaderView header_;
        std::vector<StreamingCellPayloadHeader> payloads_;
    };

    /**
     * @brief Inspects canonical bytes against their expected complete artifact hash without fabricating runtime authority.
     * @param artifact Complete bytes, borrowed only until return.
     * @param expectedHash Exact canonical artifact digest captured from the admitted index.
     * @param limits Mandatory positive encoded, decoded and TOC ceilings.
     * @param cancellation Cooperative observer checked during hashing, parsing and CRC validation.
     * @return Owned scalar/TOC projection or the original typed parser error; no registration or activation occurs.
     * @details This shares the existing canonical parser with ParseStreamingCellArtifact. It does not admit a runtime
     * operation, verify package availability, invoke a provider or deserialize native objects. Consumers still compare
     * cell identity and integrity metadata to their exact index and validate provider-owned semantic bytes.
     * @throws std::bad_alloc on bounded TOC ownership allocation.
     */
    [[nodiscard]] Result<StreamingCellInspection> InspectStreamingCellArtifact(std::span<const std::byte> artifact,
                                                                               const Sha256Digest &expectedHash,
                                                                               const StreamingCellInspectionLimits &limits,
                                                                               const CancellationToken &cancellation = {});
}  // namespace Horo::WorldStreaming
