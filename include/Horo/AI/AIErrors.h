#pragma once

/**
 * @file AIErrors.h
 * @brief Stable gameplay-AI contract errors independent of runtime storage.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::AI::AIErrors {
    /** @brief A persistent AI identity uses its reserved zero representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief Two descriptors collide within one persistent identity domain. */
    extern const ErrorCodeDescriptor DescriptorConflict;
    /** @brief A descriptor set exceeds its bounded validation capacity. */
    extern const ErrorCodeDescriptor DescriptorLimitExceeded;
    /** @brief A runtime handle is malformed or belongs to another scene-runtime incarnation. */
    extern const ErrorCodeDescriptor HandleInvalid;
    /** @brief A runtime slot generation cannot advance without wrapping. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief AI task operation context is malformed or crosses runtime/agent generations. */
    extern const ErrorCodeDescriptor TaskContextInvalid;
    /** @brief An AI task lifecycle operation is illegal for its current state. */
    extern const ErrorCodeDescriptor TaskTransitionInvalid;
    /** @brief A failed AI task omitted a valid typed failure family or cause. */
    extern const ErrorCodeDescriptor TaskFailureInvalid;
    /** @brief The selected composition deliberately provides no gameplay-AI runtime. */
    extern const ErrorCodeDescriptor RuntimeUnavailable;
    /** @brief A gameplay-AI runtime or deterministic harness exhausted its declared capacity. */
    extern const ErrorCodeDescriptor TaskCapacityExceeded;
    /** @brief A blackboard schema or key descriptor has an invalid representation. */
    extern const ErrorCodeDescriptor BlackboardSchemaInvalid;
    /** @brief A blackboard schema or collection exceeds its fixed contract capacity. */
    extern const ErrorCodeDescriptor BlackboardLimitExceeded;
    /** @brief A blackboard value does not match its admitted key type. */
    extern const ErrorCodeDescriptor BlackboardValueTypeMismatch;
    /** @brief A blackboard value is non-finite, malformed, or an invalid stored reference. */
    extern const ErrorCodeDescriptor BlackboardValueInvalid;
    /** @brief An unavailable key or value type was supplied under a rejecting schema policy. */
    extern const ErrorCodeDescriptor BlackboardUnknownValueRejected;
    /** @brief Immutable blackboard schema or instance storage could not be allocated. */
    extern const ErrorCodeDescriptor BlackboardStorageUnavailable;
    /** @brief Blackboard runtime/schema/agent generation is stale or foreign. */
    extern const ErrorCodeDescriptor BlackboardInstanceStale;
    /** @brief Blackboard instance binding or required default layout is invalid. */
    extern const ErrorCodeDescriptor BlackboardInstanceInvalid;
    /** @brief Blackboard write batch is duplicate, oversized, or revision-stale. */
    extern const ErrorCodeDescriptor BlackboardBatchInvalid;
    /** @brief Blackboard revision or instance generation cannot advance without wrapping. */
    extern const ErrorCodeDescriptor BlackboardRevisionExhausted;
    /** @brief A blackboard observer registration or generation-fenced token is invalid. */
    extern const ErrorCodeDescriptor BlackboardObserverInvalid;
    /** @brief A blackboard instance has no free bounded observer slot. */
    extern const ErrorCodeDescriptor BlackboardObserverLimitExceeded;
    /** @brief A publication callback attempted to mutate its blackboard or observer registry. */
    extern const ErrorCodeDescriptor BlackboardReentrantMutation;
    /** @brief A perception descriptor, source, capability set, or listener dependency policy is malformed. */
    extern const ErrorCodeDescriptor PerceptionDescriptorInvalid;
    /** @brief A perception descriptor registry exceeds a configured hard-bounded capacity. */
    extern const ErrorCodeDescriptor PerceptionDescriptorLimitExceeded;
    /** @brief A stable perception type identity is contributed more than once. */
    extern const ErrorCodeDescriptor PerceptionDescriptorConflict;
    /** @brief A required sense or stimulus descriptor is not registered. */
    extern const ErrorCodeDescriptor PerceptionDependencyMissing;
    /** @brief A registered perception descriptor falls outside a listener's declared version interval. */
    extern const ErrorCodeDescriptor PerceptionDescriptorIncompatible;
    /** @brief Explicit host capabilities cannot satisfy a required listener. */
    extern const ErrorCodeDescriptor PerceptionCapabilityUnavailable;
    /** @brief Storage for an immutable perception descriptor snapshot is unavailable. */
    extern const ErrorCodeDescriptor PerceptionRegistryStorageUnavailable;
    /** @brief A behavior-tree schema version, identity, or typed node/property contract is malformed. */
    extern const ErrorCodeDescriptor BehaviorTreeSchemaInvalid;
    /** @brief A behavior-tree source exceeds one of its finite node, edge, pin, or payload bounds. */
    extern const ErrorCodeDescriptor BehaviorTreeLimitExceeded;
    /** @brief Stable behavior-tree graph, node, edge, pin, or property identity is duplicated. */
    extern const ErrorCodeDescriptor BehaviorTreeIdentityConflict;
    /** @brief A behavior-tree edge, root, child, service, or pin relationship is invalid. */
    extern const ErrorCodeDescriptor BehaviorTreeTopologyInvalid;
    /** @brief A behavior-tree topology contains a directed cycle. */
    extern const ErrorCodeDescriptor BehaviorTreeCycle;
    /** @brief Immutable behavior-tree asset storage is unavailable. */
    extern const ErrorCodeDescriptor BehaviorTreeStorageUnavailable;
    /** @brief A decision asset identity, kind, version, or typed validation contract is malformed. */
    extern const ErrorCodeDescriptor DecisionAssetSchemaInvalid;
    /** @brief A decision asset catalog, node, dependency, requirement, or diagnostic exceeds a fixed bound. */
    extern const ErrorCodeDescriptor DecisionAssetLimitExceeded;
    /** @brief A required decision node descriptor is not registered. */
    extern const ErrorCodeDescriptor DecisionAssetDescriptorMissing;
    /** @brief More than one compatible decision node descriptor claims one stable type identity. */
    extern const ErrorCodeDescriptor DecisionAssetDescriptorAmbiguous;
    /** @brief A decision node descriptor exists but no version is compatible with the authored requirement. */
    extern const ErrorCodeDescriptor DecisionAssetDescriptorIncompatible;
    /** @brief A required blackboard schema identity is not registered. */
    extern const ErrorCodeDescriptor DecisionAssetSchemaMissing;
    /** @brief More than one compatible blackboard schema candidate claims one stable identity and version. */
    extern const ErrorCodeDescriptor DecisionAssetSchemaAmbiguous;
    /** @brief A registered blackboard schema version is outside the asset's authored compatibility range. */
    extern const ErrorCodeDescriptor DecisionAssetSchemaIncompatible;
    /** @brief A required stable blackboard key is absent from the selected schema. */
    extern const ErrorCodeDescriptor DecisionAssetBindingMissing;
    /** @brief Multiple incompatible binding contracts claim one stable blackboard key. */
    extern const ErrorCodeDescriptor DecisionAssetBindingAmbiguous;
    /** @brief A resolved blackboard key has the wrong typed kind or cardinality. */
    extern const ErrorCodeDescriptor DecisionAssetBindingTypeMismatch;
    /** @brief A resolved blackboard key does not grant the node's required mutability. */
    extern const ErrorCodeDescriptor DecisionAssetBindingAccessMismatch;
    /** @brief A resolved blackboard key cannot satisfy the node's required presence policy. */
    extern const ErrorCodeDescriptor DecisionAssetBindingPresenceMismatch;
    /** @brief A node requires a blackboard default that the selected schema does not provide. */
    extern const ErrorCodeDescriptor DecisionAssetBindingDefaultMissing;
    /** @brief A required subtree or subplan asset is not registered. */
    extern const ErrorCodeDescriptor DecisionAssetSubtreeMissing;
    /** @brief More than one subtree candidate claims one stable asset identity. */
    extern const ErrorCodeDescriptor DecisionAssetSubtreeAmbiguous;
    /** @brief A subtree candidate has the wrong plan kind or incompatible asset schema version. */
    extern const ErrorCodeDescriptor DecisionAssetSubtreeIncompatible;
    /** @brief The decision-asset dependency graph contains a bounded-cycle violation. */
    extern const ErrorCodeDescriptor DecisionAssetDependencyCycle;
    /** @brief A compiled decision plan or validation report could not allocate bounded storage. */
    extern const ErrorCodeDescriptor DecisionAssetStorageUnavailable;
    /** @brief An activation slot was offered a compilation without a clean immutable plan. */
    extern const ErrorCodeDescriptor DecisionAssetActivationInvalid;
}  // namespace Horo::AI::AIErrors
