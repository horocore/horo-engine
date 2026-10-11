#include "AnimationGraphInstanceInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Animation::Detail {
    namespace {
        constexpr std::int64_t RatePrecision = 1'000'000;
        constexpr std::int32_t MaximumRate = 16'000'000;

        /** @brief Checks signed arithmetic before retaining the exact per-player rate residual. */
        AnimationClipOutcome<AnimationTimeDelta> ScaleDelta(const GraphEvaluationInput &input, GraphPlayerState &player) noexcept {
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

        /** @brief Validates one scalar override's variant and value before storing the candidate snapshot. */
        GraphInstanceFailure ValidateParameterValue(const GraphParameter &declaration, const GraphParameterInput &parameter) noexcept {
            if (declaration.defaultValue.index() != parameter.value.index())
                return GraphInstanceFailure::TypeMismatch;
            if (const auto value = std::get_if<float>(&parameter.value); value && !std::isfinite(*value))
                return GraphInstanceFailure::Malformed;
            return GraphInstanceFailure::None;
        }

        /** @brief Captures all defaults and exact typed overrides before any instruction runs. */
        GraphInstanceOutcome CaptureParameters(GraphInstanceState &state, const GraphEvaluationInput &input) noexcept {
            const auto declarations = state.program->Parameters();
            if (input.parameters.size() > declarations.size())
                return {GraphInstanceFailure::LimitExceeded};
            for (std::size_t index = 0; index < declarations.size(); ++index)
                state.candidateParameters[index] = declarations[index].defaultValue;
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
                state.candidateParameters[static_cast<std::size_t>(found - declarations.begin())] = parameter.value;
            }
            return {};
        }

        /** @brief Resolves a compiled semantic operand; compiler validation already proves exactly one matching input. */
        GraphWorkingSlot Operand(const GraphInstructionOccurrence &occurrence, const GraphInstruction &instruction,
                                 const GraphPinRole role) noexcept {
            const auto pin = std::ranges::find(instruction.pins, role, &GraphPin::role);
            return std::ranges::find(occurrence.inputs, pin->id, &GraphOccurrenceInput::destination)->slot;
        }

        /** @brief Rejects non-finite sampled/blended candidates before any pose publication. */
        bool ValidTransform(const Math::Transform &transform) noexcept {
            if (!Math::IsFinite(transform.translation) || !Math::IsFinite(transform.scale) || !Math::IsFinite(transform.rotation))
                return false;
            const auto &q = transform.rotation;
            const auto length = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y + static_cast<double>(q.z) * q.z +
                                static_cast<double>(q.w) * q.w;
            return length > static_cast<double>(Math::DefaultEpsilon) * Math::DefaultEpsilon && std::abs(transform.scale.x) > 1.0e-8F &&
                   std::abs(transform.scale.y) > 1.0e-8F && std::abs(transform.scale.z) > 1.0e-8F;
        }

        /** @brief Uses double intermediates so blending opposite finite extrema cannot overflow subtraction. */
        Math::Vec3 BlendVector(const Math::Vec3 first, const Math::Vec3 second, const float weight) noexcept {
            const auto component = [weight](float a, float b) {
                return static_cast<float>((1.0 - weight) * static_cast<double>(a) + weight * static_cast<double>(b));
            };
            return {component(first.x, second.x), component(first.y, second.y), component(first.z, second.z)};
        }

        /** @brief Samples one actual immutable clip with its independent occurrence player candidate. */
        GraphInstanceOutcome SampleClip(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                        const GraphEvaluationInput &input) {
            auto &player = state.candidatePlayers[*occurrence.clipPlayer];
            const auto delta = ScaleDelta(input, player);
            if (delta.HasError())
                return {GraphInstanceFailure::ClipFailure, delta.ErrorValue()};
            const auto &clip = *state.clips[*occurrence.clipBindingIndex];
            const auto traversal = clip.TraversePrepared(player.cursor, delta.Value(), state.clipLimits);
            if (traversal.HasError())
                return {GraphInstanceFailure::ClipFailure, traversal.ErrorValue()};
            const auto &binding = clip.Data().descriptor;
            const auto sampled = clip.SamplePrepared(traversal.Value().sampleTime, {.skeleton = binding.skeleton,
                                                                                    .skeletonGeneration = binding.skeletonGeneration,
                                                                                    .clipGeneration = binding.generation,
                                                                                    .outputPose = state.Pose(occurrence.output)});
            if (sampled.HasError())
                return {GraphInstanceFailure::ClipFailure, sampled.ErrorValue()};
            for (const auto &transform : state.Pose(occurrence.output))
                if (!ValidTransform(transform))
                    return {GraphInstanceFailure::Malformed};
            player.cursor = traversal.Value().cursor;
            return {};
        }

        /** @brief Executes the compiler's two-pose blend without adding a second graph scheduling model. */
        GraphInstanceOutcome BlendPose(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                       const GraphInstruction &instruction) {
            const auto first = state.Pose(Operand(occurrence, instruction, GraphPinRole::FirstPose));
            const auto second = state.Pose(Operand(occurrence, instruction, GraphPinRole::SecondPose));
            const auto weightSlot = Operand(occurrence, instruction, GraphPinRole::Weight);
            const auto weight =
                std::clamp(std::get<float>(
                               state.workingScalars[state.scalarOffsets[static_cast<std::size_t>(weightSlot.type)] + weightSlot.index]),
                           0.0F, 1.0F);
            const auto output = state.Pose(occurrence.output);
            for (std::size_t joint = 0; joint < output.size(); ++joint) {
                output[joint] = {BlendVector(first[joint].translation, second[joint].translation, weight),
                                 Math::Slerp(first[joint].rotation, second[joint].rotation, weight),
                                 BlendVector(first[joint].scale, second[joint].scale, weight)};
                if (!ValidTransform(output[joint]))
                    return {GraphInstanceFailure::Malformed};
            }
            return {};
        }
    }  // namespace

    /** @copydoc ExecuteGraph */
    GraphInstanceOutcome ExecuteGraph(GraphInstanceState &state, const GraphEvaluationInput &input) {
        if (input.delta.ticks < 0 || input.rate.millionths < -MaximumRate || input.rate.millionths > MaximumRate)
            return {GraphInstanceFailure::Malformed};
        if (const auto captured = CaptureParameters(state, input); captured.HasError())
            return captured;
        std::ranges::copy(state.players, state.candidatePlayers.begin());
        for (const auto &occurrence : state.program->Occurrences()) {
            if (input.cancellation.IsCancellationRequested() || state.ownerCancellation.IsCancellationRequested())
                return {GraphInstanceFailure::Cancelled};
            const auto &instruction = state.program->Definitions()[occurrence.definitionIndex].instructions[occurrence.instructionIndex];
            if (std::holds_alternative<GraphClipNode>(instruction.payload)) {
                if (const auto sampled = SampleClip(state, occurrence, input); sampled.HasError())
                    return sampled;
            } else if (std::holds_alternative<GraphParameterNode>(instruction.payload)) {
                state.workingScalars[state.scalarOffsets[static_cast<std::size_t>(occurrence.output.type)] + occurrence.output.index] =
                    state.candidateParameters[*occurrence.parameterRead];
            } else if (std::holds_alternative<GraphBlendNode>(instruction.payload)) {
                if (const auto blended = BlendPose(state, occurrence, instruction); blended.HasError())
                    return blended;
            }
            // Input, Call and Output alias the actual earlier writer, as established by the compiled access plan.
        }
        if (input.cancellation.IsCancellationRequested() || state.ownerCancellation.IsCancellationRequested())
            return {GraphInstanceFailure::Cancelled};
        return {};
    }
}  // namespace Horo::Animation::Detail
