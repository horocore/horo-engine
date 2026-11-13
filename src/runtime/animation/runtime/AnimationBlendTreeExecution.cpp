#include "AnimationGraphInstanceInternal.h"
#include "AnimationPoseComposition.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Merges authored contribution coverage only for positive-weight sampled members. */
        void MergeCoverage(std::span<std::uint8_t> output, std::span<const std::uint8_t> sampled) noexcept {
            for (std::size_t joint = 0; joint < output.size(); ++joint)
                output[joint] = static_cast<std::uint8_t>(output[joint] != 0 || sampled[joint] != 0);
        }

        /** @brief Validates the completed normalized writer without changing the last-good publication. */
        GraphInstanceOutcome ValidOutput(std::span<const Math::Transform> output, const float accumulated) noexcept {
            if (accumulated <= 0.0F)
                return {GraphInstanceFailure::Malformed};
            for (const auto &transform : output)
                if (!ValidPoseTransform(transform))
                    return {GraphInstanceFailure::Malformed};
            return {};
        }
    }  // namespace

    /** @copydoc ExecuteGraphBlendTree */
    GraphInstanceOutcome ExecuteGraphBlendTree(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) {
        if (state.blends.trees[*occurrence.blendTree].frozenSource)
            return {};
        const auto output = state.Pose(occurrence.output);
        const auto scratch = state.Pose(*occurrence.blendScratch);
        const auto coverage = state.Coverage(occurrence.output);
        std::ranges::fill(coverage, 0);
        float accumulated = 0.0F;
        for (const auto &binding : occurrence.blendSamples) {
            const auto &member = state.blends.members[binding.member];
            const auto weight = member.observation.weight;
            if (weight <= 0.0F)
                continue;
            const auto &clip = *state.clips[binding.clip];
            const auto &descriptor = clip.Data().descriptor;
            const AnimationClipSampleContext sample{.skeleton = descriptor.skeleton,
                                                    .skeletonGeneration = descriptor.skeletonGeneration,
                                                    .clipGeneration = descriptor.generation,
                                                    .outputPose = scratch};
            if (const auto sampled = clip.SamplePrepared(member.traversal.sampleTime, sample); sampled.HasError())
                return {GraphInstanceFailure::ClipFailure, sampled.ErrorValue()};
            if (accumulated == 0.0F)
                std::ranges::copy(scratch, output.begin());
            else if (const auto failure =
                         ComposePose({.base = output, .overlay = scratch, .weight = weight / (accumulated + weight)}, output);
                     failure != GraphInstanceFailure::None)
                return {failure};
            accumulated += weight;
            MergeCoverage(coverage, binding.coverage);
        }
        return ValidOutput(output, accumulated);
    }
}  // namespace Horo::Animation::Detail
