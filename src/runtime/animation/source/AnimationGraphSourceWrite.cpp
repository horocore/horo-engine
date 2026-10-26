#include "AnimationGraphSourceWriter.h"

#include <type_traits>

namespace Horo::Animation {
    namespace {
        using namespace GraphSourceDetail;

        /** @brief Writes a known type in the canonical source vocabulary. */
        std::string_view Type(const GraphValueType value) {
            return Types.at(static_cast<std::size_t>(value));
        }

        /** @brief Writes stable pin identities and exact semantic direction/interface information. */
        void Pin(SourceWriter &out, const GraphPin &pin) {
            out.Object([&] {
                out.Field("id", pin.id.Value());
                out.Field("role", Roles.at(static_cast<std::size_t>(pin.role)));
                out.Field("type", Type(pin.type));
                out.Field("output", pin.output);
                out.Field("interface", [&] {
                    pin.interfaceId.IsValid() ? out.Value(pin.interfaceId.Value()) : out.Value(nullptr);
                });
            });
        }

        /** @brief Writes one transition's ordered typed conditions and exact time/policy operands. */
        void StateTransition(SourceWriter &out, const GraphStateTransition &transition) {
            out.Object([&] {
                out.Field("id", transition.id.Value());
                out.Field("source", transition.source.Value());
                out.Field("destination", transition.destination.Value());
                out.Field("priority", transition.priority);
                out.Field("duration", transition.duration.ticks);
                out.Field("exitTime", [&] {
                    transition.exitTime ? out.Value(transition.exitTime->ticks) : out.Value(nullptr);
                });
                out.Field("interruption", Interruptions.at(static_cast<std::size_t>(transition.interruption)));
                out.Field("curve", BlendCurves.at(static_cast<std::size_t>(transition.curve)));
                out.Field("conditions", [&] {
                    out.Array(transition.conditions, [&](const GraphStateCondition &condition) {
                        out.Object([&] {
                            out.Field("parameter", condition.parameter.Value());
                            out.Field("predicate", Predicates.at(static_cast<std::size_t>(condition.predicate)));
                            out.Field("threshold", [&] {
                                std::visit([&](const auto scalar) {
                                    out.Value(scalar);
                                }, condition.threshold);
                            });
                            out.Field("release", condition.releaseThreshold);
                        });
                    });
                });
            });
        }

        /** @brief Writes authored state identities and policies without runtime clocks or generations. */
        void StateMachineReference(SourceWriter &out, const GraphStateMachineNode &machine) {
            out.Object([&] {
                out.Field("initial", machine.initial.Value());
                out.Field("states", [&] {
                    out.Array(machine.states, [&](const GraphState &state) {
                        const auto *clip = std::get_if<AnimationClipId>(&state.source);
                        out.Object([&] {
                            out.Field("id", state.id.Value());
                            out.Field("sourceKind", clip ? "clip" : "definition");
                            out.Field("reference", [&] {
                                if (clip)
                                    out.Value(clip->Asset().ToString());
                                else
                                    out.Value(std::get<GraphDefinitionId>(state.source).Value());
                            });
                            out.Field("loopPeriod", [&] {
                                state.loopPeriod ? out.Value(state.loopPeriod->ticks) : out.Value(nullptr);
                            });
                        });
                    });
                });
                out.Field("transitions", [&] {
                    out.Array(machine.transitions, [&](const auto &transition) {
                        StateTransition(out, transition);
                    });
                });
            });
        }

        /** @brief Writes the one reference allowed by each typed opcode. */
        void Reference(SourceWriter &out, const GraphNodePayload &payload) {
            std::visit([&]<typename T>(const T &value) {
                if constexpr (std::is_same_v<T, GraphClipNode>)
                    out.Value(value.clip.Asset().ToString());
                else if constexpr (std::is_same_v<T, GraphParameterNode>)
                    out.Value(value.parameter.Value());
                else if constexpr (std::is_same_v<T, GraphInputNode>)
                    out.Value(value.input.Value());
                else if constexpr (std::is_same_v<T, GraphCallNode>)
                    out.Value(value.definition.Value());
                else if constexpr (std::is_same_v<T, GraphStateMachineNode>)
                    StateMachineReference(out, value);
                else
                    out.Value(nullptr);
            }, payload);
        }

        /** @brief Writes typed settings independently of opcode identity and runtime generations. */
        void Settings(SourceWriter &out, const GraphNodePayload &payload) {
            out.Object([&] {
                if (const auto *clip = std::get_if<GraphClipNode>(&payload))
                    out.Field("referencePose", [&] {
                        clip->referencePose ? out.Value(clip->referencePose->Value()) : out.Value(nullptr);
                    });
                if (const auto *blend = std::get_if<GraphBlendNode>(&payload)) {
                    out.Field("mode", BlendModes.at(static_cast<std::size_t>(blend->mode)));
                    out.Field("missingJoint", MissingJoints.at(static_cast<std::size_t>(blend->missingJoint)));
                    out.Field("mask", [&] {
                        out.Object([&] {
                            out.Field("defaultWeight", blend->mask.defaultWeight);
                            out.Field("joints", [&] {
                                out.Array(blend->mask.joints, [&](const GraphBoneWeight &entry) {
                                    out.Object([&] {
                                        out.Field("joint", entry.joint.Value());
                                        out.Field("weight", entry.weight);
                                    });
                                });
                            });
                        });
                    });
                }
            });
        }

        /** @brief Writes canonical nodes without persisting variant indexes or ABI layout. */
        void Node(SourceWriter &out, const GraphNode &node) {
            out.Object([&] {
                out.Field("id", node.id.Value());
                out.Field("kind", Kinds.at(node.payload.index()));
                out.Field("reference", [&] {
                    Reference(out, node.payload);
                });
                out.Field("pins", [&] {
                    out.Array(node.pins, [&](const auto &pin) {
                        Pin(out, pin);
                    });
                });
                out.Field("settings", [&] {
                    Settings(out, node.payload);
                });
            });
        }

        /** @brief Writes exact stable edge endpoints. */
        void Endpoint(SourceWriter &out, const GraphEndpoint &endpoint) {
            out.Object([&] {
                out.Field("node", endpoint.node.Value());
                out.Field("pin", endpoint.pin.Value());
            });
        }

        /** @brief Writes a fully validated canonical definition. */
        void Definition(SourceWriter &out, const GraphDefinition &definition) {
            out.Object([&] {
                out.Field("id", definition.id.Value());
                out.Field("inputs", [&] {
                    out.Array(definition.inputs, [&](const auto &input) {
                        out.Object([&] {
                            out.Field("id", input.id.Value());
                            out.Field("type", Type(input.type));
                        });
                    });
                });
                out.Field("nodes", [&] {
                    out.Array(definition.nodes, [&](const auto &node) {
                        Node(out, node);
                    });
                });
                out.Field("connections", [&] {
                    out.Array(definition.connections, [&](const GraphConnection &edge) {
                        out.Object([&] {
                            out.Field("source", [&] {
                                Endpoint(out, edge.source);
                            });
                            out.Field("destination", [&] {
                                Endpoint(out, edge.destination);
                            });
                            out.Field("type", Type(edge.type));
                        });
                    });
                });
            });
        }

        /** @brief Writes finite scalar defaults with the existing canonical primitive spelling. */
        void Parameter(SourceWriter &out, const GraphParameter &parameter) {
            out.Object([&] {
                out.Field("id", parameter.id.Value());
                out.Field("name", parameter.name);
                out.Field("type", Type(parameter.type));
                out.Field("default", [&] {
                    std::visit([&](const auto scalar) {
                        out.Value(scalar);
                    }, parameter.defaultValue);
                });
            });
        }

        /** @brief Preserves the codec's sorted clip dependencies followed by the exact skeleton. */
        void Dependencies(SourceWriter &out, const AnimationGraphData &data) {
            DependencyProjection projection;
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    std::visit(projection, node.payload);
            out.Array([&] {
                for (const auto clip : projection.clips)
                    out.Item([&] {
                        out.Object([&] {
                            out.Field("assetType", "core.animation.clip");
                            out.Field("assetId", clip.Asset().ToString());
                        });
                    });
                out.Item([&] {
                    out.Object([&] {
                        out.Field("assetType", "core.animation.skeleton");
                        out.Field("assetId", data.skeleton.Asset().ToString());
                    });
                });
            });
        }
    }  // namespace

    /** @copydoc SerializeAnimationGraphSource */
    Result<std::string> SerializeAnimationGraphSource(const AnimationGraphData &data, const AnimationGraphCompileContext &context,
                                                      const AnimationGraphSourceLimits &limits) {
        try {
            CheckLimits(limits);
            CheckAdmission(context);
            if (data.schemaVersion != CurrentAnimationGraphSchemaVersion)
                Reject(AnimationErrors::GraphVersionUnsupported);
            auto checked = MigrateAnimationGraph(data, context);
            if (checked.HasError())
                return Result<std::string>::Failure(checked.ErrorValue());
            const auto &canonical = checked.Value();
            SourceWriter out(context, limits);
            out.Object([&] {
                out.Field("schemaVersion", CurrentAnimationGraphSchemaVersion);
                out.Field("assetType", "core.animation.graph");
                out.Field("contractVersion", [&] {
                    out.Object([&] {
                        out.Field("major", CurrentAnimationGraphContractVersion.major);
                        out.Field("minor", CurrentAnimationGraphContractVersion.minor);
                        out.Field("patch", CurrentAnimationGraphContractVersion.patch);
                    });
                });
                out.Field("dependencies", [&] {
                    Dependencies(out, canonical);
                });
                out.Field("payload", [&] {
                    out.Object([&] {
                        out.Field("skeleton", canonical.skeleton.Asset().ToString());
                        out.Field("entry", canonical.entry.Value());
                        out.Field("parameters", [&] {
                            out.Array(canonical.parameters, [&](const auto &parameter) {
                                Parameter(out, parameter);
                            });
                        });
                        out.Field("definitions", [&] {
                            out.Array(canonical.definitions, [&](const auto &definition) {
                                Definition(out, definition);
                            });
                        });
                    });
                });
            });
            return Result<std::string>::Success(out.Finish());
        } catch (const Rejection &failure) {
            return Result<std::string>::Failure(MakeError(*failure.code));
        }
    }
}  // namespace Horo::Animation
