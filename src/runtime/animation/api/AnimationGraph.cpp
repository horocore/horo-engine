#include "AnimationGraphCompilerInternal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

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
                                             "/definitions/" + std::to_string(location.definition.Value()) + "/nodes/" +
                                                 std::to_string(location.node.Value()) + "/pins/" + std::to_string(location.pin.Value())});
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

        /** @brief Preflights all storage counts before making a detached candidate copy. */
        Result<void> Preflight(const AnimationGraphData &data, const AnimationGraphCompileContext &context) {
            if (auto state = Admission(context); state.HasError())
                return state;
            if (!data.id.IsValid() || !data.skeleton.IsValid() || !data.entry.IsValid() || data.id.Asset() == data.skeleton.Asset())
                return Result<void>::Failure(Failure(GraphMalformed));
            if (context.replacing && *context.replacing != data.id)
                return Result<void>::Failure(Failure(GraphReloadMismatch));
            const auto &limits = context.limits;
            const AnimationGraphLimits hard{};
            if (limits.definitions == 0 || limits.definitions > hard.definitions || limits.nodes == 0 || limits.nodes > hard.nodes ||
                limits.pins == 0 || limits.pins > hard.pins || limits.connections > hard.connections ||
                limits.parameters > hard.parameters || limits.callDepth == 0 || limits.callDepth > hard.callDepth ||
                limits.evaluationInstructions == 0 || limits.evaluationInstructions > hard.evaluationInstructions ||
                limits.sourceMapCallEntries > hard.sourceMapCallEntries || limits.poseSlots > hard.poseSlots ||
                limits.scalarSlots > hard.scalarSlots || limits.clipPlayers > hard.clipPlayers ||
                limits.poseTransforms > hard.poseTransforms)
                return Result<void>::Failure(Failure(GraphLimitExceeded));
            if (data.definitions.empty() || data.definitions.size() > limits.definitions || data.parameters.size() > limits.parameters)
                return Result<void>::Failure(Failure(GraphLimitExceeded));
            std::uint64_t nodes{}, pins{}, connections{}, interfaces{};
            for (const auto &definition : data.definitions) {
                nodes += definition.nodes.size();
                connections += definition.connections.size();
                interfaces += definition.inputs.size();
                if (nodes > limits.nodes || connections > limits.connections || interfaces > limits.pins)
                    return Result<void>::Failure(Failure(GraphLimitExceeded));
                for (const auto &node : definition.nodes) {
                    if (auto state = Admission(context); state.HasError())
                        return state;
                    pins += node.pins.size();
                    if (pins > limits.pins)
                        return Result<void>::Failure(Failure(GraphLimitExceeded));
                }
            }
            for (const auto &parameter : data.parameters)
                if (parameter.name.empty() || parameter.name.size() > AnimationGraphHardLimits::ParameterNameBytes)
                    return Result<void>::Failure(Failure(GraphMalformed));
            return Result<void>::Success();
        }

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
                for (auto &node : definition.nodes)
                    std::ranges::sort(node.pins, byId);
            }
        }

        /** @brief Rejects zero or duplicated identities in a canonical collection. */
        template <typename Collection> bool UniqueIds(const Collection &values) {
            for (std::size_t index = 0; index < values.size(); ++index)
                if (!values[index].id.IsValid() || (index != 0 && values[index - 1].id == values[index].id))
                    return false;
            return true;
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

        /** @brief Returns the only admitted node shape using typed references and subgraph interfaces. */
        Result<std::vector<GraphPin>> ExpectedPins(const GraphNode &node, const GraphDefinition &definition,
                                                   const AnimationGraphData &data) {
            std::vector<GraphPin> expected;
            const auto add = [&expected](GraphPinRole role, GraphValueType type, bool output, GraphInterfaceId input = {}) {
                expected.push_back({{}, role, type, output, input});
            };
            const GraphSourceLocation location{definition.id, node.id, {}};
            if (const auto *clip = std::get_if<GraphClipNode>(&node.payload)) {
                if (!clip->clip.IsValid() || clip->clip.Asset() == data.id.Asset() || clip->clip.Asset() == data.skeleton.Asset())
                    return Result<std::vector<GraphPin>>::Failure(Failure(GraphMalformed, location));
                add(GraphPinRole::Result, GraphValueType::Pose, true);
            } else if (std::holds_alternative<GraphBlendNode>(node.payload)) {
                add(GraphPinRole::FirstPose, GraphValueType::Pose, false);
                add(GraphPinRole::SecondPose, GraphValueType::Pose, false);
                add(GraphPinRole::Weight, GraphValueType::Float, false);
                add(GraphPinRole::Result, GraphValueType::Pose, true);
            } else if (const auto *parameter = std::get_if<GraphParameterNode>(&node.payload)) {
                const auto *value = Find(data.parameters, parameter->parameter);
                if (!value)
                    return Result<std::vector<GraphPin>>::Failure(Failure(GraphMalformed, location));
                add(GraphPinRole::Result, value->type, true);
            } else if (std::holds_alternative<GraphOutputNode>(node.payload)) {
                add(GraphPinRole::Value, GraphValueType::Pose, false);
            } else if (const auto *input = std::get_if<GraphInputNode>(&node.payload)) {
                const auto *value = Find(definition.inputs, input->input);
                if (!value)
                    return Result<std::vector<GraphPin>>::Failure(Failure(GraphMalformed, location));
                add(GraphPinRole::Result, value->type, true);
            } else if (const auto *call = std::get_if<GraphCallNode>(&node.payload)) {
                const auto *target = Find(data.definitions, call->definition);
                if (!target)
                    return Result<std::vector<GraphPin>>::Failure(Failure(GraphMalformed, location));
                for (const auto &input : target->inputs)
                    add(GraphPinRole::Interface, input.type, false, input.id);
                add(GraphPinRole::Result, GraphValueType::Pose, true);
            } else {
                return Result<std::vector<GraphPin>>::Failure(Failure(GraphMalformed, location));
            }
            return Result<std::vector<GraphPin>>::Success(std::move(expected));
        }

        /** @brief Validates stable pins against exactly one declared semantic role each. */
        Result<void> ValidatePins(const GraphNode &node, const GraphDefinition &definition, const AnimationGraphData &data) {
            auto expected = ExpectedPins(node, definition, data);
            if (expected.HasError())
                return Result<void>::Failure(expected.ErrorValue());
            if (!UniqueIds(node.pins) || node.pins.size() != expected.Value().size())
                return Result<void>::Failure(Failure(GraphMalformed, {definition.id, node.id, {}}));
            std::set<std::pair<GraphPinRole, GraphInterfaceId>> seen;
            for (const auto &pin : node.pins) {
                const auto found = std::ranges::find_if(expected.Value(), [&pin](const auto &value) {
                    return value.role == pin.role && value.interfaceId == pin.interfaceId;
                });
                if (found == expected.Value().end() || !seen.emplace(pin.role, pin.interfaceId).second || found->output != pin.output)
                    return Result<void>::Failure(Failure(GraphMalformed, {definition.id, node.id, pin.id}));
                if (found->type != pin.type)
                    return Result<void>::Failure(Failure(GraphTypeMismatch, {definition.id, node.id, pin.id}));
            }
            return Result<void>::Success();
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
                    if (output)
                        return Result<void>::Failure(Failure(GraphMalformed, location));
                    output = index;
                }
                if (const auto *input = std::get_if<GraphInputNode>(&node.payload))
                    ++inputUses[input->input];
            }
            if (!output || inputUses.size() != definition.inputs.size())
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
            const auto source = topology.indexes.find(edge.source.node), destination = topology.indexes.find(edge.destination.node);
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
            std::vector<std::size_t> pending{topology.output};
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

        /** @brief Rejects recursive calls and budgets expanded execution without expanding stored programs. */
        Result<void> BoundCalls(std::vector<GraphCompiledDefinition> &definitions, const AnimationGraphCompileContext &context) {
            std::vector<std::set<std::size_t>> dependencies(definitions.size()), callers(definitions.size());
            for (std::size_t index = 0; index < definitions.size(); ++index)
                for (const auto &instruction : definitions[index].instructions)
                    if (instruction.definitionIndex) {
                        dependencies[index].insert(*instruction.definitionIndex);
                        callers[*instruction.definitionIndex].insert(index);
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
                auto &definition = definitions[index];
                definition.callDepth = 1;
                definition.evaluationInstructions = definition.instructions.size();
                for (const auto &instruction : definition.instructions)
                    if (instruction.definitionIndex) {
                        const auto &target = definitions[*instruction.definitionIndex];
                        definition.callDepth = std::max(definition.callDepth, target.callDepth + 1);
                        definition.evaluationInstructions += target.evaluationInstructions;
                        definition.sourceMapCallEntries += target.sourceMapCallEntries + target.evaluationInstructions;
                    }
                if (definition.callDepth > context.limits.callDepth ||
                    definition.evaluationInstructions > context.limits.evaluationInstructions ||
                    definition.sourceMapCallEntries > context.limits.sourceMapCallEntries)
                    return Result<void>::Failure(Failure(GraphLimitExceeded, {definition.id, {}, {}}));
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

    }  // namespace

    /** @copydoc AnimationGraphProgram::CompileSchema */
    Result<AnimationGraphProgram> AnimationGraphProgram::CompileSchema(const AnimationGraphData &data,
                                                                       const AnimationGraphCompileContext &context) {
        if (data.schemaVersion != CurrentAnimationGraphSchemaVersion || data.contractVersion != CurrentAnimationGraphContractVersion)
            return Result<AnimationGraphProgram>::Failure(Failure(GraphVersionUnsupported));
        if (auto preflight = Preflight(data, context); preflight.HasError())
            return Result<AnimationGraphProgram>::Failure(preflight.ErrorValue());
        auto canonical = data;
        Canonicalize(canonical);
        if (!UniqueIds(canonical.definitions) || !UniqueIds(canonical.parameters))
            return Result<AnimationGraphProgram>::Failure(Failure(GraphMalformed));
        const auto *entry = Find(canonical.definitions, canonical.entry);
        if (!entry || !entry->inputs.empty())
            return Result<AnimationGraphProgram>::Failure(Failure(GraphMalformed));
        std::set<std::string> names;
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
        if (auto state = Admission(context); state.HasError())
            return Result<AnimationGraphProgram>::Failure(state.ErrorValue());
        return Result<AnimationGraphProgram>::Success(std::move(result));
    }

    /** @copydoc MigrateAnimationGraph */
    Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &data, const AnimationGraphCompileContext &context) {
        if (data.schemaVersion != 1 && data.schemaVersion != CurrentAnimationGraphSchemaVersion)
            return Result<AnimationGraphData>::Failure(Failure(GraphVersionUnsupported));
        if (auto preflight = Preflight(data, context); preflight.HasError())
            return Result<AnimationGraphData>::Failure(preflight.ErrorValue());
        auto migrated = data;
        Canonicalize(migrated);
        if (migrated.schemaVersion == 1) {
            for (auto &definition : migrated.definitions)
                for (auto &edge : definition.connections) {
                    if (auto state = Admission(context); state.HasError())
                        return Result<AnimationGraphData>::Failure(state.ErrorValue());
                    const auto *node = Find(definition.nodes, edge.source.node);
                    const auto *pin = node ? Find(node->pins, edge.source.pin) : nullptr;
                    if (!pin || edge.type != GraphValueType::Unspecified)
                        return Result<AnimationGraphData>::Failure(
                            Failure(GraphMalformed, {definition.id, edge.source.node, edge.source.pin}));
                    edge.type = pin->type;
                }
            migrated.schemaVersion = CurrentAnimationGraphSchemaVersion;
        }
        auto checked = AnimationGraphProgram::CompileSchema(migrated, context);
        if (checked.HasError())
            return Result<AnimationGraphData>::Failure(checked.ErrorValue());
        return Result<AnimationGraphData>::Success(std::move(migrated));
    }
}  // namespace Horo::Animation
