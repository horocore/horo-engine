#include "AnimationClipSceneProgramAccess.h"
#include "AnimationGraphCompilerInternal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <type_traits>

namespace Horo::Animation {
    /** @copydoc AnimationGraphData::AnimationGraphData */
    AnimationGraphData::AnimationGraphData(AnimationGraphData &&other)
        : schemaVersion(other.schemaVersion), contractVersion(other.contractVersion), id(other.id), skeleton(other.skeleton),
          entry(other.entry) {
        parameters.swap(other.parameters);
        definitions.swap(other.definitions);
        actionSlots.swap(other.actionSlots);
        actions.swap(other.actions);
    }

    /** @copydoc GraphCompiledDefinition::GraphCompiledDefinition */
    GraphCompiledDefinition::GraphCompiledDefinition(GraphCompiledDefinition &&other)
        : id(other.id), outputInstruction(other.outputInstruction), callDepth(other.callDepth),
          evaluationInstructions(other.evaluationInstructions), sourceMapCallEntries(other.sourceMapCallEntries) {
        inputs.swap(other.inputs);
        instructions.swap(other.instructions);
    }

    /** @copydoc AnimationGraphCookedData::AnimationGraphCookedData */
    AnimationGraphCookedData::AnimationGraphCookedData(AnimationGraphCookedData &&other)
        : contractVersion(other.contractVersion), id(other.id), skeleton(other.skeleton), entry(other.entry) {
        parameters.swap(other.parameters);
        actionSlots.swap(other.actionSlots);
        actions.swap(other.actions);
        definitions.swap(other.definitions);
    }

    /** @copydoc AnimationGraphProgram::AnimationGraphProgram */
    AnimationGraphProgram::AnimationGraphProgram(AnimationGraphProgram &&other)
        : memory_(other.memory_), skeletonBinding_(other.skeletonBinding_), id_(other.id_), sourceClip_(other.sourceClip_),
          skeleton_(other.skeleton_), entry_(other.entry_) {
        occurrences_.swap(other.occurrences_);
        stateMachines_.swap(other.stateMachines_);
        clipBindings_.swap(other.clipBindings_);
        parameters_.swap(other.parameters_);
        actionSlots_.swap(other.actionSlots_);
        actions_.swap(other.actions_);
        definitions_.swap(other.definitions_);
        clips_.swap(other.clips_);
    }

    /** @copydoc GraphBlendSampleBinding::GraphBlendSampleBinding */
    GraphBlendSampleBinding::GraphBlendSampleBinding(GraphBlendSampleBinding &&other)
        : player(other.player), clip(other.clip), member(other.member) {
        coverage.swap(other.coverage);
    }

    /** @copydoc GraphInstructionOccurrence::GraphInstructionOccurrence */
    GraphInstructionOccurrence::GraphInstructionOccurrence(GraphInstructionOccurrence &&other)
        : source(other.source), definitionIndex(other.definitionIndex), instructionIndex(other.instructionIndex), output(other.output),
          outputProducer(other.outputProducer), clipPlayer(other.clipPlayer), clipBindingIndex(other.clipBindingIndex),
          parameterRead(other.parameterRead), actionSlot(other.actionSlot), stateMachine(other.stateMachine), phase(other.phase),
          blendScratch(other.blendScratch), blendTree(other.blendTree), blendParameters(other.blendParameters) {
        callPath.swap(other.callPath);
        inputs.swap(other.inputs);
        guards.swap(other.guards);
        jointMask.swap(other.jointMask);
        clipCoverage.swap(other.clipCoverage);
        blendSamples.swap(other.blendSamples);
    }

    /** @copydoc GraphStateMachineBinding::GraphStateMachineBinding */
    GraphStateMachineBinding::GraphStateMachineBinding(GraphStateMachineBinding &&other)
        : beginOccurrence(other.beginOccurrence), endOccurrence(other.endOccurrence), conditionOffset(other.conditionOffset),
          poses(other.poses) {
        branches.swap(other.branches);
    }

    namespace GraphCompileDetail {
        /** @copydoc Failure */
        Error Failure(const ErrorCodeDescriptor &code, GraphSourceLocation location) {
            auto error = MakeError(code);
            if (location.definition.IsValid()) {
                error.diagnostics.push_back({DiagnosticCode{code.code.Value()},
                                             DiagnosticSeverity::Error,
                                             error.message,
                                             {"animation.graph", 0, 0},
                                             std::format("/definitions/{}/nodes/{}/pins/{}", location.definition.Value(),
                                                         location.node.Value(), location.pin.Value())});
            }
            return error;
        }

        /** @copydoc Admission */
        Result<void> Admission(const AnimationGraphCompileContext &context) {
            if (!context.accepting)
                return Result<void>::Failure(Failure(AnimationErrors::GraphAdmissionRejected));
            if (context.cancellation.IsCancellationRequested())
                return Result<void>::Failure(Failure(AnimationErrors::GraphOperationCancelled));
            return Result<void>::Success();
        }

    }  // namespace GraphCompileDetail

    namespace {
        using namespace AnimationErrors;
        using namespace GraphCompileDetail;

        /** @brief Orders scalar indices before fallible copies, avoiding noexcept
         * moves of checked-library storage while sorting owned cold graph data. */
        template <typename Value> void CanonicalizeOwnedIds(std::vector<Value> &values) {
            if (std::ranges::is_sorted(values, {}, &Value::id))
                return;
            std::vector<std::size_t> order(values.size());
            for (std::size_t index = 0; index < order.size(); ++index)
                order[index] = index;
            std::ranges::sort(order, {}, [&values](const std::size_t index) {
                return values[index].id;
            });
            std::vector<Value> sorted(std::size_t{0});
            sorted.reserve(values.size());
            for (const auto index : order) {
                const auto &original = values[index];
                sorted.push_back(original);
            }
            values.swap(sorted);
        }

        /** @brief Canonicalizes the inert action catalog without changing authored
         * successor/loop semantics. */
        void CanonicalizeActions(AnimationGraphData &data) {
            CanonicalizeOwnedIds(data.actionSlots);
            CanonicalizeOwnedIds(data.actions);
            for (auto &action : data.actions) {
                CanonicalizeOwnedIds(action.sequence.sections);
                std::ranges::sort(action.sequence.loops, {}, &GraphActionLoopRegion::id);
                std::ranges::sort(action.mask.joints, {}, &GraphBoneWeight::joint);
            }
        }

        /** @brief Canonicalizes semantically unordered authoring collections by stable
         * identities. */
        void Canonicalize(AnimationGraphData &data) {
            CanonicalizeActions(data);
            const auto byId = [](const auto &left, const auto &right) {
                return left.id < right.id;
            };
            CanonicalizeOwnedIds(data.parameters);
            CanonicalizeOwnedIds(data.definitions);
            for (auto &definition : data.definitions) {
                std::ranges::sort(definition.inputs, byId);
                CanonicalizeOwnedIds(definition.nodes);
                std::ranges::sort(definition.connections);
                for (auto &node : definition.nodes) {
                    std::ranges::sort(node.pins, byId);
                    if (auto *machine = std::get_if<GraphStateMachineNode>(&node.payload)) {
                        std::ranges::sort(machine->states, byId);
                        CanonicalizeOwnedIds(machine->transitions);
                    }
                    if (auto *tree = std::get_if<GraphBlendTreeNode>(&node.payload))
                        std::ranges::sort(tree->samples, byId);
                    if (auto *blend = std::get_if<GraphBlendNode>(&node.payload))
                        std::ranges::sort(blend->mask.joints, {}, &GraphBoneWeight::joint);
                }
            }
        }

        /** @brief Rejects new pose policies mislabeled as a legacy node contract. */
        bool LegacyNode(const GraphNode &node) {
            const bool defaults = std::visit([]<typename Payload>(const Payload &payload) {
                if constexpr (std::is_same_v<Payload, GraphClipNode>)
                    return !payload.referencePose;
                else if constexpr (std::is_same_v<Payload, GraphBlendNode>)
                    return payload == GraphBlendNode{};
                else if constexpr (std::is_same_v<Payload, GraphBlendTreeNode>)
                    return false;
                else
                    return true;
            }, node.payload);
            return defaults && std::ranges::none_of(node.pins, [](const GraphPin &pin) {
                return pin.role == GraphPinRole::ReferencePose;
            });
        }

        /** @brief Requires every legacy definition to retain its original opcode
         * defaults. */
        bool LegacyDefaults(const AnimationGraphData &data) {
            return std::ranges::all_of(data.definitions, [](const GraphDefinition &definition) {
                return std::ranges::all_of(definition.nodes, LegacyNode);
            });
        }

        /** @brief Checks scalar defaults without interpreting gameplay values or
         * trigger consumption. */
        bool ValidParameter(const GraphParameter &parameter) {
            const auto asciiLetter = [](const unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            };
            if (!asciiLetter(static_cast<unsigned char>(parameter.name.front())))
                return false;
            for (const unsigned char c : parameter.name)
                if (!asciiLetter(c) && !(c >= '0' && c <= '9'))
                    return false;
            switch (parameter.type) {
                case GraphValueType::Float:
                    return std::holds_alternative<float>(parameter.defaultValue) && std::isfinite(std::get<float>(parameter.defaultValue));
                case GraphValueType::Boolean:
                    return std::holds_alternative<bool>(parameter.defaultValue);
                case GraphValueType::Integer:
                    return std::holds_alternative<std::int32_t>(parameter.defaultValue);
                case GraphValueType::Trigger:
                    return std::holds_alternative<bool>(parameter.defaultValue) && !std::get<bool>(parameter.defaultValue);
                default:
                    return false;
            }
        }

        /** @brief Invocation-local dependency indexes; never retained by the immutable
         * program. */
        struct DefinitionTopology final {
            std::map<GraphNodeId, std::size_t> indexes;
            std::vector<std::vector<std::size_t>> incoming{std::size_t{0}};
            std::vector<std::vector<std::size_t>> outgoing{std::size_t{0}};
            std::vector<std::size_t> indegree = std::vector<std::size_t>(std::size_t{0});
            std::size_t output{};
        };

        /** @brief Validates node shapes, the unique sink and exact definition-input
         * declarations. */
        Result<void> IndexNodes(const GraphDefinition &definition, const AnimationGraphData &data,
                                const AnimationGraphCompileContext &context, DefinitionTopology &topology) {
            const GraphSourceLocation location{definition.id, {}, {}};
            if (definition.nodes.empty() || !UniqueIds(definition.nodes) || !UniqueIds(definition.inputs))
                return Result<void>::Failure(Failure(GraphMalformed, location));
            for (const auto &input : definition.inputs)
                if (input.type >= GraphValueType::Unspecified)
                    return Result<void>::Failure(Failure(GraphTypeMismatch, location));
            std::map<GraphInterfaceId, std::uint32_t> inputUses;
            std::optional<std::size_t> output;
            for (std::size_t index = 0; index < definition.nodes.size(); ++index) {
                if (auto state = Admission(context); state.HasError())
                    return state;
                const auto &node = definition.nodes[index];
                if (auto pins = ValidatePins(node, definition, data); pins.HasError())
                    return pins;
                topology.indexes.emplace(node.id, index);
                if (std::holds_alternative<GraphOutputNode>(node.payload)) {
                    if (output.has_value())
                        return Result<void>::Failure(Failure(GraphMalformed, location));
                    output = index;
                }
                if (const auto *input = std::get_if<GraphInputNode>(&node.payload))
                    ++inputUses[input->input];
            }
            if (!output.has_value() || inputUses.size() != definition.inputs.size())
                return Result<void>::Failure(Failure(GraphMalformed, location));
            for (const auto &[id, uses] : inputUses)
                if (uses != 1)
                    return Result<void>::Failure(Failure(GraphMalformed, location));
            topology.output = *output;
            return Result<void>::Success();
        }

        /** @brief Indexes one edge only after validating both endpoint directions and
         * its exact type. */
        Result<void> IndexEdge(const GraphDefinition &definition, std::size_t index, DefinitionTopology &topology,
                               std::set<GraphEndpoint> &destinations) {
            const auto &edge = definition.connections[index];
            const auto source = topology.indexes.find(edge.source.node);
            const auto destination = topology.indexes.find(edge.destination.node);
            const GraphSourceLocation location{definition.id, edge.destination.node, edge.destination.pin};
            if (source == topology.indexes.end() || destination == topology.indexes.end())
                return Result<void>::Failure(Failure(GraphMalformed, location));
            const auto *sourcePin = Find(definition.nodes[source->second].pins, edge.source.pin);
            const auto *destinationPin = Find(definition.nodes[destination->second].pins, edge.destination.pin);
            if (!sourcePin || !destinationPin || !sourcePin->output || destinationPin->output ||
                !destinations.insert(edge.destination).second)
                return Result<void>::Failure(Failure(GraphMalformed, location));
            if (edge.type != sourcePin->type || edge.type != destinationPin->type)
                return Result<void>::Failure(Failure(GraphTypeMismatch, location));
            topology.incoming[destination->second].push_back(index);
            topology.outgoing[source->second].push_back(destination->second);
            ++topology.indegree[destination->second];
            return Result<void>::Success();
        }

        /** @brief Builds bounded adjacency tables and rejects every unconnected
         * required input. */
        Result<void> IndexEdges(const GraphDefinition &definition, const AnimationGraphCompileContext &context,
                                DefinitionTopology &topology) {
            topology.incoming.resize(definition.nodes.size(), std::vector<std::size_t>(std::size_t{0}));
            topology.outgoing.resize(definition.nodes.size(), std::vector<std::size_t>(std::size_t{0}));
            topology.indegree.resize(definition.nodes.size());
            std::set<GraphEndpoint> destinations;
            for (std::size_t index = 0; index < definition.connections.size(); ++index) {
                if (auto state = Admission(context); state.HasError())
                    return state;
                if (auto edge = IndexEdge(definition, index, topology, destinations); edge.HasError())
                    return edge;
            }
            for (const auto &node : definition.nodes)
                for (const auto &pin : node.pins)
                    if (!pin.output && !destinations.contains({node.id, pin.id}))
                        return Result<void>::Failure(Failure(GraphMalformed, {definition.id, node.id, pin.id}));
            return Result<void>::Success();
        }

        /** @brief Checks that every node contributes to the sink, without recursive
         * stack growth. */
        Result<void> ValidateReachability(const GraphDefinition &definition, const DefinitionTopology &topology,
                                          const AnimationGraphCompileContext &context) {
            std::vector<bool> reached(definition.nodes.size());
            std::vector pending{topology.output};
            reached[topology.output] = true;
            while (!pending.empty()) {
                if (auto state = Admission(context); state.HasError())
                    return state;
                const auto index = pending.back();
                pending.pop_back();
                for (const auto edgeIndex : topology.incoming[index]) {
                    const auto source = topology.indexes.at(definition.connections[edgeIndex].source.node);
                    if (!reached[source]) {
                        reached[source] = true;
                        pending.push_back(source);
                    }
                }
            }
            if (std::ranges::find(reached, false) != reached.end())
                return Result<void>::Failure(Failure(GraphMalformed, {definition.id, {}, {}}));
            return Result<void>::Success();
        }

        /** @brief Copies a validated payload directly into a detached owner.
         * @param destination Unpublished payload distinct from source.
         * @param source Validated payload to copy.
         * @throws std::bad_alloc When payload storage cannot be allocated. */
        void CopyGraphPayload(GraphNodePayload &destination, const GraphNodePayload &source) {
            // Variant copy assignment can move a temporary through allocating checked-STL noexcept constructors.
            std::visit([&destination]<typename Payload>(const Payload &payload) {
                destination.emplace<Payload>(payload);
            }, source);
        }

        /** @brief Materializes exact typed operands after their source instructions
         * have been emitted. */
        void EmitInstruction(const GraphDefinition &definition, const AnimationGraphData &data, const DefinitionTopology &topology,
                             std::size_t index, const std::vector<std::uint32_t> &registers, GraphInstruction &instruction) {
            const auto &node = definition.nodes[index];
            instruction.source = {definition.id, node.id, {}};
            CopyGraphPayload(instruction.payload, node.payload);
            instruction.pins = node.pins;
            for (const auto edgeIndex : topology.incoming[index]) {
                const auto &edge = definition.connections[edgeIndex];
                instruction.inputs.push_back({edge.destination.pin,
                                              registers[topology.indexes.at(edge.source.node)],
                                              {definition.id, edge.source.node, edge.source.pin},
                                              edge.type});
            }
            std::ranges::sort(instruction.inputs, {}, &GraphCompiledInput::destination);
            if (const auto *parameter = std::get_if<GraphParameterNode>(&node.payload))
                instruction.parameterIndex =
                    static_cast<std::uint32_t>(Find(data.parameters, parameter->parameter) - data.parameters.data());
            if (const auto *tree = std::get_if<GraphBlendTreeNode>(&node.payload)) {
                instruction.blendParameters[0] = static_cast<std::uint32_t>(Find(data.parameters, tree->x) - data.parameters.data());
                if (tree->dimension == GraphBlendDimension::Two)
                    instruction.blendParameters[1] = static_cast<std::uint32_t>(Find(data.parameters, tree->y) - data.parameters.data());
            }
            if (const auto *slot = std::get_if<GraphActionSlotNode>(&node.payload))
                instruction.actionSlot = static_cast<std::uint32_t>(Find(data.actionSlots, slot->slot) - data.actionSlots.data());
            if (const auto *call = std::get_if<GraphCallNode>(&node.payload))
                instruction.definitionIndex =
                    static_cast<std::uint32_t>(Find(data.definitions, call->definition) - data.definitions.data());
            if (const auto *machine = std::get_if<GraphStateMachineNode>(&node.payload))
                for (const auto &state : machine->states)
                    if (const auto *definitionId = std::get_if<GraphDefinitionId>(&state.source))
                        instruction.stateDefinitions.push_back(
                            static_cast<std::uint32_t>(Find(data.definitions, *definitionId) - data.definitions.data()));
        }

        /** @brief Builds a stable-id topological schedule and exact operand/source map
         * for one definition. */
        Result<GraphCompiledDefinition> CompileDefinition(const GraphDefinition &definition, const AnimationGraphData &data,
                                                          const AnimationGraphCompileContext &context) {
            DefinitionTopology topology;
            if (auto nodes = IndexNodes(definition, data, context, topology); nodes.HasError())
                return Result<GraphCompiledDefinition>::Failure(nodes.ErrorValue());
            if (auto edges = IndexEdges(definition, context, topology); edges.HasError())
                return Result<GraphCompiledDefinition>::Failure(edges.ErrorValue());
            std::set<std::size_t> ready;
            for (std::size_t index = 0; index < definition.nodes.size(); ++index)
                if (topology.indegree[index] == 0)
                    ready.insert(index);
            GraphCompiledDefinition result;
            result.id = definition.id;
            result.inputs = definition.inputs;
            result.instructions.reserve(definition.nodes.size());
            std::vector<std::uint32_t> registers(definition.nodes.size());
            while (!ready.empty()) {
                if (auto state = Admission(context); state.HasError())
                    return Result<GraphCompiledDefinition>::Failure(state.ErrorValue());
                const auto index = *ready.begin();
                ready.erase(ready.begin());
                const auto registerIndex = static_cast<std::uint32_t>(result.instructions.size());
                auto &instruction = result.instructions.emplace_back();
                EmitInstruction(definition, data, topology, index, registers, instruction);
                registers[index] = registerIndex;
                for (const auto next : topology.outgoing[index])
                    if (--topology.indegree[next] == 0)
                        ready.insert(next);
            }
            if (result.instructions.size() != definition.nodes.size())
                return Result<GraphCompiledDefinition>::Failure(Failure(GraphCycle, {definition.id, {}, {}}));
            if (auto reachable = ValidateReachability(definition, topology, context); reachable.HasError())
                return Result<GraphCompiledDefinition>::Failure(reachable.ErrorValue());
            result.outputInstruction = registers[topology.output];
            return Result<GraphCompiledDefinition>::Success(std::move(result));
        }

        /** @brief State branches charge both admitted lanes, control completion and
         * full guarded call provenance. */
        void AccumulateStateBudget(GraphCompiledDefinition &definition, std::span<const GraphCompiledDefinition> definitions) {
            for (const auto &instruction : definition.instructions) {
                const auto *machine = std::get_if<GraphStateMachineNode>(&instruction.payload);
                if (!machine)
                    continue;
                ++definition.evaluationInstructions;  // Control precedes guarded children
                                                      // and pose completion.
                const auto clipStates = static_cast<std::uint64_t>(std::ranges::count_if(machine->states, [](const GraphState &state) {
                    return std::holds_alternative<AnimationClipId>(state.source);
                }));
                definition.evaluationInstructions += 2 * clipStates;
                definition.sourceMapCallEntries += 4 * clipStates;
                if (clipStates != 0)
                    definition.callDepth = std::max(definition.callDepth, 2U);
                for (const auto targetIndex : instruction.stateDefinitions) {
                    const auto &target = definitions[targetIndex];
                    definition.callDepth = std::max(definition.callDepth, target.callDepth + 1);
                    definition.evaluationInstructions += 2 * target.evaluationInstructions;
                    definition.sourceMapCallEntries += 2 * (target.sourceMapCallEntries + 2 * target.evaluationInstructions);
                }
            }
        }

        /** @brief Propagates completed callee costs into one definition under exact
         * captured limits. */
        Result<void> AccumulateCallBudget(GraphCompiledDefinition &definition, std::span<const GraphCompiledDefinition> definitions,
                                          const AnimationGraphCompileContext &context) {
            definition.callDepth = 1;
            definition.evaluationInstructions = definition.instructions.size();
            for (const auto &instruction : definition.instructions)
                if (instruction.definitionIndex.has_value()) {
                    const auto &target = definitions[*instruction.definitionIndex];
                    definition.callDepth = std::max(definition.callDepth, target.callDepth + 1);
                    definition.evaluationInstructions += target.evaluationInstructions;
                    definition.sourceMapCallEntries += target.sourceMapCallEntries + target.evaluationInstructions;
                }
            AccumulateStateBudget(definition, definitions);
            if (definition.callDepth > context.limits.callDepth ||
                definition.evaluationInstructions > context.limits.evaluationInstructions ||
                definition.sourceMapCallEntries > context.limits.sourceMapCallEntries)
                return Result<void>::Failure(Failure(GraphLimitExceeded, {definition.id, {}, {}}));
            return Result<void>::Success();
        }

        /** @brief Builds one canonical dependency relation for ordinary calls and every
         * state subgraph. */
        void IndexCallDependencies(const std::span<const GraphCompiledDefinition> definitions,
                                   std::vector<std::set<std::size_t>> &dependencies, std::vector<std::set<std::size_t>> &callers) {
            for (std::size_t index = 0; index < definitions.size(); ++index)
                for (const auto &instruction : definitions[index].instructions) {
                    if (instruction.definitionIndex.has_value()) {
                        dependencies[index].insert(*instruction.definitionIndex);
                        callers[*instruction.definitionIndex].insert(index);
                    }
                    for (const auto target : instruction.stateDefinitions) {
                        dependencies[index].insert(target);
                        callers[target].insert(index);
                    }
                }
        }

        /** @brief Rejects recursive calls and budgets expanded execution without
         * expanding stored programs. */
        Result<void> BoundCalls(std::vector<GraphCompiledDefinition> &definitions, const AnimationGraphCompileContext &context) {
            std::vector<std::set<std::size_t>> dependencies(definitions.size());
            std::vector<std::set<std::size_t>> callers(definitions.size());
            IndexCallDependencies(definitions, dependencies, callers);
            std::set<std::size_t> ready;
            for (std::size_t index = 0; index < definitions.size(); ++index)
                if (dependencies[index].empty())
                    ready.insert(index);
            std::size_t completed{};
            while (!ready.empty()) {
                if (auto state = Admission(context); state.HasError())
                    return state;
                const auto index = *ready.begin();
                ready.erase(ready.begin());
                if (auto budget = AccumulateCallBudget(definitions[index], definitions, context); budget.HasError())
                    return budget;
                ++completed;
                for (const auto caller : callers[index]) {
                    dependencies[caller].erase(index);
                    if (dependencies[caller].empty())
                        ready.insert(caller);
                }
            }
            if (completed != definitions.size())
                return Result<void>::Failure(Failure(GraphCycle));
            return Result<void>::Success();
        }

        /** @brief Infers legacy edge types on a detached definition while preserving
         * cancellation and error order. */
        Result<void> MigrateConnections(GraphDefinition &definition, const AnimationGraphCompileContext &context) {
            for (auto &edge : definition.connections) {
                if (auto state = Admission(context); state.HasError())
                    return state;
                const auto *node = Find(definition.nodes, edge.source.node);
                const auto *pin = node ? Find(node->pins, edge.source.pin) : nullptr;
                if (!pin || edge.type != GraphValueType::Unspecified)
                    return Result<void>::Failure(Failure(GraphMalformed, {definition.id, edge.source.node, edge.source.pin}));
                edge.type = pin->type;
            }
            return Result<void>::Success();
        }

        /** @brief Canonical sorted ID tables reject duplicate persistent declarations.
         */
        Result<void> ValidateCanonicalIdentities(const AnimationGraphData &canonical) {
            if (!UniqueIds(canonical.definitions) || !UniqueIds(canonical.parameters))
                return Result<void>::Failure(Failure(GraphMalformed));
            return Result<void>::Success();
        }

        /** @brief Resolves the one closed entry definition to its canonical stable
         * program index. */
        Result<std::uint32_t> ResolveEntryIndex(const AnimationGraphData &canonical) {
            const auto *entry = Find(canonical.definitions, canonical.entry);
            if (!entry || !entry->inputs.empty())
                return Result<std::uint32_t>::Failure(Failure(GraphMalformed));
            return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(entry - canonical.definitions.data()));
        }

        /** @brief All typed parameter declarations and names are validated before
         * compiling any definition. */
        Result<void> ValidateCanonicalParameters(const AnimationGraphData &canonical) {
            std::set<std::string, std::less<>> names;
            for (const auto &parameter : canonical.parameters)
                if (!ValidParameter(parameter) || !names.insert(parameter.name).second)
                    return Result<void>::Failure(Failure(GraphTypeMismatch));
            return Result<void>::Success();
        }

        /** @brief Projects one typed opcode's direct clips, including every inactive
         * state source. */
        void CollectPayloadClips(const GraphNodePayload &opcode, std::set<AnimationClipId> &clips) {
            std::visit([&clips]<typename Payload>(const Payload &payload) {
                if constexpr (std::is_same_v<Payload, GraphClipNode>)
                    clips.insert(payload.clip);
                else if constexpr (std::is_same_v<Payload, GraphBlendTreeNode>)
                    for (const auto &sample : payload.samples)
                        clips.insert(sample.clip);
                else if constexpr (std::is_same_v<Payload, GraphStateMachineNode>)
                    for (const auto &state : payload.states)
                        if (const auto *clip = std::get_if<AnimationClipId>(&state.source))
                            clips.insert(*clip);
            }, opcode);
        }

        /** @brief Collects each compiled definition's immutable dependency set without
         * selecting active branches. */
        void CollectClips(const GraphCompiledDefinition &definition, std::set<AnimationClipId> &clips) {
            for (const auto &instruction : definition.instructions)
                CollectPayloadClips(instruction.payload, clips);
        }

        /** @brief Compiles the complete detached definition set and collects even
         * inactive clip dependencies before publication. */
        Result<void> CompileDefinitions(const AnimationGraphData &canonical, const AnimationGraphCompileContext &context,
                                        std::set<AnimationClipId> &clips, std::vector<GraphCompiledDefinition> &definitions) {
            definitions.reserve(canonical.definitions.size());
            for (const auto &definition : canonical.definitions) {
                auto compiled = CompileDefinition(definition, canonical, context);
                if (compiled.HasError())
                    return Result<void>::Failure(compiled.ErrorValue());
                CollectClips(compiled.Value(), clips);
                definitions.push_back(std::move(compiled).Value());
            }
            return Result<void>::Success();
        }

        /** @brief State authoring payloads are not valid in any pre-state source
         * schema. */
        bool HasStatePayload(const AnimationGraphData &data) {
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    if (std::holds_alternative<GraphStateMachineNode>(node.payload))
                        return true;
            return false;
        }

        /** @brief Exact semantic contract corresponding to an already range-admitted
         * authoring schema. */
        AnimationGraphContractVersion SourceContract(const std::uint32_t schema) noexcept {
            if (schema <= 2)
                return {1, 0, 0};
            if (schema == 3)
                return {1, 1, 0};
            if (schema == 4)
                return {1, 2, 0};
            if (schema == 5)
                return {1, 3, 0};
            return CurrentAnimationGraphContractVersion;
        }

        /** @brief Action authoring cannot be mislabeled as a legacy contract. */
        bool HasActionPayload(const AnimationGraphData &data) {
            if (!data.actions.empty() || !data.actionSlots.empty())
                return true;
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    if (std::holds_alternative<GraphActionSlotNode>(node.payload))
                        return true;
            return false;
        }

        /** @brief Blend-space semantics retain their exact introduction version. */
        bool HasBlendPayload(const AnimationGraphData &data) {
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    if (std::holds_alternative<GraphBlendTreeNode>(node.payload))
                        return true;
            return false;
        }

        /** @brief Each admitted source schema has exactly one semantic contract; pose
         * policies remain unchanged in schema 3. */
        bool ValidSourceVersion(const AnimationGraphData &data) {
            if (data.schemaVersion == 0 || data.schemaVersion > CurrentAnimationGraphSchemaVersion)
                return false;
            if (const auto expected = SourceContract(data.schemaVersion); data.contractVersion != expected)
                return false;
            if (data.schemaVersion < 6 && HasActionPayload(data))
                return false;
            if (data.schemaVersion < 5 && HasBlendPayload(data))
                return false;
            return data.schemaVersion >= 4 || !HasStatePayload(data);
        }

        /** @brief Validates both typed namespaces before emitting any instruction. */
        Result<void> ValidateCanonicalCatalog(const AnimationGraphData &data) {
            if (const auto parameters = ValidateCanonicalParameters(data); parameters.HasError())
                return parameters;
            return ValidateActions(data);
        }

        /** @brief Every immutable action section remains an exact clip dependency, including inactive requests. */
        void CollectActionClips(const AnimationGraphData &data, std::set<AnimationClipId> &clips) {
            for (const auto &action : data.actions)
                for (const auto &section : action.sequence.sections)
                    clips.insert(section.clip);
        }

        /** @brief Projects supplied instructions into the existing semantic validator, never selecting an execution order. */
        void CookedValidationDefinition(const GraphCompiledDefinition &compiled, GraphDefinition &result) {
            result.id = compiled.id;
            result.inputs = compiled.inputs;
            result.nodes.reserve(compiled.instructions.size());
            for (const auto &instruction : compiled.instructions) {
                auto &node = result.nodes.emplace_back();
                node.id = instruction.source.node;
                CopyGraphPayload(node.payload, instruction.payload);
                node.pins = instruction.pins;
                for (const auto &input : instruction.inputs)
                    result.connections.push_back(
                        {{input.source.node, input.source.pin}, {instruction.source.node, input.destination}, input.type});
            }
        }

        /** @brief Builds bounded validation-only declarations after whole-candidate cold admission. */
        AnimationGraphData CookedValidationModel(const AnimationGraphCookedData &data) {
            AnimationGraphData model;
            model.id = data.id;
            model.skeleton = data.skeleton;
            model.entry = data.entry;
            model.parameters = data.parameters;
            model.actionSlots = data.actionSlots;
            model.actions = data.actions;
            model.definitions.reserve(data.definitions.size());
            for (const auto &definition : data.definitions) {
                auto &destination = model.definitions.emplace_back();
                CookedValidationDefinition(definition, destination);
            }
            Canonicalize(model);
            return model;
        }

        /** @brief Cooked catalogs must already use the same canonical ordering as compiler output. */
        bool CookedCatalogMatches(const AnimationGraphCookedData &data, const AnimationGraphData &model) {
            return data.parameters == model.parameters && data.actionSlots == model.actionSlots && data.actions == model.actions;
        }

        /** @brief Runs the shared semantic topology checks without compiling or replacing the supplied schedule. */
        Result<void> CookedTopology(const GraphDefinition &definition, const AnimationGraphData &model,
                                    const AnimationGraphCompileContext &context, DefinitionTopology &topology) {
            if (auto nodes = IndexNodes(definition, model, context, topology); nodes.HasError())
                return nodes;
            if (auto edges = IndexEdges(definition, context, topology); edges.HasError())
                return edges;
            return ValidateReachability(definition, topology, context);
        }

        /** @brief Verifies one stored instruction against the existing canonical operand encoder. */
        Result<std::size_t> VerifyCookedInstruction(const GraphInstruction &instruction, const GraphDefinition &definition,
                                                    const AnimationGraphData &model, const DefinitionTopology &topology,
                                                    const std::vector<std::uint32_t> &registers, const std::set<std::size_t> &ready) {
            const auto found = topology.indexes.find(instruction.source.node);
            if (found == topology.indexes.end() || ready.empty() || found->second != *ready.begin())
                return Result<std::size_t>::Failure(Failure(GraphMalformed, instruction.source));
            // This checks already supplied code. It neither chooses a schedule nor emits a replacement program.
            GraphInstruction expected;
            EmitInstruction(definition, model, topology, found->second, registers, expected);
            if (expected != instruction)
                return Result<std::size_t>::Failure(Failure(GraphMalformed, instruction.source));
            return Result<std::size_t>::Success(found->second);
        }

        /** @brief Advances the validation frontier only after the exact stored instruction has been checked. */
        void AdvanceCookedFrontier(DefinitionTopology &topology, const std::size_t node, std::set<std::size_t> &ready) {
            ready.erase(node);
            for (const auto next : topology.outgoing[node])
                if (--topology.indegree[next] == 0)
                    ready.insert(next);
        }

        /** @brief Checks source provenance, operand indexes and the deterministic supplied topological order. */
        Result<void> VerifyCookedSchedule(const GraphCompiledDefinition &compiled, const GraphDefinition &definition,
                                          const AnimationGraphData &model, const AnimationGraphCompileContext &context) {
            DefinitionTopology topology;
            if (auto checked = CookedTopology(definition, model, context, topology); checked.HasError())
                return checked;
            std::set<std::size_t> ready;
            for (std::size_t index = 0; index < topology.indegree.size(); ++index)
                if (topology.indegree[index] == 0)
                    ready.insert(index);
            std::vector<std::uint32_t> registers(definition.nodes.size());
            for (std::size_t index = 0; index < compiled.instructions.size(); ++index) {
                if (auto admission = Admission(context); admission.HasError())
                    return admission;
                auto node = VerifyCookedInstruction(compiled.instructions[index], definition, model, topology, registers, ready);
                if (node.HasError())
                    return Result<void>::Failure(node.ErrorValue());
                registers[node.Value()] = static_cast<std::uint32_t>(index);
                AdvanceCookedFrontier(topology, node.Value(), ready);
            }
            if (compiled.outputInstruction != registers[topology.output])
                return Result<void>::Failure(Failure(GraphMalformed));
            return Result<void>::Success();
        }

        /** @brief Verifies every canonical definition before any stored index reaches binding or call expansion. */
        Result<void> VerifyCookedDefinitions(const AnimationGraphCookedData &data, const AnimationGraphData &model,
                                             const AnimationGraphCompileContext &context) {
            for (std::size_t index = 0; index < data.definitions.size(); ++index) {
                const auto &compiled = data.definitions[index];
                const auto &definition = model.definitions[index];
                if (compiled.id != definition.id || compiled.inputs != definition.inputs)
                    return Result<void>::Failure(Failure(GraphMalformed));
                if (auto checked = VerifyCookedSchedule(compiled, definition, model, context); checked.HasError())
                    return checked;
            }
            return Result<void>::Success();
        }

        /** @brief Verifies claimed expansion costs against the one existing bounded call-graph accounting path. */
        Result<void> VerifyCookedCosts(std::vector<GraphCompiledDefinition> &definitions, const AnimationGraphCookedData &data,
                                       const AnimationGraphCompileContext &context) {
            for (auto &definition : definitions)
                definition.sourceMapCallEntries = 0;
            if (auto bounded = BoundCalls(definitions, context); bounded.HasError())
                return bounded;
            for (std::size_t index = 0; index < definitions.size(); ++index) {
                const auto &actual = definitions[index];
                const auto &claimed = data.definitions[index];
                if (actual.callDepth != claimed.callDepth || actual.evaluationInstructions != claimed.evaluationInstructions ||
                    actual.sourceMapCallEntries != claimed.sourceMapCallEntries)
                    return Result<void>::Failure(Failure(GraphMalformed));
            }
            return Result<void>::Success();
        }

        /** @brief Admits an exact current contract and canonical semantic namespace without a source compiler call. */
        Result<AnimationGraphData> ValidateCookedModel(const AnimationGraphCookedData &data, const AnimationGraphCompileContext &context) {
            if (data.contractVersion != CurrentAnimationGraphContractVersion)
                return Result<AnimationGraphData>::Failure(Failure(GraphVersionUnsupported));
            if (auto admitted = PreflightCookedGraph(data, context); admitted.HasError())
                return Result<AnimationGraphData>::Failure(admitted.ErrorValue());
            auto model = CookedValidationModel(data);
            if (auto identities = ValidateCanonicalIdentities(model); identities.HasError())
                return Result<AnimationGraphData>::Failure(identities.ErrorValue());
            if (!CookedCatalogMatches(data, model))
                return Result<AnimationGraphData>::Failure(Failure(GraphMalformed));
            if (auto catalog = ValidateCanonicalCatalog(model); catalog.HasError())
                return Result<AnimationGraphData>::Failure(catalog.ErrorValue());
            if (auto definitions = VerifyCookedDefinitions(data, model, context); definitions.HasError())
                return Result<AnimationGraphData>::Failure(definitions.ErrorValue());
            return Result<AnimationGraphData>::Success(std::move(model));
        }

        /** @brief Defines execution-local operands only; these IDs never claim a persistent graph asset. */
        AnimationGraphData DirectClipDeclarations(const AnimationClipId clip, const SkeletonId skeleton) {
            using enum Horo::Animation::GraphValueType;
            AnimationGraphData model;
            model.skeleton = skeleton;
            model.entry = GraphDefinitionId::Create(1).Value();
            const auto sample = GraphNodeId::Create(1).Value();
            const auto output = GraphNodeId::Create(2).Value();
            const auto pin = GraphPinId::Create(1).Value();
            GraphDefinition definition{.id = model.entry};
            definition.nodes = {{sample, GraphClipNode{clip, {}}, {{pin, GraphPinRole::Result, Pose, true}}},
                                {output, GraphOutputNode{}, {{pin, GraphPinRole::Value, Pose, false}}}};
            definition.connections = {{{sample, pin}, {output, pin}, Pose}};
            model.definitions.push_back(std::move(definition));
            return model;
        }

        /** @brief Publishes a complete detached schema or bound program only after the
         * final admission/cancellation fence. */
        Result<AnimationGraphProgram> PublishProgram(AnimationGraphProgram program, const AnimationGraphCompileContext &context) {
            if (auto state = Admission(context); state.HasError())
                return Result<AnimationGraphProgram>::Failure(state.ErrorValue());
            return Result<AnimationGraphProgram>::Success(std::move(program));
        }
    }  // namespace

    /** @copydoc AnimationGraphProgram::CompileSchema */
    Result<AnimationGraphProgram> AnimationGraphProgram::CompileSchema(const AnimationGraphData &data,
                                                                       const AnimationGraphCompileContext &context) {
        if (data.schemaVersion != CurrentAnimationGraphSchemaVersion || data.contractVersion != CurrentAnimationGraphContractVersion)
            return Result<AnimationGraphProgram>::Failure(Failure(GraphVersionUnsupported));
        if (auto preflight = PreflightGraph(data, context); preflight.HasError())
            return Result<AnimationGraphProgram>::Failure(preflight.ErrorValue());
        auto canonical = data;
        Canonicalize(canonical);
        if (auto identities = ValidateCanonicalIdentities(canonical); identities.HasError())
            return Result<AnimationGraphProgram>::Failure(identities.ErrorValue());
        const auto entry = ResolveEntryIndex(canonical);
        if (entry.HasError())
            return Result<AnimationGraphProgram>::Failure(entry.ErrorValue());
        if (const auto catalog = ValidateCanonicalCatalog(canonical); catalog.HasError())
            return Result<AnimationGraphProgram>::Failure(catalog.ErrorValue());
        AnimationGraphProgram result;
        result.actionSlots_ = canonical.actionSlots;
        result.actions_ = canonical.actions;
        result.id_ = canonical.id;
        result.skeleton_ = canonical.skeleton;
        result.parameters_ = canonical.parameters;
        result.entry_ = entry.Value();
        std::set<AnimationClipId> clips;
        auto definitions = CompileDefinitions(canonical, context, clips, result.definitions_);
        if (definitions.HasError())
            return Result<AnimationGraphProgram>::Failure(definitions.ErrorValue());
        if (auto calls = BoundCalls(result.definitions_, context); calls.HasError())
            return Result<AnimationGraphProgram>::Failure(calls.ErrorValue());
        CollectActionClips(canonical, clips);
        result.clips_.assign(clips.begin(), clips.end());
        return PublishProgram(std::move(result), context);
    }

    /** @copydoc AnimationGraphProgram::Compile */
    Result<AnimationGraphProgram> AnimationGraphProgram::Compile(const AnimationGraphData &data,
                                                                 const AnimationGraphCompileContext &context) {
        auto schema = CompileSchema(data, context);
        if (schema.HasError())
            return schema;
        return BindProgram(std::move(schema).Value(), context);
    }

    /** @copydoc AnimationGraphProgram::BindProgram */
    Result<AnimationGraphProgram> AnimationGraphProgram::BindProgram(AnimationGraphProgram result,
                                                                     const AnimationGraphCompileContext &context) {
        auto bound = GraphCompileDetail::Bind(result, context);
        if (bound.HasError())
            return Result<AnimationGraphProgram>::Failure(bound.ErrorValue());
        result.skeletonBinding_ = bound.Value().skeleton;
        result.memory_ = bound.Value().memory;
        auto owned = std::move(bound).Value();
        result.clipBindings_ = std::move(owned.clips);
        result.occurrences_ = std::move(owned.occurrences);
        result.stateMachines_ = std::move(owned.stateMachines);
        return PublishProgram(std::move(result), context);
    }

    /** @copydoc AnimationGraphProgram::FromCooked */
    Result<AnimationGraphProgram> AnimationGraphProgram::FromCooked(const AnimationGraphCookedData &data,
                                                                    const AnimationGraphCompileContext &context) {
        auto model = ValidateCookedModel(data, context);
        if (model.HasError())
            return Result<AnimationGraphProgram>::Failure(model.ErrorValue());
        const auto entry = ResolveEntryIndex(model.Value());
        if (entry.HasError())
            return Result<AnimationGraphProgram>::Failure(entry.ErrorValue());
        AnimationGraphProgram result;
        result.id_ = data.id;
        result.skeleton_ = data.skeleton;
        result.entry_ = entry.Value();
        result.parameters_ = data.parameters;
        result.actionSlots_ = data.actionSlots;
        result.actions_ = data.actions;
        result.definitions_ = data.definitions;
        if (auto costs = VerifyCookedCosts(result.definitions_, data, context); costs.HasError())
            return Result<AnimationGraphProgram>::Failure(costs.ErrorValue());
        std::set<AnimationClipId> clips;
        for (const auto &definition : result.definitions_)
            CollectClips(definition, clips);
        CollectActionClips(model.Value(), clips);
        result.clips_.assign(clips.begin(), clips.end());
        return BindProgram(std::move(result), context);
    }

    /** @copydoc AnimationGraphProgram::PrepareClip */
    Result<AnimationGraphProgram> AnimationGraphProgram::PrepareClip(const AnimationClipId clip,
                                                                     const AnimationGraphCompileContext &context) {
        if (auto admitted = PreflightClipProgram(clip, context); admitted.HasError())
            return Result<AnimationGraphProgram>::Failure(admitted.ErrorValue());
        const auto declarations = DirectClipDeclarations(clip, context.skeleton->Data().skeleton);
        auto definition = CompileDefinition(declarations.definitions.front(), declarations, context);
        if (definition.HasError())
            return Result<AnimationGraphProgram>::Failure(definition.ErrorValue());
        AnimationGraphProgram result;
        result.sourceClip_ = clip;
        result.skeleton_ = declarations.skeleton;
        result.definitions_.push_back(std::move(definition).Value());
        result.clips_.push_back(clip);
        if (auto costs = BoundCalls(result.definitions_, context); costs.HasError())
            return Result<AnimationGraphProgram>::Failure(costs.ErrorValue());
        return BindProgram(std::move(result), context);
    }

    /** @copydoc Detail::ClipSceneProgramAccess::Prepare */
    Result<AnimationGraphProgram> Detail::ClipSceneProgramAccess::Prepare(const AnimationClipId clip,
                                                                          const AnimationGraphCompileContext &context) {
        return AnimationGraphProgram::PrepareClip(clip, context);
    }

    /** @copydoc MigrateAnimationGraph */
    Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &data, const AnimationGraphCompileContext &context) {
        const bool legacy = data.schemaVersion == 1 || data.schemaVersion == 2;
        if (!ValidSourceVersion(data))
            return Result<AnimationGraphData>::Failure(Failure(GraphVersionUnsupported));
        if (auto preflight = PreflightGraph(data, context); preflight.HasError())
            return Result<AnimationGraphData>::Failure(preflight.ErrorValue());
        auto migrated = data;
        if (legacy && !LegacyDefaults(data))
            return Result<AnimationGraphData>::Failure(Failure(GraphVersionUnsupported));
        Canonicalize(migrated);
        if (migrated.schemaVersion == 1) {
            for (auto &definition : migrated.definitions)
                if (auto connections = MigrateConnections(definition, context); connections.HasError())
                    return Result<AnimationGraphData>::Failure(connections.ErrorValue());
        }
        migrated.schemaVersion = CurrentAnimationGraphSchemaVersion;
        migrated.contractVersion = CurrentAnimationGraphContractVersion;
        if (auto checked = AnimationGraphProgram::CompileSchema(migrated, context); checked.HasError())
            return Result<AnimationGraphData>::Failure(checked.ErrorValue());
        return Result<AnimationGraphData>::Success(std::move(migrated));
    }
}  // namespace Horo::Animation
