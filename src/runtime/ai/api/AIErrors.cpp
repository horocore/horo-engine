#include "Horo/AI/AIErrors.h"

namespace Horo::AI::AIErrors {
    namespace {
        const ErrorDomainId AiDomain{"horo.ai"};
    }

    const ErrorCodeDescriptor IdentityInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.identity.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI identity uses its reserved invalid representation.",
        .remediationHint = "Use a non-zero identity issued by the owning authoring boundary.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DescriptorConflict{
        .domain = AiDomain,
        .code = ErrorCode{"ai.descriptor.conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Gameplay-AI descriptors collide in one persistent identity domain.",
        .remediationHint = "Issue a distinct stable identity for each descriptor and duplicated authored object.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DescriptorLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.descriptor.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI descriptor identity set exceeds its validation bound.",
        .remediationHint = "Partition the authored contribution or reduce its descriptor count before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HandleInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.handle.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI runtime handle is malformed, foreign, or stale.",
        .remediationHint = "Resolve the persistent binding again against the active SceneRuntime incarnation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GenerationExhausted{
        .domain = AiDomain,
        .code = ErrorCode{"ai.generation.exhausted"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "The gameplay-AI runtime slot generation range is exhausted.",
        .remediationHint = "Retire the exhausted slot; never wrap or reuse an issued runtime generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TaskContextInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.task.context_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The AI task context is malformed or crosses runtime generations.",
        .remediationHint = "Capture valid task and agent handles from the same active SceneRuntime incarnation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TaskTransitionInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.task.transition_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The AI task lifecycle operation is invalid for its current state.",
        .remediationHint = "Start once, publish one terminal result, then claim and complete cleanup in order.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TaskFailureInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.task.failure_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The AI task failure detail is missing a typed family or cause identity.",
        .remediationHint = "Provide a known failure family and an owned non-empty typed Error cause.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor RuntimeUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.runtime.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected composition provides no gameplay-AI runtime.",
        .remediationHint = "Compose an explicit gameplay-AI runtime or handle capability absence without fabricating a decision.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor TaskCapacityExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.task.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI task execution capacity is exhausted.",
        .remediationHint = "Reduce admitted task work or select a larger bounded AI task capacity before activation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BlackboardSchemaInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.schema_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI blackboard schema has an invalid identity, version, policy, or key descriptor.",
        .remediationHint = "Correct the schema metadata and submit a bounded typed key set.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BlackboardLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI blackboard schema or value exceeds a fixed contract capacity.",
        .remediationHint = "Reduce the key, collection, or opaque payload count before admission.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BlackboardValueTypeMismatch{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.value_type_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI blackboard value does not match its schema key type.",
        .remediationHint = "Encode the value using the key's exact scalar kind and cardinality.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BlackboardValueInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.value_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI blackboard value is non-finite, malformed, or contains an invalid stored reference.",
        .remediationHint = "Provide finite scalar data and well-formed canonical reference bytes.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BlackboardUnknownValueRejected{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.unknown_value_rejected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The gameplay-AI blackboard schema rejects an unavailable key or value type.",
        .remediationHint = "Load the owning type adapter or use an explicit preserve-opaque schema version policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BlackboardStorageUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.storage_unavailable"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "Immutable gameplay-AI blackboard schema or instance storage could not be allocated.",
        .remediationHint = "Release memory pressure and retry blackboard admission before scene activation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BlackboardInstanceStale{.domain = AiDomain,
                                                      .code = ErrorCode{"ai.blackboard.instance_stale"},
                                                      .defaultSeverity = ErrorSeverity::Warning,
                                                      .summary = "The blackboard snapshot, batch, or instance generation is stale.",
                                                      .remediationHint = "Capture the active instance generation and revision again.",
                                                      .retryable = true,
                                                      .userActionable = false};
    const ErrorCodeDescriptor
        BlackboardInstanceInvalid{.domain = AiDomain,
                                  .code = ErrorCode{"ai.blackboard.instance_invalid"},
                                  .defaultSeverity = ErrorSeverity::Error,
                                  .summary = "The blackboard binding or required default layout is invalid.",
                                  .remediationHint = "Use one exact runtime, agent, schema publication, and non-zero instance generation.",
                                  .retryable = false,
                                  .userActionable = true};
    const ErrorCodeDescriptor BlackboardBatchInvalid{.domain = AiDomain,
                                                     .code = ErrorCode{"ai.blackboard.batch_invalid"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "The blackboard write batch is invalid.",
                                                     .remediationHint =
                                                         "Use unique writable schema keys and commit the detached batch at BlackboardSync.",
                                                     .retryable = false,
                                                     .userActionable = true};
    const ErrorCodeDescriptor BlackboardRevisionExhausted{.domain = AiDomain,
                                                          .code = ErrorCode{"ai.blackboard.revision_exhausted"},
                                                          .defaultSeverity = ErrorSeverity::Critical,
                                                          .summary = "The blackboard revision or instance generation is exhausted.",
                                                          .remediationHint =
                                                              "Retire the instance; never wrap or reuse an issued generation.",
                                                          .retryable = false,
                                                          .userActionable = false};
    const ErrorCodeDescriptor BlackboardObserverInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.observer_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The blackboard observer registration or token is invalid, foreign, or stale.",
        .remediationHint = "Register a valid key and task against the active agent-scoped blackboard generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BlackboardObserverLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.observer_limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The agent-scoped blackboard observer registry has no reusable slot.",
        .remediationHint = "Remove completed task observers before registering additional key observers.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BlackboardReentrantMutation{
        .domain = AiDomain,
        .code = ErrorCode{"ai.blackboard.reentrant_mutation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A blackboard publication callback attempted a reentrant mutation.",
        .remediationHint = "Stage the mutation for the next BlackboardSync safe point after publication completes.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionDescriptorInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception descriptor has invalid identity, source, version, capability, or dependency metadata.",
        .remediationHint = "Correct the typed descriptor metadata before composing the SceneRuntime perception registry.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDescriptorLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The perception descriptor registry exceeds a configured bounded capacity.",
        .remediationHint = "Reduce or partition perception contributions before SceneRuntime activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDescriptorConflict{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Multiple perception descriptors claim the same stable type identity.",
        .remediationHint = "Assign one globally unique stable identity to each sense, stimulus, and listener type.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDependencyMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.dependency_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required perception listener dependency is not registered.",
        .remediationHint = "Register the exact referenced sense and stimulus descriptors or make the listener optional.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDescriptorIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception sense or stimulus descriptor version is incompatible with its listener.",
        .remediationHint = "Install a descriptor version inside the listener's declared inclusive compatibility interval.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionCapabilityUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The composition root cannot satisfy a required perception listener capability.",
        .remediationHint = "Provide the declared gameplay query service or disable the dependent listener explicitly.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionRegistryStorageUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.registry_storage_unavailable"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "Storage for an immutable perception descriptor registry is unavailable.",
        .remediationHint = "Release memory pressure and retry registry composition before SceneRuntime activation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BehaviorTreeSchemaInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.behavior_tree.schema_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior-tree schema or typed node/property contract is malformed.",
        .remediationHint = "Use the current schema version and valid stable identities, kinds, versions, and bounded values.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BehaviorTreeLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.behavior_tree.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior-tree source exceeds a finite asset bound.",
        .remediationHint = "Reduce nodes, edges, pins, properties, children, or opaque payload bytes before capture.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BehaviorTreeIdentityConflict{
        .domain = AiDomain,
        .code = ErrorCode{"ai.behavior_tree.identity_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A behavior-tree stable identity is duplicated within its graph domain.",
        .remediationHint = "Issue a distinct stable identity; display names and layout positions are not identity.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BehaviorTreeTopologyInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.behavior_tree.topology_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A behavior-tree root, edge, pin, child, or service relationship is invalid.",
        .remediationHint = "Connect valid pins, give each non-root node one structural parent, and honor node child bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BehaviorTreeCycle{
        .domain = AiDomain,
        .code = ErrorCode{"ai.behavior_tree.cycle"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The behavior-tree structural graph contains a directed cycle.",
        .remediationHint = "Remove the cyclic child or service relationship before publishing the asset.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor BehaviorTreeStorageUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.behavior_tree.storage_unavailable"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "Storage for an immutable behavior-tree asset is unavailable.",
        .remediationHint = "Release memory pressure and retry behavior-tree capture before activation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor DecisionAssetSchemaInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.schema_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision asset identity, kind, version, source, or typed validation contract is malformed.",
        .remediationHint = "Correct the stable semantic asset data before attempting compilation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision asset validation input or output exceeds a fixed bounded capacity.",
        .remediationHint = "Reduce or partition assets, dependencies, requirements, or diagnostic output before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetDescriptorMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.descriptor_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision node references a descriptor that is not registered.",
        .remediationHint = "Load the native, package, or script descriptor identified by the stable node type ID.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetDescriptorAmbiguous{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.descriptor_ambiguous"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Multiple compatible decision node descriptors claim one stable node type identity.",
        .remediationHint = "Retain exactly one compatible descriptor for the node type and provider generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetDescriptorIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A registered decision node descriptor is outside the authored version interval.",
        .remediationHint = "Install a descriptor version inside the asset's explicit compatibility range or recook the asset.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetSchemaMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.schema_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision asset references a blackboard schema that is not registered.",
        .remediationHint = "Load the exact stable blackboard schema identity before compiling the decision asset.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetSchemaAmbiguous{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.schema_ambiguous"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Multiple blackboard schema candidates satisfy one decision asset binding.",
        .remediationHint = "Publish one unambiguous schema candidate for the stable identity and requested version.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetSchemaIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.schema_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The available blackboard schema version is outside the decision asset's compatibility interval.",
        .remediationHint = "Keep the last valid plan active and provide a compatible schema or explicit asset migration.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetBindingMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.binding_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision node requires a blackboard key absent from the selected typed schema.",
        .remediationHint = "Declare the stable key in the schema or migrate the node requirement explicitly.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetBindingAmbiguous{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.binding_ambiguous"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Incompatible node requirements claim the same stable blackboard key.",
        .remediationHint = "Make the node contracts agree on key type, cardinality, access, and presence policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetBindingTypeMismatch{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.binding_type_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision node's blackboard key kind or cardinality does not match the selected schema.",
        .remediationHint = "Use the schema's exact typed key contract or perform an authored migration.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetBindingAccessMismatch{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.binding_access_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision node requests blackboard mutability that the schema does not grant.",
        .remediationHint = "Declare the key ReadWrite or change the node to a read-only contract.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetBindingPresenceMismatch{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.binding_presence_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision node requires a present blackboard value that the schema may omit.",
        .remediationHint = "Make the key required or provide an explicit schema default before activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetBindingDefaultMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.binding_default_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision node requires a blackboard default that is absent from the schema.",
        .remediationHint = "Author a validated default for the stable key or remove the node's default requirement.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetSubtreeMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.subtree_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision asset references a subtree or subplan that is not registered.",
        .remediationHint = "Load the referenced asset by stable identity before compiling the parent plan.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetSubtreeAmbiguous{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.subtree_ambiguous"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Multiple decision assets claim one referenced subtree identity.",
        .remediationHint = "Publish one exact subtree asset identity in the active catalog.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetSubtreeIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.subtree_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A referenced subtree has an incompatible plan kind or asset schema version.",
        .remediationHint = "Use an explicitly compatible dependency or recook the parent asset.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetDependencyCycle{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.dependency_cycle"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Decision-asset dependencies contain a cycle that cannot be compiled into a bounded plan.",
        .remediationHint = "Remove the cyclic subtree reference or split the assets at an explicit runtime boundary.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor DecisionAssetStorageUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.storage_unavailable"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "Bounded storage for decision-asset validation or plan publication is unavailable.",
        .remediationHint = "Release memory pressure and retry compilation without replacing the active plan.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor DecisionAssetActivationInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.decision_asset.activation_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A decision-asset activation slot received no clean immutable plan.",
        .remediationHint = "Submit the completed result from a successful decision-asset compilation attempt.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor SceneComponentInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene_component.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An authored AI scene component has invalid typed lifecycle data.",
        .remediationHint = "Use non-zero identities, the current component schema, valid capabilities, and an admitted startup policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ControllerDescriptorMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.controller.descriptor_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An enabled AI controller has no matching immutable descriptor.",
        .remediationHint = "Load the controller descriptor and its decision and blackboard dependencies before scene activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ControllerDescriptorIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.controller.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An authored AI controller does not match its immutable descriptor dependencies.",
        .remediationHint = "Repair the controller, decision-asset, and blackboard-schema identities as one typed binding.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene.capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The active host cannot stage a required AI capability.",
        .remediationHint = "Compose the behavior, navigation, or perception capability before publishing the SceneRuntime generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneActivationInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene.activation_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The AI scene activation candidate is invalid or stale.",
        .remediationHint = "Discard the candidate and prepare a complete generation-fenced AI population again.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AgentCapacityExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene.agent_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded AI scene-agent capacity has been exhausted.",
        .remediationHint = "Reduce admitted scene agents or compose a larger explicit AI capacity.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::AI::AIErrors
