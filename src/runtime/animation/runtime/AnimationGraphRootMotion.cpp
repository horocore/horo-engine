#include "AnimationGraphInstanceInternal.h"
#include "AnimationPoseComposition.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Returns the exact count of compiled pose writers, including reserved transition slots. */
        std::uint32_t PoseSlots(const AnimationGraphProgram &program) noexcept {
            return program.Memory().workingSlots[static_cast<std::size_t>(GraphValueType::Pose)];
        }
    }  // namespace

    /** @copydoc AdmitRootMotion */
    GraphInstanceFailure AdmitRootMotion(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context) noexcept {
        using enum GraphInstanceFailure;
        if (!context.extractRootMotion)
            return None;
        if (context.evaluationDomain != AnimationEvaluationDomain::AuthoritativeSimulation || assets.skeleton->RootJoints().size() != 1)
            return AdmissionRejected;
        const auto invalidTrack = [](const auto &clip) {
            return clip->Data().descriptor.hasRootMotion && !clip->HasRootMotionTrack();
        };
        return std::ranges::any_of(assets.clips, invalidTrack) ? BindingMismatch : None;
    }

    /** @copydoc RootMotionStorageBytes */
    std::uint64_t RootMotionStorageBytes(const AnimationGraphProgram &program, const GraphInstanceCreateContext &context) noexcept {
        if (!context.extractRootMotion)
            return 0;
        return PoseSlots(program) * sizeof(AnimationRootMotionDelta) + program.ActionSlots().size() * sizeof(GraphActionRootMotion) +
               (context.clipLimits.maximumBoundaryCrossings + 2ULL) * sizeof(AnimationClipTraversalSegment);
    }

    /** @copydoc RootMotionOperations */
    std::uint64_t RootMotionOperations(const AnimationGraphProgram &program, const GraphInstanceCreateContext &context) noexcept {
        if (!context.extractRootMotion)
            return 0;
        const auto segments = context.clipLimits.maximumBoundaryCrossings + 2ULL;
        const auto searches = 2ULL * std::bit_width(context.clipLimits.maximumKeysPerTrack);
        const auto intervals = program.Memory().clipPlayers + (program.ActionSlots().empty() ? 0ULL : context.actions.transitionsPerTick);
        return intervals * segments * (searches + 16ULL) + PoseSlots(program) + 16ULL * program.Occurrences().size();
    }

    /** @copydoc PrepareRootMotion */
    void PrepareRootMotion(GraphInstanceState &state, const GraphInstanceCreateContext &context) {
        if (!context.extractRootMotion)
            return;
        const auto &joints = state.skeleton->Data().joints;
        const auto root = std::ranges::find(joints, state.skeleton->RootJoints().front(), &SkeletonJoint::id);
        state.rootMotion.joint = static_cast<std::size_t>(root - joints.begin());
        state.rootMotion.poses.resize(PoseSlots(*state.program));
        state.rootMotion.actions.resize(state.program->ActionSlots().size());
        state.rootMotion.trace.resize(context.clipLimits.maximumBoundaryCrossings + 2ULL);
    }

    /** @copydoc TraverseRootMotion */
    AnimationClipOutcome<AnimationClipRootMotion> TraverseRootMotion(GraphInstanceState &state, const AnimationClipAsset &clip,
                                                                     AnimationClipCursor cursor, AnimationTimeDelta delta,
                                                                     AnimationClipRegion region) {
        if (!state.rootMotion.trace.empty() && clip.Data().descriptor.hasRootMotion)
            return clip.ExtractRootMotionPrepared(cursor, delta, region, state.rootMotion.trace, state.clipLimits);
        const auto traversed = clip.TraverseRegionPrepared(cursor, delta, region, state.clipLimits);
        if (traversed.HasError())
            return AnimationClipOutcome<AnimationClipRootMotion>::Failure(traversed.ErrorValue());
        return AnimationClipOutcome<AnimationClipRootMotion>::Success({traversed.Value(), {}});
    }

    /** @copydoc BlendRootMotion */
    AnimationRootMotionDelta BlendRootMotion(AnimationRootMotionDelta base, AnimationRootMotionDelta overlay, float weight) noexcept {
        return {{std::lerp(base.translation.x, overlay.translation.x, weight), std::lerp(base.translation.y, overlay.translation.y, weight),
                 std::lerp(base.translation.z, overlay.translation.z, weight)},
                Math::Slerp(base.rotation, overlay.rotation, weight)};
    }

    /** @copydoc AppendRootMotionDelta */
    bool AppendRootMotionDelta(AnimationRootMotionDelta &base, AnimationRootMotionDelta delta) noexcept {
        base.translation += base.rotation.Rotate(delta.translation);
        base.rotation = (base.rotation * delta.rotation).Normalized();
        return Math::IsFinite(base.translation);
    }

    /** @copydoc RootLayerWeight */
    float RootLayerWeight(std::size_t joint, const PoseCompositionInput &composition) noexcept {
        if (!composition.coverage.empty() && composition.coverage[joint] == 0)
            return 0.0F;
        const auto mask = composition.mask.empty() ? 1.0F : composition.mask[joint];
        return composition.weight * mask;
    }

    /** @copydoc LayerRootMotion */
    GraphInstanceFailure LayerRootMotion(GraphInstanceState &state, GraphWorkingSlot output, GraphWorkingSlot base,
                                         GraphWorkingSlot overlay, GraphWorkingSlot reference,
                                         const PoseCompositionInput &composition) noexcept {
        auto &motion = state.rootMotion.poses;
        if (motion.empty())
            return GraphInstanceFailure::None;
        const auto weight = RootLayerWeight(state.rootMotion.joint, composition);
        if (composition.mode == GraphBlendMode::Normalized) {
            motion[output.index] = BlendRootMotion(motion[base.index], motion[overlay.index], weight);
            return GraphInstanceFailure::None;
        }
        const auto transform = [](AnimationRootMotionDelta delta) {
            return Math::Transform{delta.translation, delta.rotation};
        };
        const std::array basePose{transform(motion[base.index])};
        const std::array overlayPose{transform(motion[overlay.index])};
        const std::array referencePose{transform(motion[reference.index])};
        std::array<Math::Transform, 1> result{};
        const auto failure =
            ComposePose({.base = basePose, .overlay = overlayPose, .reference = referencePose, .mode = composition.mode, .weight = weight},
                        result);
        if (failure == GraphInstanceFailure::None)
            motion[output.index] = {result.front().translation, result.front().rotation};
        return failure;
    }
}  // namespace Horo::Animation::Detail
