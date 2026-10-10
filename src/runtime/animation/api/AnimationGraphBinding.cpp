#include "AnimationGraphCompilerInternal.h"

#include <map>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        using namespace AnimationErrors;

        /** @brief Captures validated compatibility values, not borrowed asset lifetimes. */
        struct GraphDependencies final {
            GraphSkeletonBinding skeleton{};
            std::vector<AnimationClipDescriptor> clips{};
        };

        /** @brief Requires the exact typed dependency set and one pinned skeleton publication. */
        Result<GraphDependencies> ValidateDependencies(const AnimationGraphProgram &program, const AnimationGraphCompileContext &context) {
            if (!context.skeleton || !context.skeletonGeneration.IsValid() || context.skeleton->Data().skeleton != program.Skeleton() ||
                context.skeleton->Data().contractVersion != CurrentSkeletonAssetContractVersion ||
                context.clips.size() != program.Clips().size())
                return Result<GraphDependencies>::Failure(Failure(GraphBindingMismatch));
            GraphDependencies result;
            result.skeleton = {program.Skeleton(), context.skeletonGeneration, context.skeleton->Data().contractVersion,
                               static_cast<std::uint32_t>(context.skeleton->Data().joints.size())};
            for (const auto &clip : context.clips) {
                if (auto state = Admission(context); state.HasError())
                    return Result<GraphDependencies>::Failure(state.ErrorValue());
                const auto &descriptor = clip.Data().descriptor;
                if (descriptor.skeleton != result.skeleton.id || descriptor.skeletonGeneration != result.skeleton.generation ||
                    descriptor.contractVersion != CurrentAnimationClipContractVersion || !descriptor.generation.IsValid())
                    return Result<GraphDependencies>::Failure(Failure(GraphBindingMismatch));
                result.clips.push_back(descriptor);
            }
            std::ranges::sort(result.clips, {}, &AnimationClipDescriptor::id);
            for (std::size_t index = 0; index < result.clips.size(); ++index)
                if (result.clips[index].id != program.Clips()[index])
                    return Result<GraphDependencies>::Failure(Failure(GraphBindingMismatch));
            return Result<GraphDependencies>::Success(std::move(result));
        }

        /** @brief Detached preallocated instance access plan under construction. */
        struct OccurrencePlan final {
            std::vector<GraphInstructionOccurrence> occurrences{};
            GraphMemoryRequirements memory{};
        };

        /** @brief Allocates one compile-time working slot only within exact captured storage limits. */
        Result<GraphWorkingSlot> AllocateSlot(GraphValueType type, OccurrencePlan &plan, const AnimationGraphCompileContext &context,
                                              std::uint32_t joints) {
            const auto kind = static_cast<std::size_t>(type);
            if (kind >= plan.memory.workingSlots.size())
                return Result<GraphWorkingSlot>::Failure(Failure(GraphTypeMismatch));
            const auto slot = plan.memory.workingSlots[kind]++;
            if (type == GraphValueType::Pose) {
                plan.memory.poseTransforms += joints;
                if (plan.memory.workingSlots[kind] > context.limits.poseSlots || plan.memory.poseTransforms > context.limits.poseTransforms)
                    return Result<GraphWorkingSlot>::Failure(Failure(GraphLimitExceeded));
            } else {
                std::uint64_t scalars{};
                for (std::size_t index = 1; index < plan.memory.workingSlots.size(); ++index)
                    scalars += plan.memory.workingSlots[index];
                if (scalars > context.limits.scalarSlots)
                    return Result<GraphWorkingSlot>::Failure(Failure(GraphLimitExceeded));
            }
            return Result<GraphWorkingSlot>::Success({type, slot});
        }

        /** @brief One bounded compile-time call frame; no process-local pointer is published. */
        struct OccurrenceFrame final {
            std::uint32_t definition{};
            std::size_t next{};
            std::vector<GraphSourceLocation> path{};
            std::vector<GraphWorkingSlot> slots{};
            std::vector<std::uint32_t> producers{};
            std::map<GraphInterfaceId, GraphOccurrenceInput> inputs{};
            std::optional<std::uint32_t> callerInstruction{};
            std::optional<GraphInstructionOccurrence> completion{};
        };

        /** @brief Creates a detached bounded call frame with stable path identity. */
        OccurrenceFrame Frame(std::uint32_t definition, std::span<const GraphCompiledDefinition> definitions) {
            OccurrenceFrame frame;
            frame.definition = definition;
            frame.slots.resize(definitions[definition].instructions.size());
            frame.producers.resize(frame.slots.size());
            return frame;
        }

        /** @brief Resolves all input accesses to earlier producers in this exact call occurrence. */
        GraphInstructionOccurrence Occurrence(const GraphInstruction &instruction, const OccurrenceFrame &frame, std::uint32_t index) {
            GraphInstructionOccurrence occurrence;
            occurrence.source = instruction.source;
            occurrence.callPath = frame.path;
            occurrence.definitionIndex = frame.definition;
            occurrence.instructionIndex = index;
            occurrence.parameterRead = instruction.parameterIndex;
            occurrence.output.type = GraphValueType::Unspecified;
            for (const auto &input : instruction.inputs)
                occurrence.inputs.push_back({input.destination, frame.slots[input.instruction], frame.producers[input.instruction]});
            return occurrence;
        }

        /** @brief Assigns exact working/state slots to a non-call instruction without evaluating animation. */
        Result<void> AssignOutput(const GraphInstruction &instruction, OccurrenceFrame &frame, GraphInstructionOccurrence &occurrence,
                                  OccurrencePlan &plan, const GraphDependencies &dependencies,
                                  const AnimationGraphCompileContext &context) {
            if (const auto *input = std::get_if<GraphInputNode>(&instruction.payload)) {
                auto access = frame.inputs.at(input->input);
                access.destination = instruction.pins.front().id;
                occurrence.inputs.push_back(access);
                occurrence.output = access.slot;
                occurrence.outputProducer = access.sourceOccurrence;
            } else if (std::holds_alternative<GraphOutputNode>(instruction.payload)) {
                occurrence.output = occurrence.inputs.front().slot;
                occurrence.outputProducer = occurrence.inputs.front().sourceOccurrence;
            } else {
                const auto pin = std::ranges::find_if(instruction.pins, [](const auto &value) {
                    return value.output;
                });
                auto slot = AllocateSlot(pin->type, plan, context, dependencies.skeleton.joints);
                if (slot.HasError())
                    return Result<void>::Failure(slot.ErrorValue());
                occurrence.output = slot.Value();
                occurrence.outputProducer = static_cast<std::uint32_t>(plan.occurrences.size());
            }
            if (const auto *clip = std::get_if<GraphClipNode>(&instruction.payload)) {
                if (plan.memory.clipPlayers >= context.limits.clipPlayers)
                    return Result<void>::Failure(Failure(GraphLimitExceeded));
                occurrence.clipPlayer = plan.memory.clipPlayers++;
                occurrence.clipBindingIndex = static_cast<std::uint32_t>(Find(dependencies.clips, clip->clip) - dependencies.clips.data());
            }
            return Result<void>::Success();
        }

        /** @brief Creates the callee's interface bindings and stable path from the exact caller operands. */
        OccurrenceFrame Callee(const GraphInstruction &instruction, const OccurrenceFrame &caller, GraphInstructionOccurrence occurrence,
                               std::span<const GraphCompiledDefinition> definitions) {
            auto frame = Frame(*instruction.definitionIndex, definitions);
            frame.path = caller.path;
            frame.path.push_back(instruction.source);
            frame.callerInstruction = occurrence.instructionIndex;
            for (const auto &input : occurrence.inputs)
                frame.inputs.emplace(Find(instruction.pins, input.destination)->interfaceId, input);
            frame.completion = std::move(occurrence);
            return frame;
        }

        /** @brief Expands bounded occurrence identities and state/access plans with an explicit call stack. */
        Result<OccurrencePlan> BuildOccurrencePlan(const AnimationGraphProgram &program, const GraphDependencies &dependencies,
                                                   const AnimationGraphCompileContext &context) {
            OccurrencePlan plan;
            plan.memory.parameters = static_cast<std::uint32_t>(program.Parameters().size());
            const auto definitions = program.Definitions();
            const auto &entry = definitions[program.EntryIndex()];
            plan.memory.callDepth = entry.callDepth;
            plan.occurrences.reserve(static_cast<std::size_t>(entry.evaluationInstructions));
            std::vector<OccurrenceFrame> frames;
            frames.reserve(entry.callDepth);
            frames.push_back(Frame(program.EntryIndex(), definitions));
            while (!frames.empty()) {
                if (auto state = Admission(context); state.HasError())
                    return Result<OccurrencePlan>::Failure(state.ErrorValue());
                auto &frame = frames.back();
                const auto &definition = definitions[frame.definition];
                if (frame.next == definition.instructions.size()) {
                    const auto output = frame.slots[definition.outputInstruction];
                    const auto producer = frame.producers[definition.outputInstruction];
                    const auto callerInstruction = frame.callerInstruction;
                    auto completion = std::move(frame.completion);
                    frames.pop_back();
                    if (callerInstruction) {
                        frames.back().slots[*callerInstruction] = output;
                        frames.back().producers[*callerInstruction] = producer;
                        completion->output = output;
                        completion->outputProducer = producer;
                        plan.occurrences.push_back(std::move(*completion));
                    }
                    continue;
                }
                const auto index = static_cast<std::uint32_t>(frame.next++);
                const auto &instruction = definition.instructions[index];
                auto occurrence = Occurrence(instruction, frame, index);
                if (instruction.definitionIndex) {
                    auto child = Callee(instruction, frame, std::move(occurrence), definitions);
                    frames.push_back(std::move(child));
                } else {
                    if (auto output = AssignOutput(instruction, frame, occurrence, plan, dependencies, context); output.HasError())
                        return Result<OccurrencePlan>::Failure(output.ErrorValue());
                    frame.slots[index] = occurrence.output;
                    frame.producers[index] = occurrence.outputProducer;
                    plan.occurrences.push_back(std::move(occurrence));
                }
            }
            return Result<OccurrencePlan>::Success(std::move(plan));
        }
    }  // namespace

    /** @copydoc Bind */
    Result<BoundPlan> Bind(const AnimationGraphProgram &program, const AnimationGraphCompileContext &context) {
        auto dependencies = ValidateDependencies(program, context);
        if (dependencies.HasError())
            return Result<BoundPlan>::Failure(dependencies.ErrorValue());
        auto plan = BuildOccurrencePlan(program, dependencies.Value(), context);
        if (plan.HasError())
            return Result<BoundPlan>::Failure(plan.ErrorValue());
        BoundPlan result;
        result.skeleton = dependencies.Value().skeleton;
        result.memory = plan.Value().memory;
        result.clips = std::move(dependencies).Value().clips;
        result.occurrences = std::move(plan).Value().occurrences;
        return Result<BoundPlan>::Success(std::move(result));
    }
}  // namespace Horo::Animation::GraphCompileDetail
