#include "AnimationGraphCompilerInternal.h"

#include <map>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        /** @brief Compile-only rejection; no partially expanded plan is published. */
        struct Rejection final {
            const ErrorCodeDescriptor *code;
        };

        /** @brief Retains exact canonical pose-binding diagnostics across compile-only expansion unwinding. */
        struct BindingRejection final {
            Error error;
        };

        /** @brief Exact earlier writer access, independent of aliases and active state lanes. */
        struct Access final {
            GraphWorkingSlot slot;
            std::uint32_t producer{};
        };

        /** @brief Builds a single bounded guarded access plan; runtime executes it without recursion. */
        class Builder final {
        public:
            /** @brief Captures immutable dependency compatibility for this detached compilation. */
            Builder(const AnimationGraphProgram &program, const AnimationGraphCompileContext &context, const GraphSkeletonBinding &skeleton,
                    std::vector<AnimationClipDescriptor> clips)
                : program_(program), context_(context) {
                plan_.skeleton = skeleton;
                plan_.clips = std::move(clips);
                plan_.memory.parameters = static_cast<std::uint32_t>(program.Parameters().size());
                plan_.memory.callDepth = program.Definitions()[program.EntryIndex()].callDepth;
            }

            /** @brief Returns only a complete admitted entry plan. */
            BoundPlan Build() {
                Expand(program_.EntryIndex(), {}, {}, {});
                Check();
                return std::move(plan_);
            }

        private:
            /** @brief Checks caller admission and cancellation at bounded expansion checkpoints. */
            void Check() const {
                if (!context_.accepting)
                    throw Rejection{&AnimationErrors::GraphAdmissionRejected};
                if (context_.cancellation.IsCancellationRequested())
                    throw Rejection{&AnimationErrors::GraphOperationCancelled};
            }

            /** @brief Allocates one exact typed working slot within the captured memory policy. */
            GraphWorkingSlot Slot(const GraphValueType type) {
                const auto kind = static_cast<std::size_t>(type);
                const auto index = plan_.memory.workingSlots[kind]++;
                if (type == GraphValueType::Pose) {
                    plan_.memory.poseTransforms += plan_.skeleton.joints;
                    if (plan_.memory.workingSlots[kind] > context_.limits.poseSlots ||
                        plan_.memory.poseTransforms > context_.limits.poseTransforms)
                        throw Rejection{&AnimationErrors::GraphLimitExceeded};
                } else {
                    std::uint64_t scalars = 0;
                    for (std::size_t scalar = 1; scalar < plan_.memory.workingSlots.size(); ++scalar)
                        scalars += plan_.memory.workingSlots[scalar];
                    if (scalars > context_.limits.scalarSlots)
                        throw Rejection{&AnimationErrors::GraphLimitExceeded};
                }
                return {type, index};
            }

            /** @brief Appends one complete occurrence after charging actual call-path and guard storage. */
            std::uint32_t Append(GraphInstructionOccurrence occurrence) {
                Check();
                pathEntries_ += occurrence.callPath.size() + occurrence.guards.size();
                if (plan_.occurrences.size() >= context_.limits.evaluationInstructions ||
                    pathEntries_ > context_.limits.sourceMapCallEntries)
                    throw Rejection{&AnimationErrors::GraphLimitExceeded};
                const auto index = static_cast<std::uint32_t>(plan_.occurrences.size());
                plan_.occurrences.push_back(std::move(occurrence));
                return index;
            }

            /** @brief Gives this exact clip occurrence its own player and complete pose storage. */
            Access Clip(const GraphClipNode &clip, GraphInstructionOccurrence occurrence) {
                if (plan_.memory.clipPlayers >= context_.limits.clipPlayers)
                    throw Rejection{&AnimationErrors::GraphLimitExceeded};
                occurrence.clipPlayer = plan_.memory.clipPlayers++;
                occurrence.clipBindingIndex = static_cast<std::uint32_t>(Find(plan_.clips, clip.clip) - plan_.clips.data());
                if (auto bound = BindWriterComposition(clip, occurrence, plan_.memory, context_); bound.HasError())
                    throw BindingRejection{bound.ErrorValue()};
                occurrence.output = Slot(GraphValueType::Pose);
                occurrence.outputProducer = static_cast<std::uint32_t>(plan_.occurrences.size());
                const Access output{occurrence.output, occurrence.outputProducer};
                Append(std::move(occurrence));
                return output;
            }

            /** @brief Expands the two incarnations of one state with exact guarded child ranges. */
            GraphStateBranch StateBranch(const GraphState &state, const GraphInstructionOccurrence &occurrence, const std::uint32_t machine,
                                         const std::uint32_t stateIndex) {
                GraphStateBranch branch;
                branch.id = state.id;
                branch.loopPeriod = state.loopPeriod;
                for (std::uint8_t lane = 0; lane < 2; ++lane) {
                    auto child = occurrence;
                    child.phase = GraphOccurrencePhase::Evaluate;
                    child.stateMachine.reset();
                    child.callPath.push_back(occurrence.source);
                    child.guards.push_back({machine, stateIndex, lane});
                    branch.begin[lane] = static_cast<std::uint32_t>(plan_.occurrences.size());
                    branch.output[lane] = StateSource(state, std::move(child)).slot;
                    branch.end[lane] = static_cast<std::uint32_t>(plan_.occurrences.size());
                }
                if (const auto *clip = std::get_if<AnimationClipId>(&state.source)) {
                    const auto &descriptor = *Find(plan_.clips, *clip);
                    if (descriptor.wrapMode == AnimationWrapMode::Loop)
                        branch.loopPeriod = descriptor.duration;
                    if (descriptor.wrapMode == AnimationWrapMode::PingPong)
                        branch.loopPeriod = AnimationDeltaTime{2 * descriptor.duration.ticks};
                }
                return branch;
            }

            /** @brief Resolves one state source through canonical clip binding or the bounded subgraph expander. */
            Access StateSource(const GraphState &state, GraphInstructionOccurrence child) {
                if (const auto *clip = std::get_if<AnimationClipId>(&state.source))
                    return Clip(GraphClipNode{*clip}, std::move(child));
                const auto id = std::get<GraphDefinitionId>(state.source);
                const auto definitions = program_.Definitions();
                const auto found = std::ranges::lower_bound(definitions, id, {}, &GraphCompiledDefinition::id);
                return Expand(static_cast<std::uint32_t>(found - definitions.begin()), child.callPath, child.guards, {});
            }

            /** @brief Emits control, guarded state lanes, and final composition in actual runtime order. */
            Access Machine(const GraphStateMachineNode &machine, GraphInstructionOccurrence occurrence) {
                const auto index = static_cast<std::uint32_t>(plan_.stateMachines.size());
                if (index >= context_.limits.stateMachines)
                    throw Rejection{&AnimationErrors::GraphLimitExceeded};
                GraphStateMachineBinding binding;
                binding.conditionOffset = plan_.memory.conditionLatches;
                for (const auto &transition : machine.transitions)
                    plan_.memory.conditionLatches += static_cast<std::uint32_t>(transition.conditions.size());
                if (plan_.memory.conditionLatches > context_.limits.conditions || plan_.memory.stateEvents > context_.limits.stateEvents ||
                    262U > context_.limits.stateEvents - plan_.memory.stateEvents)
                    throw Rejection{&AnimationErrors::GraphLimitExceeded};
                plan_.memory.stateEvents += 262;
                plan_.memory.stateMachines = index + 1;
                occurrence.stateMachine = index;
                occurrence.phase = GraphOccurrencePhase::StateBegin;
                occurrence.output = Slot(GraphValueType::Pose);
                binding.beginOccurrence = Append(occurrence);
                plan_.stateMachines.push_back(binding);
                // Four complete owner buffers: committed/candidate composed poses and frozen interruption sources.
                for (std::size_t buffer = 0; buffer < 4; ++buffer)
                    plan_.stateMachines[index].poses[buffer] = Slot(GraphValueType::Pose);
                for (std::uint32_t stateIndex = 0; stateIndex < machine.states.size(); ++stateIndex) {
                    // Expanding a nested source may grow stateMachines; reacquire its owner only after that expansion.
                    auto branch = StateBranch(machine.states[stateIndex], occurrence, index, stateIndex);
                    plan_.stateMachines[index].branches.push_back(branch);
                }
                occurrence.phase = GraphOccurrencePhase::StateEnd;
                occurrence.outputProducer = static_cast<std::uint32_t>(plan_.occurrences.size());
                plan_.stateMachines[index].endOccurrence = occurrence.outputProducer;
                const Access output{occurrence.output, occurrence.outputProducer};
                Append(std::move(occurrence));
                return output;
            }

            /** @brief Binds ordinary aliases/calls or a new typed writer without changing evaluation order. */
            Access OutputAccess(const GraphInstruction &instruction, GraphInstructionOccurrence &occurrence,
                                const std::map<GraphInterfaceId, Access> &interfaceInputs) {
                Access output;
                if (instruction.definitionIndex.has_value()) {
                    std::map<GraphInterfaceId, Access> inputs;
                    for (const auto &input : occurrence.inputs)
                        inputs.try_emplace(Find(instruction.pins, input.destination)->interfaceId, input.slot, input.sourceOccurrence);
                    auto childPath = occurrence.callPath;
                    childPath.push_back(instruction.source);
                    output = Expand(*instruction.definitionIndex, childPath, occurrence.guards, inputs);
                } else if (const auto *input = std::get_if<GraphInputNode>(&instruction.payload)) {
                    output = interfaceInputs.at(input->input);
                    occurrence.inputs.emplace_back(instruction.pins.front().id, output.slot, output.producer);
                } else if (std::holds_alternative<GraphOutputNode>(instruction.payload)) {
                    output = {occurrence.inputs.front().slot, occurrence.inputs.front().sourceOccurrence};
                } else {
                    const auto pin = std::ranges::find_if(instruction.pins, [](const auto &value) {
                        return value.output;
                    });
                    output = {Slot(pin->type), static_cast<std::uint32_t>(plan_.occurrences.size())};
                }
                return output;
            }

            /** @brief Expands only a compiler-proven acyclic, depth-bounded definition while preserving actual writer aliases. */
            Access Expand(const std::uint32_t definitionIndex, const std::vector<GraphSourceLocation> &path,
                          const std::vector<GraphStateGuard> &guards, const std::map<GraphInterfaceId, Access> &interfaceInputs) {
                Check();
                if (path.size() >= context_.limits.callDepth)
                    throw Rejection{&AnimationErrors::GraphLimitExceeded};
                const auto &definition = program_.Definitions()[definitionIndex];
                std::vector<Access> outputs(definition.instructions.size());
                for (std::uint32_t instructionIndex = 0; instructionIndex < definition.instructions.size(); ++instructionIndex) {
                    const auto &instruction = definition.instructions[instructionIndex];
                    GraphInstructionOccurrence occurrence;
                    occurrence.source = instruction.source;
                    occurrence.callPath = path;
                    occurrence.guards = guards;
                    occurrence.definitionIndex = definitionIndex;
                    occurrence.instructionIndex = instructionIndex;
                    occurrence.parameterRead = instruction.parameterIndex;
                    for (const auto &input : instruction.inputs)
                        occurrence.inputs.emplace_back(input.destination, outputs[input.instruction].slot,
                                                       outputs[input.instruction].producer);
                    if (const auto *clip = std::get_if<GraphClipNode>(&instruction.payload)) {
                        outputs[instructionIndex] = Clip(*clip, std::move(occurrence));
                        continue;
                    }
                    if (const auto *machine = std::get_if<GraphStateMachineNode>(&instruction.payload)) {
                        outputs[instructionIndex] = Machine(*machine, std::move(occurrence));
                        continue;
                    }
                    const auto output = OutputAccess(instruction, occurrence, interfaceInputs);
                    occurrence.output = output.slot;
                    occurrence.outputProducer = output.producer;
                    outputs[instructionIndex] = output;
                    if (auto bound = BindWriterComposition(instruction.payload, occurrence, plan_.memory, context_); bound.HasError())
                        throw BindingRejection{bound.ErrorValue()};
                    Append(std::move(occurrence));
                }
                return outputs[definition.outputInstruction];
            }

            const AnimationGraphProgram &program_;
            const AnimationGraphCompileContext &context_;
            BoundPlan plan_;
            std::uint64_t pathEntries_{};
        };
    }  // namespace

    /** @copydoc BindStateNodes */
    Result<BoundPlan> BindStateNodes(const AnimationGraphProgram &program, const AnimationGraphCompileContext &context,
                                     const GraphSkeletonBinding &skeleton, std::vector<AnimationClipDescriptor> clips) {
        try {
            return Result<BoundPlan>::Success(Builder{program, context, skeleton, std::move(clips)}.Build());
        } catch (const Rejection &rejection) {
            return Result<BoundPlan>::Failure(Failure(*rejection.code));
        } catch (const BindingRejection &rejection) {
            return Result<BoundPlan>::Failure(rejection.error);
        }
    }
}  // namespace Horo::Animation::GraphCompileDetail
