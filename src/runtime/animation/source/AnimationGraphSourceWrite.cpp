#include "AnimationGraphSourceWriter.h"

#include <type_traits>

namespace Horo::Animation {
    namespace {
        using namespace GraphSourceDetail;

        /** @brief Writes a known type in the canonical source vocabulary. */
        std::string_view Type(const GraphValueType value) {
            return Types.at(static_cast<std::size_t>(value));
        }

        /** @brief Writes stable pin identities and exact semantic direction/interface
         * information. */
        void Pin(SourceWriter &out, const GraphPin &pin) {
            out.Object([&out, &pin] {
                out.Field("id", pin.id.Value());
                out.Field("role", Roles.at(static_cast<std::size_t>(pin.role)));
                out.Field("type", Type(pin.type));
                out.Field("output", pin.output);
                out.Field("interface", [&out, &pin] {
                    pin.interfaceId.IsValid() ? out.Value(pin.interfaceId.Value()) : out.Value(nullptr);
                });
            });
        }

        /** @brief Writes one transition's ordered typed conditions and exact
         * time/policy operands. */
        void StateTransition(SourceWriter &out, const GraphStateTransition &transition) {
            out.Object([&out, &transition] {
                out.Field("id", transition.id.Value());
                out.Field("source", transition.source.Value());
                out.Field("destination", transition.destination.Value());
                out.Field("priority", transition.priority);
                out.Field("duration", transition.duration.ticks);
                out.Field("exitTime", [&out, &transition] {
                    transition.exitTime ? out.Value(transition.exitTime->ticks) : out.Value(nullptr);
                });
                out.Field("interruption", Interruptions.at(static_cast<std::size_t>(transition.interruption)));
                out.Field("curve", BlendCurves.at(static_cast<std::size_t>(transition.curve)));
                out.Field("conditions", [&out, &transition] {
                    out.Array(transition.conditions, [&out](const GraphStateCondition &condition) {
                        out.Object([&out, &condition] {
                            out.Field("parameter", condition.parameter.Value());
                            out.Field("predicate", Predicates.at(static_cast<std::size_t>(condition.predicate)));
                            out.Field("threshold", [&out, &condition] {
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

        /** @brief Writes authored state identities and policies without runtime clocks
         * or generations. */
        void StateMachineReference(SourceWriter &out, const GraphStateMachineNode &machine) {
            out.Object([&out, &machine] {
                out.Field("initial", machine.initial.Value());
                out.Field("states", [&out, &machine] {
                    out.Array(machine.states, [&out](const GraphState &state) {
                        const auto *clip = std::get_if<AnimationClipId>(&state.source);
                        out.Object([&out, &state, &clip] {
                            out.Field("id", state.id.Value());
                            out.Field("sourceKind", clip ? "clip" : "definition");
                            out.Field("reference", [&out, &state, &clip] {
                                if (clip)
                                    out.Value(clip->Asset().ToString());
                                else
                                    out.Value(std::get<GraphDefinitionId>(state.source).Value());
                            });
                            out.Field("loopPeriod", [&out, &state] {
                                state.loopPeriod ? out.Value(state.loopPeriod->ticks) : out.Value(nullptr);
                            });
                        });
                    });
                });
                out.Field("transitions", [&out, &machine] {
                    out.Array(machine.transitions, [&out](const auto &transition) {
                        StateTransition(out, transition);
                    });
                });
            });
        }

        /** @brief Writes immutable typed tree geometry and instance-local
         * synchronization names. */
        void BlendTreeReference(SourceWriter &out, const GraphBlendTreeNode &tree) {
            out.Object([&out, &tree] {
                out.Field("dimension", Dimensions.at(static_cast<std::size_t>(tree.dimension)));
                out.Field("x", tree.x.Value());
                out.Field("y", [&out, &tree] {
                    tree.y.IsValid() ? out.Value(tree.y.Value()) : out.Value(nullptr);
                });
                out.Field("outside", Outside.at(static_cast<std::size_t>(tree.outside)));
                out.Field("group", [&out, &tree] {
                    tree.group.IsValid() ? out.Value(tree.group.Value()) : out.Value(nullptr);
                });
                out.Field("sync", SyncModes.at(static_cast<std::size_t>(tree.sync.mode)));
                out.Field("events", SyncEvents.at(static_cast<std::size_t>(tree.sync.events)));
                out.Field("leader", SyncLeaders.at(static_cast<std::size_t>(tree.sync.leader)));
                out.Field("samples", [&out, &tree] {
                    out.Array(tree.samples, [&out](const GraphBlendSample &sample) {
                        out.Object([&out, &sample] {
                            out.Field("id", sample.id.Value());
                            out.Field("clip", sample.clip.Asset().ToString());
                            out.Field("x", sample.x);
                            out.Field("y", sample.y);
                            out.Field("canLead", sample.canLead);
                        });
                    });
                });
                out.Field("triangles", [&out, &tree] {
                    out.Array(tree.triangles, [&out](const GraphBlendTriangle &triangle) {
                        out.Array(triangle.samples, [&out](const auto id) {
                            out.Value(id.Value());
                        });
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
                else if constexpr (std::is_same_v<T, GraphActionSlotNode>)
                    out.Value(value.slot.Value());
                else if constexpr (std::is_same_v<T, GraphBlendTreeNode>)
                    BlendTreeReference(out, value);
                else
                    out.Value(nullptr);
            }, payload);
        }

        /** @brief Shared sparse-mask encoding for pose layers and action definitions.
         */
        void BoneMask(SourceWriter &out, const GraphBoneMask &mask) {
            out.Object([&out, &mask] {
                out.Field("defaultWeight", mask.defaultWeight);
                out.Field("joints", [&out, &mask] {
                    out.Array(mask.joints, [&out](const GraphBoneWeight &entry) {
                        out.Object([&out, &entry] {
                            out.Field("joint", entry.joint.Value());
                            out.Field("weight", entry.weight);
                        });
                    });
                });
            });
        }

        /** @brief Writes one exact named section and its sole ordinary successor
         * authority. */
        void ActionSection(SourceWriter &out, const GraphActionSection &section) {
            out.Object([&out, &section] {
                out.Field("id", section.id.Value());
                out.Field("name", section.name);
                out.Field("clip", section.clip.Asset().ToString());
                out.Field("begin", section.begin.ticks);
                out.Field("end", section.end.ticks);
                out.Field("next", [&out, &section] {
                    section.next.IsValid() ? out.Value(section.next.Value()) : out.Value(nullptr);
                });
            });
        }

        /** @brief Writes the explicit finite/until-release loop path without a second
         * exit field. */
        void ActionLoop(SourceWriter &out, const GraphActionLoopRegion &loop) {
            out.Object([&out, &loop] {
                out.Field("id", loop.id.Value());
                out.Field("first", loop.first.Value());
                out.Field("last", loop.last.Value());
                out.Field("mode", ActionLoops.at(static_cast<std::size_t>(loop.mode)));
                out.Field("repetitions", loop.repetitions);
            });
        }

        /** @brief Writes an immutable action sequence catalog value, never a live
         * request/cursor. */
        void Action(SourceWriter &out, const GraphActionDefinition &action) {
            out.Object([&out, &action] {
                out.Field("id", action.id.Value());
                out.Field("name", action.name);
                out.Field("slot", action.slot.Value());
                out.Field("blendIn", action.blendIn.ticks);
                out.Field("blendOut", action.blendOut.ticks);
                out.Field("curve", BlendCurves.at(static_cast<std::size_t>(action.curve)));
                out.Field("interruption", ActionInterruptions.at(static_cast<std::size_t>(action.interruption)));
                out.Field("reverse", ActionReverse.at(static_cast<std::size_t>(action.reverse)));
                out.Field("missingJoint", MissingJoints.at(static_cast<std::size_t>(action.missingJoint)));
                out.Field("mask", [&out, &action] {
                    BoneMask(out, action.mask);
                });
                out.Field("sequence", [&out, &action] {
                    out.Object([&out, &action] {
                        out.Field("entry", action.sequence.entry.Value());
                        out.Field("sections", [&out, &action] {
                            out.Array(action.sequence.sections, [&out](const auto &section) {
                                ActionSection(out, section);
                            });
                        });
                        out.Field("loops", [&out, &action] {
                            out.Array(action.sequence.loops, [&out](const auto &loop) {
                                ActionLoop(out, loop);
                            });
                        });
                    });
                });
            });
        }

        /** @brief Writes graph-wide named slots and their immutable action definitions.
         */
        void ActionCatalog(SourceWriter &out, const AnimationGraphData &data) {
            out.Field("actionSlots", [&out, &data] {
                out.Array(data.actionSlots, [&out](const GraphActionSlot &slot) {
                    out.Object([&out, &slot] {
                        out.Field("id", slot.id.Value());
                        out.Field("name", slot.name);
                    });
                });
            });
            out.Field("actions", [&out, &data] {
                out.Array(data.actions, [&out](const auto &action) {
                    Action(out, action);
                });
            });
        }

        /** @brief Writes typed settings independently of opcode identity and runtime
         * generations. */
        void Settings(SourceWriter &out, const GraphNodePayload &payload) {
            out.Object([&out, &payload] {
                if (const auto *clip = std::get_if<GraphClipNode>(&payload))
                    out.Field("referencePose", [&out, &clip] {
                        clip->referencePose ? out.Value(clip->referencePose->Value()) : out.Value(nullptr);
                    });
                if (const auto *blend = std::get_if<GraphBlendNode>(&payload)) {
                    out.Field("mode", BlendModes.at(static_cast<std::size_t>(blend->mode)));
                    out.Field("missingJoint", MissingJoints.at(static_cast<std::size_t>(blend->missingJoint)));
                    out.Field("mask", [&out, &blend] {
                        BoneMask(out, blend->mask);
                    });
                }
            });
        }

        /** @brief Writes canonical nodes without persisting variant indexes or ABI
         * layout. */
        void Node(SourceWriter &out, const GraphNode &node) {
            out.Object([&out, &node] {
                out.Field("id", node.id.Value());
                out.Field("kind", Kinds.at(node.payload.index()));
                out.Field("reference", [&out, &node] {
                    Reference(out, node.payload);
                });
                out.Field("pins", [&out, &node] {
                    out.Array(node.pins, [&out](const auto &pin) {
                        Pin(out, pin);
                    });
                });
                out.Field("settings", [&out, &node] {
                    Settings(out, node.payload);
                });
            });
        }

        /** @brief Writes exact stable edge endpoints. */
        void Endpoint(SourceWriter &out, const GraphEndpoint &endpoint) {
            out.Object([&out, &endpoint] {
                out.Field("node", endpoint.node.Value());
                out.Field("pin", endpoint.pin.Value());
            });
        }

        /** @brief Writes a fully validated canonical definition. */
        void Definition(SourceWriter &out, const GraphDefinition &definition) {
            out.Object([&out, &definition] {
                out.Field("id", definition.id.Value());
                out.Field("inputs", [&out, &definition] {
                    out.Array(definition.inputs, [&out](const auto &input) {
                        out.Object([&out, &input] {
                            out.Field("id", input.id.Value());
                            out.Field("type", Type(input.type));
                        });
                    });
                });
                out.Field("nodes", [&out, &definition] {
                    out.Array(definition.nodes, [&out](const auto &node) {
                        Node(out, node);
                    });
                });
                out.Field("connections", [&out, &definition] {
                    out.Array(definition.connections, [&out](const GraphConnection &edge) {
                        out.Object([&out, &edge] {
                            out.Field("source", [&out, &edge] {
                                Endpoint(out, edge.source);
                            });
                            out.Field("destination", [&out, &edge] {
                                Endpoint(out, edge.destination);
                            });
                            out.Field("type", Type(edge.type));
                        });
                    });
                });
            });
        }

        /** @brief Writes finite scalar defaults with the existing canonical primitive
         * spelling. */
        void Parameter(SourceWriter &out, const GraphParameter &parameter) {
            out.Object([&out, &parameter] {
                out.Field("id", parameter.id.Value());
                out.Field("name", parameter.name);
                out.Field("type", Type(parameter.type));
                out.Field("default", [&out, &parameter] {
                    std::visit([&](const auto scalar) {
                        out.Value(scalar);
                    }, parameter.defaultValue);
                });
            });
        }

        /** @brief Preserves the codec's sorted clip dependencies followed by the exact
         * skeleton. */
        void Dependencies(SourceWriter &out, const AnimationGraphData &data) {
            const auto projection = ProjectDependencies(data);
            out.Array([&out, &projection, &data] {
                for (const auto clip : projection.clips)
                    out.Item([&out, &clip] {
                        out.Object([&out, &clip] {
                            out.Field("assetType", "core.animation.clip");
                            out.Field("assetId", clip.Asset().ToString());
                        });
                    });
                out.Item([&out, &data] {
                    out.Object([&out, &data] {
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
            out.Object([&out, &canonical] {
                out.Field("schemaVersion", CurrentAnimationGraphSchemaVersion);
                out.Field("assetType", "core.animation.graph");
                out.Field("contractVersion", [&out] {
                    out.Object([&out] {
                        out.Field("major", CurrentAnimationGraphContractVersion.major);
                        out.Field("minor", CurrentAnimationGraphContractVersion.minor);
                        out.Field("patch", CurrentAnimationGraphContractVersion.patch);
                    });
                });
                out.Field("dependencies", [&out, &canonical] {
                    Dependencies(out, canonical);
                });
                out.Field("payload", [&out, &canonical] {
                    out.Object([&out, &canonical] {
                        out.Field("skeleton", canonical.skeleton.Asset().ToString());
                        out.Field("entry", canonical.entry.Value());
                        ActionCatalog(out, canonical);
                        out.Field("parameters", [&out, &canonical] {
                            out.Array(canonical.parameters, [&out](const auto &parameter) {
                                Parameter(out, parameter);
                            });
                        });
                        out.Field("definitions", [&out, &canonical] {
                            out.Array(canonical.definitions, [&out](const auto &definition) {
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
