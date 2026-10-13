#include "AnimationGraphCompilerInternal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <type_traits>

namespace Horo::Animation {
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

        /** @brief Canonicalizes semantically unordered authoring collections by stable identities. */
        void Canonicalize(AnimationGraphData &data) {
            const auto byId = [](const auto &left, const auto &right) {
                return left.id < right.id;
            };
            std::ranges::sort(data.parameters, byId);
            std::ranges::sort(data.definitions, byId);
            for (auto &definition : data.definitions) {
                std::ranges::sort(definition.inputs, byId);
                std::ranges::sort(definition.nodes, byId);
                std::ranges::sort(definition.connections);
                for (auto &node : definition.nodes) {
                    std::ranges::sort(node.pins, byId);
                    if (auto *machine = std::get_if<GraphStateMachineNode>(&node.payload)) {
                        std::ranges::sort(machine->states, byId);
                        std::ranges::sort(machine->transitions, byId);
                    }
                    if (auto *blend = std::get_if<GraphBlendNode>(&node.payload))
                        std::ranges::sort(blend->mask.joints, {}, &GraphBoneWeight::joint);
                }
            }
        }

        /** @brief Rejects new pose policies mislabeled as a legacy node contract. */
        bool LegacyNode(const GraphNode &node) {
            const bool defaults = std::visit([](const auto &payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, GraphClipNode>)
                    return !payload.referencePose;
                else if constexpr (std::is_same_v<Payload, GraphBlendNode>)
                    return payload == GraphBlendNode{};
                else
                    return true;
            }, node.payload);
            return defaults && std::ranges::none_of(node.pins, [](const GraphPin &pin) {
                return pin.role == GraphPinRole::ReferencePose;
            });
        }

        /** @brief Requires every legacy definition to retain its original opcode defaults. */
        bool LegacyDefaults(const AnimationGraphData &data) {
            return std::ranges::all_of(data.definitions, [](const GraphDefinition &definition) {
                return std::ranges::all_of(definition.nodes, LegacyNode);
            });
        }

        /** @brief Checks scalar defaults without interpreting gameplay values or trigger consumption. */
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

        /** @brief Invocation-local dependency indexes; never retained by the immutable program. */
        struct DefinitionTopology final {
            std::map<GraphNodeId, std::size_t> indexes;
            std::vector<std::vector<std::size_t>> incoming;
            std::vector<std::vector<std::size_t>> outgoing;
            std::vector<std::size_t> indegree;
            std::size_t output{};
        };

        /** @brief Validates node shapes, the unique sink and exact definition-input declarations. */
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

        /** @brief Indexes one edge only after validating both endpoint directions and its exact type. */
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

        /** @brief Builds bounded adjacency tables and rejects every unconnected required input. */
        Result<void> IndexEdges(const GraphDefinition &definition, const AnimationGraphCompileContext &context,
                                DefinitionTopology &topology) {
            topology.incoming.resize(definition.nodes.size());
            topology.outgoing.resize(definition.nodes.size());
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

        /** @brief Checks that every node contributes to the sink, without recursive stack growth. */
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

        /** @brief Materializes exact typed operands after their source instructions have been emitted. */
        GraphInstruction EmitInstruction(const GraphDefinition &definition, const AnimationGraphData &data,
                                         const DefinitionTopology &topology, std::size_t index,
                                         const std::vector<std::uint32_t> &registers) {
            const auto &node = definition.nodes[index];
            GraphInstruction instruction;
            instruction.source = {definition.id, node.id, {}};
            instruction.payload = node.payload;
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
            if (const auto *call = std::get_if<GraphCallNode>(&node.payload))
                instruction.definitionIndex =
                    static_cast<std::uint32_t>(Find(data.definitions, call->definition) - data.definitions.data());
            if (const auto *machine = std::get_if<GraphStateMachineNode>(&node.payload))
                for (const auto &state : machine->states)
                    if (const auto *definitionId = std::get_if<GraphDefinitionId>(&state.source))
                        instruction.stateDefinitions.push_back(
                            static_cast<std::uint32_t>(Find(data.definitions, *definitionId) - data.definitions.data()));
            return instruction;
        }

        /** @brief Builds a stable-id topological schedule and exact operand/source map for one definition. */
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
            std::vector<std::uint32_t> registers(definition.nodes.size());
            while (!ready.empty()) {
                if (auto state = Admission(context); state.HasError())
                    return Result<GraphCompiledDefinition>::Failure(state.ErrorValue());
                const auto index = *ready.begin();
                ready.erase(ready.begin());
                auto instruction = EmitInstruction(definition, data, topology, index, registers);
                registers[index] = static_cast<std::uint32_t>(result.instructions.size());
                result.instructions.push_back(std::move(instruction));
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

        /** @brief Propagates completed callee costs into one definition under exact captured limits. */
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
            for (const auto &instruction : definition.instructions) {
                if (const auto *machine = std::get_if<GraphStateMachineNode>(&instruction.payload)) {
                    ++definition.evaluationInstructions;  // The control occurrence precedes guarded children and pose completion.
                    for (const auto &state : machine->states)
                        if (std::holds_alternative<AnimationClipId>(state.source)) {
                            definition.evaluationInstructions += 2;
                            definition.sourceMapCallEntries += 4;
                            definition.callDepth = std::max(definition.callDepth, 2U);
                        }
                    for (const auto targetIndex : instruction.stateDefinitions) {
                        const auto &target = definitions[targetIndex];
                        definition.callDepth = std::max(definition.callDepth, target.callDepth + 1);
                        definition.evaluationInstructions += 2 * target.evaluationInstructions;
                        definition.sourceMapCallEntries += 2 * (target.sourceMapCallEntries + 2 * target.evaluationInstructions);
                    }
                }
            }
            if (definition.callDepth > context.limits.callDepth ||
                definition.evaluationInstructions > context.limits.evaluationInstructions ||
                definition.sourceMapCallEntries > context.limits.sourceMapCallEntries)
                return Result<void>::Failure(Failure(GraphLimitExceeded, {definition.id, {}, {}}));
            return Result<void>::Success();
        }

        /** @brief Rejects recursive calls and budgets expanded execution without expanding stored programs. */
        Result<void> BoundCalls(std::vector<GraphCompiledDefinition> &definitions, const AnimationGraphCompileContext &context) {
            std::vector<std::set<std::size_t>> dependencies(definitions.size());
            std::vector<std::set<std::size_t>> callers(definitions.size());
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

        /** @brief Infers legacy edge types on a detached definition while preserving cancellation and error order. */
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
        if (!UniqueIds(canonical.definitions) || !UniqueIds(canonical.parameters))
            return Result<AnimationGraphProgram>::Failure(Failure(GraphMalformed));
        const auto *entry = Find(canonical.definitions, canonical.entry);
        if (!entry || !entry->inputs.empty())
            return Result<AnimationGraphProgram>::Failure(Failure(GraphMalformed));
        std::set<std::string, std::less<>> names;
        for (const auto &parameter : canonical.parameters)
            if (!ValidParameter(parameter) || !names.insert(parameter.name).second)
                return Result<AnimationGraphProgram>::Failure(Failure(GraphTypeMismatch));
        AnimationGraphProgram result;
        result.id_ = canonical.id;
        result.skeleton_ = canonical.skeleton;
        result.parameters_ = canonical.parameters;
        result.entry_ = static_cast<std::uint32_t>(entry - canonical.definitions.data());
        std::set<AnimationClipId> clips;
        for (const auto &definition : canonical.definitions) {
            auto compiled = CompileDefinition(definition, canonical, context);
            if (compiled.HasError())
                return Result<AnimationGraphProgram>::Failure(compiled.ErrorValue());
            for (const auto &instruction : compiled.Value().instructions)
                if (const auto *clip = std::get_if<GraphClipNode>(&instruction.payload))
                    clips.insert(clip->clip);
                else if (const auto *machine = std::get_if<GraphStateMachineNode>(&instruction.payload))
                    for (const auto &state : machine->states)
                        if (const auto *stateClip = std::get_if<AnimationClipId>(&state.source))
                            clips.insert(*stateClip);
            result.definitions_.push_back(std::move(compiled).Value());
        }
        if (auto calls = BoundCalls(result.definitions_, context); calls.HasError())
            return Result<AnimationGraphProgram>::Failure(calls.ErrorValue());
        result.clips_.assign(clips.begin(), clips.end());
        if (auto state = Admission(context); state.HasError())
            return Result<AnimationGraphProgram>::Failure(state.ErrorValue());
        return Result<AnimationGraphProgram>::Success(std::move(result));
    }

    /** @copydoc AnimationGraphProgram::Compile */
    Result<AnimationGraphProgram> AnimationGraphProgram::Compile(const AnimationGraphData &data,
                                                                 const AnimationGraphCompileContext &context) {
        auto schema = CompileSchema(data, context);
        if (schema.HasError())
            return schema;
        auto result = std::move(schema).Value();
        auto bound = GraphCompileDetail::Bind(result, context);
        if (bound.HasError())
            return Result<AnimationGraphProgram>::Failure(bound.ErrorValue());
        result.skeletonBinding_ = bound.Value().skeleton;
        result.memory_ = bound.Value().memory;
        auto owned = std::move(bound).Value();
        result.clipBindings_ = std::move(owned.clips);
        result.occurrences_ = std::move(owned.occurrences);
        result.stateMachines_ = std::move(owned.stateMachines);
        if (auto state = Admission(context); state.HasError())
            return Result<AnimationGraphProgram>::Failure(state.ErrorValue());
        return Result<AnimationGraphProgram>::Success(std::move(result));
    }

    /** @copydoc MigrateAnimationGraph */
    Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &data, const AnimationGraphCompileContext &context) {
        const bool legacy = data.schemaVersion == 1 || data.schemaVersion == 2;
        if (data.schemaVersion == 0 || data.schemaVersion > CurrentAnimationGraphSchemaVersion)
            return Result<AnimationGraphData>::Failure(Failure(GraphVersionUnsupported));
        const AnimationGraphContractVersion expected = data.schemaVersion <= 2   ? AnimationGraphContractVersion{1, 0, 0}
                                                       : data.schemaVersion == 3 ? AnimationGraphContractVersion{1, 1, 0}
                                                                                 : CurrentAnimationGraphContractVersion;
        if (data.contractVersion != expected)
            return Result<AnimationGraphData>::Failure(Failure(GraphVersionUnsupported));
        if (data.schemaVersion < 4)
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    if (std::holds_alternative<GraphStateMachineNode>(node.payload))
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
