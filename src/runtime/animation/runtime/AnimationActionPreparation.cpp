#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <bit>

namespace Horo::Animation::Detail {
    /** @copydoc ValidActionLimits */
    bool ValidActionLimits(const GraphActionLimits &limits) noexcept {
        return limits.requests > 0 && limits.requests <= AnimationGraphHardLimits::ActionRequests &&
               limits.queuedPerSlot <= limits.requests && limits.transitionsPerTick > 0 &&
               limits.transitionsPerTick <= AnimationGraphHardLimits::ActionTransitions;
    }

    /** @copydoc ActionStorageBytes */
    std::uint64_t ActionStorageBytes(const AnimationGraphProgram &program, const GraphActionLimits &limits) noexcept {
        if (program.ActionSlots().empty())
            return 0;
        const std::uint64_t slots = program.ActionSlots().size();
        const std::uint64_t joints = program.SkeletonBinding().joints;
        std::uint64_t bytes =
            slots * (2 * sizeof(ActionSlotState) + 2 * sizeof(GraphActionSlotObservation)) + 4 * slots * joints * sizeof(Math::Transform);
        bytes += limits.requests * (2 * sizeof(ActionRecord) + 2 * sizeof(GraphActionObservation) + sizeof(std::uint32_t));
        for (const auto &action : program.Actions()) {
            bytes += sizeof(PreparedAction) + joints * sizeof(float);
            bytes += action.sequence.sections.size() * (sizeof(PreparedActionSection) + joints * sizeof(std::uint8_t));
            bytes += action.sequence.loops.size() * sizeof(PreparedActionLoop);
        }
        return bytes;
    }

    /** @copydoc ActionJointOperations */
    std::uint64_t ActionJointOperations(const GraphInstanceAssets &assets) noexcept {
        auto cost = 12ULL * assets.program->ActionSlots().size() * assets.program->SkeletonBinding().joints;
        for (const auto &action : assets.program->Actions())
            for (const auto &section : action.sequence.sections) {
                const auto clip = std::ranges::find_if(assets.clips, [&](const auto &value) {
                    return value->Data().descriptor.id == section.clip;
                });
                for (const auto &track : (*clip)->Data().tracks)
                    cost += 1 + std::bit_width(track.keys.size());
            }
        return cost;
    }

    /** @copydoc ActionControlOperations */
    std::uint64_t ActionControlOperations(const AnimationGraphProgram &program, const GraphActionLimits &limits) noexcept {
        if (program.ActionSlots().empty())
            return 0;
        return 16ULL * limits.requests + 64ULL * limits.transitionsPerTick * program.ActionSlots().size();
    }

    namespace {
        /** @brief Resolves one exact section descriptor and coverage at the cold owner
         * boundary. */
        PreparedActionSection Section(const GraphInstanceState &state, const GraphActionDefinition &action,
                                      const GraphActionSection &section) {
            PreparedActionSection result;
            result.clip = static_cast<std::uint32_t>(std::ranges::find_if(state.clips, [&](const auto &clip) {
                return clip->Data().descriptor.id == section.clip;
            }) - state.clips.begin());
            result.region = {section.begin, section.end, AnimationWrapMode::Once};
            if (section.next.IsValid())
                result.next = static_cast<std::uint32_t>(
                    std::ranges::find(action.sequence.sections, section.next, &GraphActionSection::id) - action.sequence.sections.begin());
            const auto &joints = state.skeleton->Data().joints;
            result.coverage.assign(joints.size(), 0);
            for (const auto &track : state.clips[result.clip]->Data().tracks) {
                const auto found = std::ranges::find(joints, track.joint, &SkeletonJoint::id);
                result.coverage[static_cast<std::size_t>(found - joints.begin())] = 1;
            }
            return result;
        }

        /** @brief Every section in an explicit nonoverlapping loop receives one exact
         * prepared owner index. */
        void Loops(PreparedAction &prepared, const GraphActionDefinition &action) {
            for (std::uint32_t i = 0; i < action.sequence.loops.size(); ++i) {
                const auto &loop = action.sequence.loops[i];
                const auto first = static_cast<std::uint32_t>(
                    std::ranges::find(action.sequence.sections, loop.first, &GraphActionSection::id) - action.sequence.sections.begin());
                const auto last = static_cast<std::uint32_t>(
                    std::ranges::find(action.sequence.sections, loop.last, &GraphActionSection::id) - action.sequence.sections.begin());
                prepared.loops.push_back({first, last});
                auto current = first;
                for (;;) {
                    prepared.sections[current].loop = i;
                    if (current == last)
                        break;
                    current = prepared.sections[current].next;
                }
            }
        }

        /** @brief Resolves dense mask and section links once; no dependency lookup
         * survives into the hot step. */
        PreparedAction Definition(const GraphInstanceState &state, const GraphActionDefinition &action) {
            PreparedAction result;
            result.slot = static_cast<std::uint32_t>(std::ranges::find(state.program->ActionSlots(), action.slot, &GraphActionSlot::id) -
                                                     state.program->ActionSlots().begin());
            result.entry =
                static_cast<std::uint32_t>(std::ranges::find(action.sequence.sections, action.sequence.entry, &GraphActionSection::id) -
                                           action.sequence.sections.begin());
            const auto &joints = state.skeleton->Data().joints;
            result.mask.assign(joints.size(), action.mask.defaultWeight);
            for (const auto &entry : action.mask.joints)
                result.mask[static_cast<std::size_t>(std::ranges::find(joints, entry.joint, &SkeletonJoint::id) - joints.begin())] =
                    entry.weight;
            result.sections.reserve(action.sequence.sections.size());
            result.loops.reserve(action.sequence.loops.size());
            for (const auto &section : action.sequence.sections)
                result.sections.push_back(Section(state, action, section));
            Loops(result, action);
            return result;
        }
    }  // namespace

    /** @copydoc PrepareActions */
    GraphInstanceFailure PrepareActions(GraphInstanceState &state, const GraphActionLimits &limits) {
        auto &work = state.actions;
        work.limits = limits;
        const auto count = state.program->ActionSlots().size();
        if (count == 0)
            return GraphInstanceFailure::None;
        work.records.resize(limits.requests);
        work.candidateRecords.resize(limits.requests);
        work.admissions.reserve(limits.requests);
        work.slots.resize(count);
        work.candidateSlots.resize(count);
        work.definitions.reserve(state.program->Actions().size());
        for (const auto &action : state.program->Actions())
            work.definitions.push_back(Definition(state, action));
        const auto poses = count * state.program->SkeletonBinding().joints;
        work.overlay.resize(poses);
        work.committedOverlay.resize(poses);
        work.frozen.resize(poses);
        work.committedFrozen.resize(poses);
        for (auto &publication : work.published)
            publication.resize(count);
        for (auto &completion : work.completions)
            completion.resize(limits.requests);
        return GraphInstanceFailure::None;
    }

    /** @copydoc ActionPlayer */
    std::uint32_t ActionPlayer(const GraphInstanceState &state, const std::uint32_t slot) noexcept {
        return state.program->Memory().clipPlayers - state.program->Memory().actionSlots + slot;
    }

    /** @copydoc ActionCurve */
    float ActionCurve(const GraphStateBlendCurve curve, const double fraction) noexcept {
        return EvaluateGraphBlendCurve(curve, std::clamp(fraction, 0.0, 1.0));
    }
}  // namespace Horo::Animation::Detail
