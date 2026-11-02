#include "AnimationGraphInstanceInternal.h"
#include "AnimationPoseComposition.h"

#include <algorithm>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Borrows exactly one prepared complete slot pose from a fixed flat
         * allocation. */
        std::span<Math::Transform> Pose(GraphInstanceState &state, std::vector<Math::Transform> &values,
                                        const std::uint32_t slot) noexcept {
            const auto joints = state.program->SkeletonBinding().joints;
            return std::span{values}.subspan(static_cast<std::size_t>(slot) * joints, joints);
        }

        /** @brief Normalized masked authored coverage uses the same effective
         * contribution as the pose layer. */
        void Coverage(const GraphActionDefinition &action, const PreparedAction &prepared, const PreparedActionSection &section,
                      const float weight, std::span<std::uint8_t> output) noexcept {
            for (std::size_t i = 0; i < output.size(); ++i) {
                const auto effective = action.missingJoint == GraphMissingJointPolicy::PreserveBase && section.coverage[i] == 0
                                           ? 0.0F
                                           : weight * prepared.mask[i];
                output[i] =
                    static_cast<std::uint8_t>((effective < 1.0F && output[i] != 0) || (effective > 0.0F && section.coverage[i] != 0));
            }
        }

        /** @brief One action overlay uses the established pose compositor; there is no
         * alternate transform math. */
        GraphInstanceOutcome Layer(GraphInstanceState &state, const GraphActionDefinition &action, const PreparedAction &prepared,
                                   const std::uint32_t section, const std::span<const Math::Transform> overlay, const float weight,
                                   const GraphInstructionOccurrence &occurrence) {
            const auto output = state.Pose(occurrence.output);
            PoseCompositionInput input{.base = output,
                                       .overlay = overlay,
                                       .mask = prepared.mask,
                                       .mode = GraphBlendMode::Normalized,
                                       .weight = weight};
            if (action.missingJoint == GraphMissingJointPolicy::PreserveBase)
                input.coverage = prepared.sections[section].coverage;
            if (const auto failure = ComposePose(input, output); failure != GraphInstanceFailure::None)
                return {failure};
            Coverage(action, prepared, prepared.sections[section], weight, state.Coverage(occurrence.output));
            return {};
        }

        /** @brief Frozen outgoing overlays retain only their exact action mask while
         * the underlying base continues. */
        GraphInstanceOutcome Outgoing(GraphInstanceState &state, const std::uint32_t slotIndex,
                                      const GraphInstructionOccurrence &occurrence) {
            const auto &slot = state.actions.candidateSlots[slotIndex];
            if (slot.handoffAction == NoActionIndex)
                return {};
            const auto &action = state.program->Actions()[slot.handoffAction];
            const auto weight =
                action.blendOut.ticks == 0
                    ? 0.0F
                    : slot.handoffWeight *
                          (1.0F - ActionCurve(action.curve, static_cast<double>(slot.handoffElapsed) / action.blendOut.ticks));
            return Layer(state, action, state.actions.definitions[slot.handoffAction], slot.handoffSection,
                         Pose(state, state.actions.frozen, slotIndex), weight, occurrence);
        }
    }  // namespace

    /** @copydoc SampleActions */
    GraphInstanceOutcome SampleActions(GraphInstanceState &state) {
        auto &work = state.actions;
        for (std::uint32_t i = 0; i < work.candidateSlots.size(); ++i) {
            const auto &slot = work.candidateSlots[i];
            if (slot.active == NoActionIndex)
                continue;
            const auto &record = work.candidateRecords[slot.active];
            const auto &section = work.definitions[record.action].sections[slot.section];
            const auto &clip = *state.clips[section.clip];
            const auto &descriptor = clip.Data().descriptor;
            const AnimationClipSampleContext context{.skeleton = descriptor.skeleton,
                                                     .skeletonGeneration = descriptor.skeletonGeneration,
                                                     .clipGeneration = descriptor.generation,
                                                     .outputPose = Pose(state, work.overlay, i)};
            if (const auto sampled = clip.SamplePrepared(slot.sampleTime, context); sampled.HasError())
                return {GraphInstanceFailure::ClipFailure, sampled.ErrorValue()};
            for (const auto &transform : Pose(state, work.overlay, i))
                if (!ValidPoseTransform(transform))
                    return {GraphInstanceFailure::Malformed};
        }
        return {};
    }

    /** @copydoc EvaluateActions */
    GraphInstanceOutcome EvaluateActions(GraphInstanceState &state, const GraphEvaluationInput &input) {
        auto &work = state.actions;
        if (work.slots.empty())
            return {};
        std::ranges::copy(work.records, work.candidateRecords.begin());
        std::ranges::copy(work.slots, work.candidateSlots.begin());
        std::ranges::copy(work.committedOverlay, work.overlay.begin());
        std::ranges::copy(work.committedFrozen, work.frozen.begin());
        work.transitions = 0;
        work.boundaryCrossings = 0;
        for (std::uint32_t i = 0; i < work.slots.size(); ++i)
            if (work.slots[i].active == NoActionIndex && work.slots[i].head == NoActionIndex && work.slots[i].interrupt == NoActionIndex)
                state.candidate.players[ActionPlayer(state, i)].remainder = 0;
        if (const auto commands = AdmitActionCommands(state, input.tick); commands.HasError())
            return commands;
        for (std::uint32_t i = 0; i < work.slots.size(); ++i)
            if (const auto moved = AdvanceActionSlot(state, i, input); moved.HasError())
                return moved;
        return SampleActions(state);
    }

    /** @copydoc ComposeActionSlot */
    GraphInstanceOutcome ComposeActionSlot(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence) {
        const auto &instruction = state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex];
        const auto pin = std::ranges::find(instruction.pins, GraphPinRole::FirstPose, &GraphPin::role);
        const auto base = std::ranges::find(occurrence.inputs, pin->id, &GraphOccurrenceInput::destination)->slot;
        std::ranges::copy(state.Pose(base), state.Pose(occurrence.output).begin());
        std::ranges::copy(state.Coverage(base), state.Coverage(occurrence.output).begin());
        const auto index = *occurrence.actionSlot;
        if (const auto outgoing = Outgoing(state, index, occurrence); outgoing.HasError())
            return outgoing;
        const auto &slot = state.actions.candidateSlots[index];
        if (slot.active == NoActionIndex)
            return {};
        const auto &record = state.actions.candidateRecords[slot.active];
        return Layer(state, state.program->Actions()[record.action], state.actions.definitions[record.action], slot.section,
                     Pose(state, state.actions.overlay, index), slot.weight, occurrence);
    }
}  // namespace Horo::Animation::Detail
