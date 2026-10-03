#include "SequenceFrameBlend.h"

#include "Horo/Cinematic/SequenceEvaluationErrors.h"

#include <algorithm>
#include <cmath>

namespace Horo::Cinematic {
    namespace {
        /** @brief Computes the weaker of the two directional edge weights. */
        [[nodiscard]] float FrameBlendWeight(const SequencePlayerSnapshot &player, const SequenceTime position,
                                             const SequenceFrameScratch &scratch) noexcept {
            if (scratch.blendBaselines.empty())
                return 1.0F;
            const SequenceTime fromStart = player.rate.numerator < 0 ? player.duration - position : position;
            const SequenceTime toEnd = player.rate.numerator < 0 ? position : player.duration - position;
            float weight = 1.0F;
            if (scratch.blendInDuration > 0 && fromStart < scratch.blendInDuration)
                weight = static_cast<float>(static_cast<double>(fromStart) / static_cast<double>(scratch.blendInDuration));
            if (scratch.blendOutDuration > 0 && toEnd < scratch.blendOutDuration)
                weight = std::min(weight, static_cast<float>(static_cast<double>(toEnd) / static_cast<double>(scratch.blendOutDuration)));
            return weight;
        }
    }  // namespace

    /** @copydoc ValidFrameBlendScratch */
    bool ValidFrameBlendScratch(const SequenceFrameScratch &scratch, const std::size_t trackCount) noexcept {
        return scratch.blendInDuration >= 0 && scratch.blendOutDuration >= 0 &&
               (scratch.blendBaselines.empty() || scratch.blendBaselines.size() == trackCount) &&
               ((scratch.blendInDuration == 0 && scratch.blendOutDuration == 0) || scratch.blendBaselines.size() == trackCount);
    }

    /** @copydoc SampleFrameValues */
    Result<void> SampleFrameValues(const std::span<const SequenceFrameTrackDescriptor> tracks, const SequencePlayerSnapshot &player,
                                   const SequenceTime position, const SequenceFrameScratch &scratch) {
        const float weight = FrameBlendWeight(player, position, scratch);
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            auto sampled = tracks[index].sample(tracks[index].context, position);
            if (sampled.HasError())
                return Result<void>::Failure(sampled.ErrorValue());
            float value = sampled.Value();
            if (!scratch.blendBaselines.empty()) {
                if (!std::isfinite(value) || !std::isfinite(scratch.blendBaselines[index]))
                    return Result<void>::Failure(MakeError(SequenceEvaluationErrors::Malformed));
                value = std::lerp(scratch.blendBaselines[index], value, weight);
                if (!std::isfinite(value))
                    return Result<void>::Failure(MakeError(SequenceEvaluationErrors::Malformed));
            }
            scratch.values[index] = {tracks[index].track, tracks[index].stage, value};
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Cinematic
