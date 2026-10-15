#include "Horo/WorldStreaming/StreamingCellInspection.h"

#include "StreamingCellArtifactInternal.h"

namespace Horo::WorldStreaming {
    /** @copydoc StreamingCellInspection::StreamingCellInspection */
    StreamingCellInspection::StreamingCellInspection(StreamingCellHeaderView header,
                                                     std::vector<StreamingCellPayloadHeader> payloads) noexcept
        : header_(header), payloads_(std::move(payloads)) {
        header_.payloads = {};
    }

    /** @copydoc StreamingCellInspection::Header */
    StreamingCellHeaderView StreamingCellInspection::Header() const noexcept {
        auto view = header_;
        view.payloads = payloads_;
        return view;
    }

    /** @copydoc InspectStreamingCellArtifact */
    Result<StreamingCellInspection> InspectStreamingCellArtifact(const std::span<const std::byte> artifact,
                                                                 const Sha256Digest &expectedHash,
                                                                 const StreamingCellInspectionLimits &limits,
                                                                 const CancellationToken &cancellation) {
        if (!limits.maximumPayloads || !limits.maximumCompressedBytes || !limits.maximumUncompressedBytes || expectedHash == Sha256Digest{})
            return Result<StreamingCellInspection>::Failure(MakeError(WorldStreamingErrors::CellCandidateInvalid));
        auto parsed = Detail::ParseCellArtifactBytes(artifact, limits, expectedHash, cancellation);
        if (parsed.HasError())
            return Result<StreamingCellInspection>::Failure(parsed.ErrorValue());
        auto owned = std::move(parsed).Value();
        return Result<StreamingCellInspection>::Success(StreamingCellInspection{owned.header, std::move(owned.payloads)});
    }
}  // namespace Horo::WorldStreaming
