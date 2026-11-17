#pragma once
/** @file AnimationCookedFields.h
 * @brief Explicit persistent field order shared by the one binary reader/writer; live generations have no representation.
 */
#include "AnimationCookedWire.h"

namespace Horo::Animation::CookedInternal {
    /** @brief Canonical portable fields of Math::Vec3. */
    template <> struct Fields<Math::Vec3> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.x, value.y, value.z);
        }
    };

    /** @brief Canonical portable fields of Math::Quaternion. */
    template <> struct Fields<Math::Quaternion> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.x, value.y, value.z, value.w);
        }
    };

    /** @brief Canonical portable fields of Math::Transform. */
    template <> struct Fields<Math::Transform> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.translation, value.rotation, value.scale);
        }
    };

    /** @brief Canonical portable fields of Math::Mat4. */
    template <> struct Fields<Math::Mat4> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.values);
        }
    };

    /** @brief Canonical portable fields of SkeletonAssetContractVersion. */
    template <> struct Fields<SkeletonAssetContractVersion> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.major, value.minor, value.patch);
        }
    };

    /** @brief Canonical portable fields of SkeletonJoint. */
    template <> struct Fields<SkeletonJoint> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.parent, value.name, value.referenceLocalTransform, value.inverseBindMatrix, value.retargetRole,
                            value.side, value.mirror);
        }
    };

    /** @brief Canonical portable fields of SkeletonSocket. */
    template <> struct Fields<SkeletonSocket> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.joint, value.name, value.localTransform);
        }
    };

    /** @brief Canonical portable fields of SkeletonAssetData. */
    template <> struct Fields<SkeletonAssetData> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.contractVersion, value.skeleton, value.joints, value.sockets);
        }
    };

    /** @brief Canonical portable fields of AnimationClipContractVersion. */
    template <> struct Fields<AnimationClipContractVersion> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.major, value.minor, value.patch);
        }
    };

    /** @brief Canonical portable fields of AnimationTime. */
    template <> struct Fields<AnimationTime> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.ticks);
        }
    };

    /** @brief Canonical portable fields of AnimationDeltaTime. */
    template <> struct Fields<AnimationDeltaTime> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.ticks);
        }
    };

    /** @brief Canonical portable fields of AnimationSampleRate. */
    template <> struct Fields<AnimationSampleRate> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.numerator, value.denominator);
        }
    };

    /** @brief Canonical portable fields of AnimationTransformKey. */
    template <> struct Fields<AnimationTransformKey> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.time, value.transform, value.interpolation);
        }
    };

    /** @brief Canonical portable fields of AnimationJointTrack. */
    template <> struct Fields<AnimationJointTrack> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.joint, value.keys);
        }
    };

    /** @brief Canonical portable fields of AnimationSyncMarker. */
    template <> struct Fields<AnimationSyncMarker> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.time);
        }
    };

    /** @brief Canonical portable fields of AnimationEventField. */
    template <> struct Fields<AnimationEventField> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.value);
        }
    };

    /** @brief Canonical portable fields of AnimationEventPayload. */
    template <> struct Fields<AnimationEventPayload> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.fields, value.count);
        }
    };

    /** @brief Canonical portable fields of AnimationEventFieldSchema. */
    template <> struct Fields<AnimationEventFieldSchema> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.kind, value.required, value.allowedUnsigned);
        }
    };

    /** @brief Canonical portable fields of AnimationEventSchema. */
    template <> struct Fields<AnimationEventSchema> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.type, value.revision, value.fields);
        }
    };

    /** @brief Canonical portable fields of AnimationClipEvent. */
    template <> struct Fields<AnimationClipEvent> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.name, value.type, value.schemaRevision, value.time, value.duration, value.payload,
                            value.fireInReverse, value.fireOnActivation);
        }
    };

    /** @brief Canonical portable fields of AnimationClipCookedData. */
    template <> struct Fields<AnimationClipCookedData> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.contractVersion, value.id, value.skeleton, value.duration, value.sampleRate, value.wrapMode, value.kind,
                            value.compression, value.referencePose, value.hasRootMotion, value.tracks, value.syncMarkers,
                            value.eventSchemas, value.events);
        }
    };

    /** @brief Canonical portable fields of AnimationGraphContractVersion. */
    template <> struct Fields<AnimationGraphContractVersion> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.major, value.minor, value.patch);
        }
    };

    /** @brief Canonical portable fields of GraphParameter. */
    template <> struct Fields<GraphParameter> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.name, value.type, value.defaultValue);
        }
    };

    /** @brief Canonical portable fields of GraphInterfaceInput. */
    template <> struct Fields<GraphInterfaceInput> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.type);
        }
    };

    /** @brief Canonical portable fields of GraphPin. */
    template <> struct Fields<GraphPin> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.role, value.type, value.output, value.interfaceId);
        }
    };

    /** @brief Canonical portable fields of GraphClipNode. */
    template <> struct Fields<GraphClipNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.clip, value.referencePose);
        }
    };

    /** @brief Canonical portable fields of GraphBoneWeight. */
    template <> struct Fields<GraphBoneWeight> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.joint, value.weight);
        }
    };

    /** @brief Canonical portable fields of GraphBoneMask. */
    template <> struct Fields<GraphBoneMask> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.defaultWeight, value.joints);
        }
    };

    /** @brief Canonical portable fields of GraphBlendNode. */
    template <> struct Fields<GraphBlendNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.mode, value.mask, value.missingJoint);
        }
    };

    /** @brief Canonical portable fields of GraphSyncPolicy. */
    template <> struct Fields<GraphSyncPolicy> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.mode, value.events, value.leader);
        }
    };

    /** @brief Canonical portable fields of GraphBlendSample. */
    template <> struct Fields<GraphBlendSample> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.clip, value.x, value.y, value.canLead);
        }
    };

    /** @brief Canonical portable fields of GraphBlendTriangle. */
    template <> struct Fields<GraphBlendTriangle> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.samples);
        }
    };

    /** @brief Canonical portable fields of GraphBlendTreeNode. */
    template <> struct Fields<GraphBlendTreeNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.dimension, value.x, value.y, value.outside, value.samples, value.triangles, value.group, value.sync);
        }
    };

    /** @brief Canonical portable fields of GraphParameterNode. */
    template <> struct Fields<GraphParameterNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.parameter);
        }
    };

    /** @brief Canonical portable fields of GraphOutputNode. */
    template <> struct Fields<GraphOutputNode> final {
        template <typename Value> static auto Get(Value &value) {
            (void)value;
            return std::tie();
        }
    };

    /** @brief Canonical portable fields of GraphInputNode. */
    template <> struct Fields<GraphInputNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.input);
        }
    };

    /** @brief Canonical portable fields of GraphCallNode. */
    template <> struct Fields<GraphCallNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.definition);
        }
    };

    /** @brief Canonical portable fields of GraphStateCondition. */
    template <> struct Fields<GraphStateCondition> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.parameter, value.predicate, value.threshold, value.releaseThreshold);
        }
    };

    /** @brief Canonical portable fields of GraphState. */
    template <> struct Fields<GraphState> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.source, value.loopPeriod);
        }
    };

    /** @brief Canonical portable fields of GraphStateTransition. */
    template <> struct Fields<GraphStateTransition> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.source, value.destination, value.priority, value.duration, value.exitTime, value.interruption,
                            value.curve, value.conditions);
        }
    };

    /** @brief Canonical portable fields of GraphStateMachineNode. */
    template <> struct Fields<GraphStateMachineNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.initial, value.states, value.transitions);
        }
    };

    /** @brief Canonical portable fields of GraphActionSlot. */
    template <> struct Fields<GraphActionSlot> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.name);
        }
    };

    /** @brief Canonical portable fields of GraphActionSection. */
    template <> struct Fields<GraphActionSection> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.name, value.clip, value.begin, value.end, value.next);
        }
    };

    /** @brief Canonical portable fields of GraphActionLoopRegion. */
    template <> struct Fields<GraphActionLoopRegion> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.first, value.last, value.mode, value.repetitions);
        }
    };

    /** @brief Canonical portable fields of GraphActionSequence. */
    template <> struct Fields<GraphActionSequence> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.entry, value.sections, value.loops);
        }
    };

    /** @brief Canonical portable fields of GraphActionDefinition. */
    template <> struct Fields<GraphActionDefinition> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.name, value.slot, value.sequence, value.mask, value.blendIn, value.blendOut, value.curve,
                            value.interruption, value.reverse, value.missingJoint);
        }
    };

    /** @brief Canonical portable fields of GraphActionSlotNode. */
    template <> struct Fields<GraphActionSlotNode> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.slot);
        }
    };

    /** @brief Canonical portable fields of GraphSourceLocation. */
    template <> struct Fields<GraphSourceLocation> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.definition, value.node, value.pin);
        }
    };

    /** @brief Canonical portable fields of GraphCompiledInput. */
    template <> struct Fields<GraphCompiledInput> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.destination, value.instruction, value.source, value.type);
        }
    };

    /** @brief Canonical portable fields of GraphInstruction. */
    template <> struct Fields<GraphInstruction> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.source, value.payload, value.pins, value.inputs, value.parameterIndex, value.actionSlot,
                            value.blendParameters, value.definitionIndex, value.stateDefinitions);
        }
    };

    /** @brief Canonical portable fields of GraphCompiledDefinition. */
    template <> struct Fields<GraphCompiledDefinition> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.inputs, value.instructions, value.outputInstruction, value.callDepth,
                            value.evaluationInstructions, value.sourceMapCallEntries);
        }
    };

    /** @brief Canonical portable fields of AnimationGraphCookedData. */
    template <> struct Fields<AnimationGraphCookedData> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.contractVersion, value.id, value.skeleton, value.entry, value.parameters, value.actionSlots,
                            value.actions, value.definitions);
        }
    };

    /** @brief Canonical complete dependency role identity, independent of runtime catalog state. */
    template <> struct Fields<Assets::AssetDependency> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.id, value.expectedType);
        }
    };

    /** @brief Exact semantic payload commitment, never a runtime generation or host-issued cache identity. */
    template <> struct Fields<AnimationCookedDependency> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.metadata, value.payloadDigest);
        }
    };

    template <> struct Fields<Sha256Digest> final {
        template <typename Value> static auto Get(Value &value) {
            return std::tie(value.bytes);
        }
    };
}  // namespace Horo::Animation::CookedInternal
