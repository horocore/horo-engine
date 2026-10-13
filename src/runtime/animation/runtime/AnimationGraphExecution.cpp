#include "AnimationGraphInstanceInternal.h"
#include "AnimationPoseComposition.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Animation::Detail {
    namespace {
        constexpr std::int64_t RatePrecision = 1'000'000;
        constexpr std::int32_t MaximumRate = 16'000'000;

    }  // namespace

    /** @copydoc ScaleGraphDelta */
    AnimationClipOutcome<AnimationTimeDelta> ScaleGraphDelta(const GraphEvaluationInput &input, GraphPlayerState &player) noexcept {
        const auto rate = static_cast<std::int64_t>(input.rate.millionths);
        const auto quotient = input.delta.ticks / RatePrecision;
        if ((rate > 0 && quotient > std::numeric_limits<std::int64_t>::max() / rate) ||
            (rate < 0 && quotient > std::numeric_limits<std::int64_t>::max() / -rate))
            return AnimationClipOutcome<AnimationTimeDelta>::Failure(
                {AnimationClipFailure::TimeOverflow, "The player rate overflows exact time."});
        const auto integral = quotient * rate;
        const auto residual = (input.delta.ticks % RatePrecision) * rate + player.remainder;
        const auto carry = residual / RatePrecision;
        if ((carry > 0 && integral > std::numeric_limits<std::int64_t>::max() - carry) ||
            (carry < 0 && integral < std::numeric_limits<std::int64_t>::min() - carry))
            return AnimationClipOutcome<AnimationTimeDelta>::Failure(
                {AnimationClipFailure::TimeOverflow, "The player residual overflows exact time."});
        player.remainder = residual % RatePrecision;
        return AnimationClipOutcome<AnimationTimeDelta>::Success({integral + carry});
    }

    namespace {

        /** @brief Validates one scalar override's variant and value before storing the candidate snapshot. */
        GraphInstanceFailure ValidateParameterValue(const GraphParameter &declaration, const GraphParameterInput &parameter) noexcept {
            using enum GraphInstanceFailure;
            if (declaration.defaultValue.index() != parameter.value.index())
                return TypeMismatch;
            if (const auto value = std::get_if<float>(&parameter.value); value && !std::isfinite(*value))
                return Malformed;
            return None;
        }

        /** @brief Captures all defaults and exact typed overrides before any instruction runs. */
        GraphInstanceOutcome CaptureParameters(GraphInstanceState &state, const GraphEvaluationInput &input) noexcept {
            const auto declarations = state.program->Parameters();
            if (input.parameters.size() > declarations.size())
                return {GraphInstanceFailure::LimitExceeded};
            for (std::size_t index = 0; index < declarations.size(); ++index)
                state.candidate.parameters[index] = declarations[index].defaultValue;
            for (std::size_t index = 0; index < state.curves.publications.size(); ++index)
                state.candidate.parameters[state.curves.parameters[index]] = state.curves.publications[index]->Sample(input.curveTime);
            for (std::size_t index = 0; index < declarations.size(); ++index)
                if (state.queuedTriggers[index] != 0)
                    state.candidate.parameters[index] = true;
            for (std::size_t index = 0; index < input.parameters.size(); ++index) {
                const auto &parameter = input.parameters[index];
                const auto found = std::ranges::lower_bound(declarations, parameter.id, {}, &GraphParameter::id);
                if (found == declarations.end() || found->id != parameter.id)
                    return {GraphInstanceFailure::BindingMismatch};
                if (const auto failure = ValidateParameterValue(*found, parameter); failure != GraphInstanceFailure::None)
                    return {failure};
                for (std::size_t earlier = 0; earlier < index; ++earlier)
                    if (input.parameters[earlier].id == parameter.id)
                        return {GraphInstanceFailure::Malformed};
                const auto destination = static_cast<std::size_t>(found - declarations.begin());
                // False invocation input cannot erase an already accepted queued trigger.
                if (found->type != GraphValueType::Trigger || state.queuedTriggers[destination] == 0)
                    state.candidate.parameters[destination] = parameter.value;
            }
            return {};
        }

        /** @brief Resolves a compiled semantic operand; compiler validation already proves exactly one matching input. */
        GraphWorkingSlot Operand(const GraphInstructionOccurrence &occurrence, const GraphInstruction &instruction,
                                 const GraphPinRole role) noexcept {
            const auto pin = std::ranges::find(instruction.pins, role, &GraphPin::role);
            return std::ranges::find(occurrence.inputs, pin->id, &GraphOccurrenceInput::destination)->slot;
        }

        /** @brief Samples one actual immutable clip with its independent occurrence player candidate. */
        GraphInstanceOutcome SampleClip(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                        const GraphInstruction &instruction, const GraphEvaluationInput &input) {
            auto &player = state.candidate.players[*occurrence.clipPlayer];
            const auto delta = ScaleGraphDelta(input, player);
            if (delta.HasError())
                return {GraphInstanceFailure::ClipFailure, delta.ErrorValue()};
            const auto &clip = *state.clips[*occurrence.clipBindingIndex];
            const auto traversal = clip.TraversePrepared(player.cursor, delta.Value(), state.clipLimits);
            if (traversal.HasError())
                return {GraphInstanceFailure::ClipFailure, traversal.ErrorValue()};
            const auto &binding = clip.Data().descriptor;
            AnimationClipSampleContext sample{.skeleton = binding.skeleton,
                                              .skeletonGeneration = binding.skeletonGeneration,
                                              .clipGeneration = binding.generation,
                                              .outputPose = state.Pose(occurrence.output)};
            if (binding.kind == AnimationClipKind::Additive) {
                sample.basePose = state.Pose(Operand(occurrence, instruction, GraphPinRole::FirstPose));
                sample.referenceLocalPose = state.Pose(Operand(occurrence, instruction, GraphPinRole::ReferencePose));
                sample.referencePose = binding.referencePose;
            }
            if (const auto sampled = clip.SamplePrepared(traversal.Value().sampleTime, sample); sampled.HasError())
                return {GraphInstanceFailure::ClipFailure, sampled.ErrorValue()};
            for (const auto &transform : state.Pose(occurrence.output))
                if (!ValidPoseTransform(transform))
                    return {GraphInstanceFailure::Malformed};
            const auto coverage = state.Coverage(occurrence.output);
            std::ranges::copy(occurrence.clipCoverage, coverage.begin());
            if (binding.kind == AnimationClipKind::Additive) {
                const auto baseCoverage = state.Coverage(Operand(occurrence, instruction, GraphPinRole::FirstPose));
                for (std::size_t joint = 0; joint < coverage.size(); ++joint)
                    coverage[joint] = static_cast<std::uint8_t>(coverage[joint] != 0 || baseCoverage[joint] != 0);
            }
            player.cursor = traversal.Value().cursor;
            return {};
        }

        /** @brief Propagates authored coverage using only contributions that affect the candidate pose. */
        void LayerCoverage(const GraphBlendNode &policy, const std::span<const float> mask, const float weight,
                           const std::span<const std::uint8_t> base, const std::span<const std::uint8_t> overlay,
                           const std::span<std::uint8_t> output) noexcept {
            for (std::size_t joint = 0; joint < output.size(); ++joint) {
                const auto effective =
                    policy.missingJoint == GraphMissingJointPolicy::PreserveBase && overlay[joint] == 0 ? 0.0F : weight * mask[joint];
                const bool retainsBase = policy.mode != GraphBlendMode::Normalized || effective < 1.0F;
                output[joint] = static_cast<std::uint8_t>((retainsBase && base[joint] != 0) || (effective > 0.0F && overlay[joint] != 0));
            }
        }

        /** @brief Executes one ordered layer using the compiler's exact immutable mask and pose operands. */
        GraphInstanceOutcome BlendPose(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                       const GraphInstruction &instruction) {
            using enum GraphPinRole;
            const auto first = state.Pose(Operand(occurrence, instruction, FirstPose));
            const auto second = state.Pose(Operand(occurrence, instruction, SecondPose));
            const auto firstCoverage = state.Coverage(Operand(occurrence, instruction, FirstPose));
            const auto secondCoverage = state.Coverage(Operand(occurrence, instruction, SecondPose));
            const auto &policy = std::get<GraphBlendNode>(instruction.payload);
            const auto weightSlot = Operand(occurrence, instruction, Weight);
            const auto weight =
                std::clamp(std::get<float>(
                               state.scalars.values[state.scalars.offsets[static_cast<std::size_t>(weightSlot.type)] + weightSlot.index]),
                           0.0F, 1.0F);
            PoseCompositionInput composition{.base = first,
                                             .overlay = second,
                                             .mask = occurrence.jointMask,
                                             .mode = policy.mode,
                                             .weight = weight};
            if (policy.mode != GraphBlendMode::Normalized)
                composition.reference = state.Pose(Operand(occurrence, instruction, ReferencePose));
            if (policy.missingJoint == GraphMissingJointPolicy::PreserveBase)
                composition.coverage = secondCoverage;
            if (const auto failure = ComposePose(composition, state.Pose(occurrence.output)); failure != GraphInstanceFailure::None)
                return {failure};
            LayerCoverage(policy, occurrence.jointMask, weight, firstCoverage, secondCoverage, state.Coverage(occurrence.output));
            return {};
        }

        /** @brief Executes one compiler occurrence with exhaustive typed opcode dispatch. */
        GraphInstanceOutcome ExecuteOccurrence(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                               const GraphInstruction &instruction, const GraphEvaluationInput &input) {
            return std::visit([&state, &occurrence, &instruction, &input]<typename Payload>(const Payload &) -> GraphInstanceOutcome {
                if constexpr (std::is_same_v<Payload, GraphClipNode>) {
                    return SampleClip(state, occurrence, instruction, input);
                } else if constexpr (std::is_same_v<Payload, GraphParameterNode>) {
                    state.scalars
                        .values[state.scalars.offsets[static_cast<std::size_t>(occurrence.output.type)] + occurrence.output.index] =
                        state.candidate.parameters[*occurrence.parameterRead];
                    return {};
                } else if constexpr (std::is_same_v<Payload, GraphBlendNode>) {
                    return BlendPose(state, occurrence, instruction);
                } else {
                    // Input, Call and Output alias the actual earlier writer in the compiled access plan.
                    return {};
                }
            }, instruction.payload);
        }

        /** @brief Starts a tick transaction in already allocated candidate storage; committed state is untouched. */
        void PrepareCandidate(GraphInstanceState &state) {
            std::ranges::copy(state.players, state.candidate.players.begin());
            std::ranges::copy(state.machines, state.candidate.machines.begin());
            std::ranges::copy(state.latches, state.candidate.latches.begin());
            state.candidate.eventCount = 0;
            for (auto &machine : state.candidate.machines) {
                machine.visited = false;
                machine.retiring = false;
                machine.held = {};
            }
        }

        /** @brief Executes one active guarded phase using the sole prepared clip/pose dispatch path. */
        GraphInstanceOutcome ExecuteGuardedOccurrence(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                                      const GraphEvaluationInput &input) {
            const auto &instruction = state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex];
            auto branchInput = input;
            if (!StateBranchInput(state, occurrence, branchInput))
                return {};
            if (occurrence.phase == GraphOccurrencePhase::StateBegin)
                return BeginGraphStateMachine(state, occurrence, branchInput);
            if (occurrence.phase == GraphOccurrencePhase::StateEnd)
                return EndGraphStateMachine(state, occurrence);
            if (occurrence.clipBindingIndex.has_value())
                return SampleClip(state, occurrence, instruction, branchInput);
            return ExecuteOccurrence(state, occurrence, instruction, branchInput);
        }

        /** @brief Either invocation or owner cancellation aborts the candidate before publication. */
        bool Cancelled(const GraphInstanceState &state, const GraphEvaluationInput &input) noexcept {
            return input.cancellation.IsCancellationRequested() || state.ownerCancellation.IsCancellationRequested();
        }

        /** @brief Completes retirement only after the full occurrence stream and final cancellation fence succeed. */
        void CompleteRetirement(GraphInstanceState &state) {
            for (std::size_t index = 0; index < state.candidate.machines.size(); ++index)
                if (state.candidate.machines[index].retiring)
                    ResetGraphStateControl(state, static_cast<std::uint32_t>(index));
        }
    }  // namespace

    /** @copydoc ExecuteGraph */
    GraphInstanceOutcome ExecuteGraph(GraphInstanceState &state, const GraphEvaluationInput &input) {
        if (input.delta.ticks < 0 || input.rate.millionths < -MaximumRate || input.rate.millionths > MaximumRate)
            return {GraphInstanceFailure::Malformed};
        if (const auto captured = CaptureParameters(state, input); captured.HasError())
            return captured;
        PrepareCandidate(state);
        for (const auto &occurrence : state.program->Occurrences()) {
            if (Cancelled(state, input))
                return {GraphInstanceFailure::Cancelled};
            if (const auto executed = ExecuteGuardedOccurrence(state, occurrence, input); executed.HasError())
                return executed;
            // Input, Call and Output alias the actual earlier writer, as established by the compiled access plan.
        }
        if (Cancelled(state, input))
            return {GraphInstanceFailure::Cancelled};
        CompleteRetirement(state);
        return {};
    }
}  // namespace Horo::Animation::Detail
