#include "AnimationGraphCompilerInternal.h"

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        /** @brief Resolves exact absolute clip descriptors; synchronized members require cyclic loop traversal. */
        Result<GraphBlendSampleBinding> SampleBinding(const GraphBlendTreeNode &tree, const GraphBlendSample &sample,
                                                      GraphMemoryRequirements &memory, const AnimationGraphCompileContext &context) {
            using namespace AnimationErrors;
            if (memory.clipPlayers >= context.limits.clipPlayers || memory.blendMembers >= AnimationGraphHardLimits::BlendMembers)
                return Result<GraphBlendSampleBinding>::Failure(Failure(GraphLimitExceeded));
            const auto clip = std::ranges::find_if(context.clips, [&](const auto &value) {
                return value.Data().descriptor.id == sample.clip;
            });
            if (clip == context.clips.end() || clip->Data().descriptor.kind != AnimationClipKind::Absolute)
                return Result<GraphBlendSampleBinding>::Failure(Failure(GraphBindingMismatch));
            if (tree.sync.mode != GraphSyncMode::Independent && clip->Data().descriptor.wrapMode != AnimationWrapMode::Loop)
                return Result<GraphBlendSampleBinding>::Failure(Failure(GraphTypeMismatch));
            GraphBlendSampleBinding result;
            result.player = memory.clipPlayers++;
            result.member = memory.blendMembers++;
            // Public descriptor table and runtime clip table are canonical by clip identity.
            result.clip = static_cast<std::uint32_t>(std::ranges::count_if(context.clips, [&](const auto &other) {
                return other.Data().descriptor.id < sample.clip;
            }));
            result.coverage.assign(context.skeleton->Data().joints.size(), 0);
            for (const auto &track : clip->Data().tracks) {
                const auto &joints = context.skeleton->Data().joints;
                const auto joint = std::ranges::find(joints, track.joint, &SkeletonJoint::id);
                if (joint == joints.end())
                    return Result<GraphBlendSampleBinding>::Failure(Failure(GraphBindingMismatch));
                result.coverage[static_cast<std::size_t>(joint - joints.begin())] = 1;
            }
            return Result<GraphBlendSampleBinding>::Success(std::move(result));
        }
    }  // namespace

    /** @copydoc BindBlendTree */
    Result<void> BindBlendTree(const GraphBlendTreeNode &tree, GraphInstructionOccurrence &occurrence, GraphMemoryRequirements &memory,
                               const AnimationGraphCompileContext &context) {
        using namespace AnimationErrors;
        const auto joints = context.skeleton->Data().joints.size();
        if (memory.workingSlots[0] >= context.limits.poseSlots || joints > context.limits.poseTransforms ||
            memory.poseTransforms > context.limits.poseTransforms - joints)
            return Result<void>::Failure(Failure(GraphLimitExceeded, occurrence.source));
        const auto entries = static_cast<std::uint64_t>(tree.samples.size()) * joints;
        if (entries > context.limits.poseTransforms || memory.compositionEntries > context.limits.poseTransforms - entries)
            return Result<void>::Failure(Failure(GraphLimitExceeded, occurrence.source));
        occurrence.blendScratch = GraphWorkingSlot{GraphValueType::Pose, memory.workingSlots[0]++};
        memory.poseTransforms += joints;
        occurrence.blendTree = memory.blendTrees++;
        for (const auto &sample : tree.samples) {
            auto bound = SampleBinding(tree, sample, memory, context);
            if (bound.HasError())
                return Result<void>::Failure(bound.ErrorValue());
            memory.compositionEntries += bound.Value().coverage.size();
            occurrence.blendSamples.push_back(std::move(bound).Value());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Animation::GraphCompileDetail
