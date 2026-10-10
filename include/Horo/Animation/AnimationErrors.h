#pragma once

/**
 * @file AnimationErrors.h
 * @brief Stable animation identity, handle, and component-contract failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Animation::AnimationErrors {
    /** @brief A stable or runtime animation identity uses a reserved representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief A runtime handle is malformed or belongs to no published animation object. */
    extern const ErrorCodeDescriptor HandleMalformed;
    /** @brief A runtime handle belongs to another runtime or authored component owner. */
    extern const ErrorCodeDescriptor HandleOwnerMismatch;
    /** @brief A runtime handle or result targets a retired generation. */
    extern const ErrorCodeDescriptor HandleStale;
    /** @brief A runtime generation cannot advance without reusing a previously issued value. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief An animation component contains an invalid or contradictory contract. */
    extern const ErrorCodeDescriptor ComponentInvalid;
    /** @brief A runtime component does not match its immutable authored asset binding. */
    extern const ErrorCodeDescriptor ComponentBindingMismatch;
    /** @brief A component was produced for an unsupported animation contract version. */
    extern const ErrorCodeDescriptor ContractVersionUnsupported;
    /** @brief A skeleton asset was produced for an unsupported contract version. */
    extern const ErrorCodeDescriptor SkeletonVersionUnsupported;
    /** @brief Skeleton validation is closed because the owning boundary is unavailable or shutting down. */
    extern const ErrorCodeDescriptor SkeletonAdmissionRejected;
    /** @brief Skeleton validation was cancelled before immutable publication. */
    extern const ErrorCodeDescriptor SkeletonValidationCancelled;
    /** @brief A reload candidate does not replace the declared stable skeleton identity. */
    extern const ErrorCodeDescriptor SkeletonReloadMismatch;
    /** @brief Skeleton counts, hierarchy depth, or names exceed the captured finite limits. */
    extern const ErrorCodeDescriptor SkeletonLimitExceeded;
    /** @brief Stable joint or socket identity is duplicated within one skeleton asset. */
    extern const ErrorCodeDescriptor SkeletonDuplicateIdentity;
    /** @brief A joint or socket references a joint absent from the candidate skeleton. */
    extern const ErrorCodeDescriptor SkeletonJointMissing;
    /** @brief The candidate parent graph contains a cycle and has no complete topological order. */
    extern const ErrorCodeDescriptor SkeletonHierarchyCycle;
    /** @brief A reference transform or inverse bind matrix is malformed or mutually inconsistent. */
    extern const ErrorCodeDescriptor SkeletonTransformInvalid;
    /** @brief Retarget, mirror, side, name, or socket metadata is malformed or contradictory. */
    extern const ErrorCodeDescriptor SkeletonMetadataInvalid;
    /** @brief Skeletal-mesh skinning data was produced for an unsupported contract version. */
    extern const ErrorCodeDescriptor SkinningVersionUnsupported;
    /** @brief Skinning validation is closed because the owning boundary is unavailable or shutting down. */
    extern const ErrorCodeDescriptor SkinningAdmissionRejected;
    /** @brief Skinning validation was cancelled before immutable publication. */
    extern const ErrorCodeDescriptor SkinningValidationCancelled;
    /** @brief A reload candidate does not replace the declared stable skeletal-mesh identity. */
    extern const ErrorCodeDescriptor SkinningReloadMismatch;
    /** @brief A binding targets another skeleton identity or unsupported skeleton contract. */
    extern const ErrorCodeDescriptor SkinningSkeletonMismatch;
    /** @brief A binding targets a retired skeleton publication generation. */
    extern const ErrorCodeDescriptor SkinningBindingStale;
    /** @brief Skinning counts, ranges, influences, or palettes exceed finite limits. */
    extern const ErrorCodeDescriptor SkinningLimitExceeded;
    /** @brief A mesh-local joint, target joint, section, or LOD identity is duplicated. */
    extern const ErrorCodeDescriptor SkinningDuplicateIdentity;
    /** @brief A remap, palette, or influence references an absent joint. */
    extern const ErrorCodeDescriptor SkinningJointMissing;
    /** @brief A vertex influence is non-finite, non-positive, duplicated, or cannot be normalized. */
    extern const ErrorCodeDescriptor SkinningInfluenceInvalid;
    /** @brief LOD, section, range, or bounds metadata is malformed or contradictory. */
    extern const ErrorCodeDescriptor SkinningLayoutInvalid;
    /** @brief Pose storage was produced for an unsupported public contract version. */
    extern const ErrorCodeDescriptor PoseVersionUnsupported;
    /** @brief Pose storage or evaluation is closed because its owner is unavailable. */
    extern const ErrorCodeDescriptor PoseAdmissionRejected;
    /** @brief The active pose frame was cancelled before publication. */
    extern const ErrorCodeDescriptor PoseEvaluationCancelled;
    /** @brief Pose storage targets another skeleton identity. */
    extern const ErrorCodeDescriptor PoseSkeletonMismatch;
    /** @brief Pose storage targets a retired skeleton publication generation. */
    extern const ErrorCodeDescriptor PoseSkeletonStale;
    /** @brief Pose arena capacity or caller policy exceeds a finite limit. */
    extern const ErrorCodeDescriptor PoseLimitExceeded;
    /** @brief The preallocated frame pose capacity has been consumed. */
    extern const ErrorCodeDescriptor PoseArenaExhausted;
    /** @brief An active immutable lease prevents mutation, reset, cancellation, or shutdown. */
    extern const ErrorCodeDescriptor PoseLeaseConflict;
    /** @brief A local pose transform is non-finite or has an invalid rotation. */
    extern const ErrorCodeDescriptor PoseTransformInvalid;
    /** @brief Pose evaluation references a joint absent from the immutable hierarchy. */
    extern const ErrorCodeDescriptor PoseJointMissing;
    /** @brief A requested model matrix has not been evaluated for the current local pose. */
    extern const ErrorCodeDescriptor PoseNotEvaluated;
    /** @brief A pose operation targets a retired or non-monotonic frame identity. */
    extern const ErrorCodeDescriptor PoseFrameStale;
    /** @brief A mutable pose operation ran outside the arena's declared owner thread. */
    extern const ErrorCodeDescriptor PoseThreadViolation;
    /** @brief An animation clip was produced for an unsupported public contract version. */
    extern const ErrorCodeDescriptor ClipVersionUnsupported;
    /** @brief Clip work is closed because the owning animation boundary is unavailable. */
    extern const ErrorCodeDescriptor ClipAdmissionRejected;
    /** @brief Clip validation, traversal, or sampling was cancelled before publication. */
    extern const ErrorCodeDescriptor ClipOperationCancelled;
    /** @brief A clip reload candidate does not replace the declared stable identity. */
    extern const ErrorCodeDescriptor ClipReloadMismatch;
    /** @brief A clip targets another skeleton identity or skeleton contract. */
    extern const ErrorCodeDescriptor ClipSkeletonMismatch;
    /** @brief A clip, skeleton, or additive reference targets a retired publication generation. */
    extern const ErrorCodeDescriptor ClipBindingStale;
    /** @brief Clip tracks, keys, duration, sample rate, or traversal exceed finite limits. */
    extern const ErrorCodeDescriptor ClipLimitExceeded;
    /** @brief A clip contains duplicate joint tracks or key times. */
    extern const ErrorCodeDescriptor ClipDuplicateIdentity;
    /** @brief A clip track references a joint absent from the bound skeleton. */
    extern const ErrorCodeDescriptor ClipJointMissing;
    /** @brief Clip time, transform, interpolation, wrap, kind, or compression metadata is malformed. */
    extern const ErrorCodeDescriptor ClipMalformed;
    /** @brief The requested clip feature or traversal representation is unsupported. */
    extern const ErrorCodeDescriptor ClipUnsupported;
    /** @brief Exact cursor advancement would overflow its portable time representation. */
    extern const ErrorCodeDescriptor ClipTimeOverflow;
    /** @brief Additive sampling lacks or mismatches its exact reference-pose binding. */
    extern const ErrorCodeDescriptor ClipReferencePoseMismatch;
    /** @brief Animation compression metadata was produced for an unsupported contract version. */
    extern const ErrorCodeDescriptor CompressionVersionUnsupported;
    /** @brief Compression work is closed because its owner is unavailable or shutting down. */
    extern const ErrorCodeDescriptor CompressionAdmissionRejected;
    /** @brief Compression work was cancelled before immutable publication. */
    extern const ErrorCodeDescriptor CompressionOperationCancelled;
    /** @brief A compression reload does not replace the declared source/profile compatibility. */
    extern const ErrorCodeDescriptor CompressionReloadMismatch;
    /** @brief Compression or decompression targets a retired clip or skeleton publication. */
    extern const ErrorCodeDescriptor CompressionBindingStale;
    /** @brief A compression profile contains malformed thresholds or finite limits. */
    extern const ErrorCodeDescriptor CompressionProfileMalformed;
    /** @brief The requested compression tier, scheme, or representation is unsupported. */
    extern const ErrorCodeDescriptor CompressionUnsupported;
    /** @brief Compression cook or decompression exceeds its captured finite work budget. */
    extern const ErrorCodeDescriptor CompressionBudgetExceeded;
    /** @brief Graph schema version requires an unavailable migration. */
    extern const ErrorCodeDescriptor GraphVersionUnsupported;
    /** @brief Graph compilation owner has closed admission. */
    extern const ErrorCodeDescriptor GraphAdmissionRejected;
    /** @brief Graph compilation or migration was cancelled. */
    extern const ErrorCodeDescriptor GraphOperationCancelled;
    /** @brief Graph replacement targets another stable identity. */
    extern const ErrorCodeDescriptor GraphReloadMismatch;
    /** @brief Graph compilation exceeds a captured finite work limit. */
    extern const ErrorCodeDescriptor GraphLimitExceeded;
    /** @brief Graph identities, node schemas or endpoints are malformed. */
    extern const ErrorCodeDescriptor GraphMalformed;
    /** @brief Graph parameter, interface or connection types disagree. */
    extern const ErrorCodeDescriptor GraphTypeMismatch;
    /** @brief Graph nodes or subgraph calls contain a cycle. */
    extern const ErrorCodeDescriptor GraphCycle;
    /** @brief A graph dependency is missing, duplicated, extra, stale or incompatible with its exact skeleton publication. */
    extern const ErrorCodeDescriptor GraphBindingMismatch;
}  // namespace Horo::Animation::AnimationErrors
