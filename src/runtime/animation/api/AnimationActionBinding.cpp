#include "AnimationGraphCompilerInternal.h"

#include <utility>

namespace Horo::Animation::GraphCompileDetail {
    /** @copydoc ValidateActionDependencies */
    Result<void> ValidateActionDependencies(const AnimationGraphProgram &program, const AnimationGraphCompileContext &context) {
        using namespace AnimationErrors;
        for (const auto &action : program.Actions()) {
            for (const auto &entry : action.mask.joints)
                if (std::ranges::find(context.skeleton->Data().joints, entry.joint, &SkeletonJoint::id) ==
                    context.skeleton->Data().joints.end())
                    return Result<void>::Failure(Failure(GraphBindingMismatch));
            for (const auto &section : action.sequence.sections) {
                if (const auto admission = Admission(context); admission.HasError())
                    return admission;
                const auto clip = std::ranges::find_if(context.clips, [&](const auto &value) {
                    return value.Data().descriptor.id == section.clip;
                });
                if (clip == context.clips.end() || clip->Data().descriptor.kind != AnimationClipKind::Absolute ||
                    section.end.ticks > clip->Data().descriptor.duration.ticks)
                    return Result<void>::Failure(Failure(GraphBindingMismatch));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc FinalizeActionPlan */
    Result<BoundPlan> FinalizeActionPlan(Result<BoundPlan> plan, const AnimationGraphProgram &program,
                                         const AnimationGraphCompileContext &context) {
        if (plan.HasError())
            return plan;
        auto owned = std::move(plan).Value();
        auto &memory = owned.memory;
        const auto slots = static_cast<std::uint32_t>(program.ActionSlots().size());
        if (slots > context.limits.clipPlayers || memory.clipPlayers > context.limits.clipPlayers - slots)
            return Result<BoundPlan>::Failure(Failure(AnimationErrors::GraphLimitExceeded));
        memory.actionSlots = slots;
        memory.clipPlayers += slots;
        return Result<BoundPlan>::Success(std::move(owned));
    }
}  // namespace Horo::Animation::GraphCompileDetail
