#include "WorldBakeInternal.h"

namespace Horo::Application::WorldBakeDetail {
    namespace W = WorldStreaming;
    namespace S = Runtime;

    /** @brief Encodes already qualified actual Scene payloads into canonical HOROCELL artifacts within one aggregate budget. */
    Result<void> Encode(const WorldBakeRequest &request, WorldBakeOutput &output, const CancellationToken &cancellation,
                        const std::function<void(float)> &progress) {
        std::size_t bytes{};
        for (std::size_t index = 0; index < output.cells.size(); ++index) {
            auto encoded = S::EncodeSceneCellPayload(*output.cells[index], request.limits.codec, cancellation);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            const auto payload = std::move(encoded).Value();
            const W::StreamingCellPayloadBytes provider{W::StreamingCellProvider::CoreEcs, W::StreamingCellPayloadRequirement::Required,
                                                        S::SceneCellPayloadCodecVersion, payload};
            const W::StreamingCellEncodingLimits limits{1, request.limits.maximumOutputBytes - bytes,
                                                        request.limits.codec.maximumEncodedBytes};
            auto artifact =
                W::EncodeStreamingCellArtifact(output.cells[index]->Identity().cell, std::span{&provider, 1}, limits, cancellation);
            if (artifact.HasError())
                return Result<void>::Failure(artifact.ErrorValue());
            bytes += artifact.Value().bytes.size();
            if (progress)
                progress(0.75F + 0.25F * static_cast<float>(index + 1) / static_cast<float>(output.cells.size()));
            output.artifacts.push_back(std::move(artifact).Value());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Application::WorldBakeDetail
