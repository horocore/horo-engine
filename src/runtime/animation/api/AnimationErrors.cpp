#include "Horo/Animation/AnimationErrors.h"

namespace Horo::Animation::AnimationErrors {
    namespace {
        const ErrorDomainId AnimationDomain{"horo.animation"};
    }

    const ErrorCodeDescriptor IdentityInvalid{.domain = AnimationDomain,
                                              .code = ErrorCode{"animation.identity.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The animation identity uses a reserved representation.",
                                              .remediationHint = "Use a typed identity issued by the owning asset or animation boundary."};
    const ErrorCodeDescriptor HandleMalformed{.domain = AnimationDomain,
                                              .code = ErrorCode{"animation.handle.malformed"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The animation handle is incomplete or malformed.",
                                              .remediationHint = "Resolve a current non-owning handle from the animation runtime."};
    const ErrorCodeDescriptor HandleOwnerMismatch{.domain = AnimationDomain,
                                                  .code = ErrorCode{"animation.handle.owner_mismatch"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "The animation handle belongs to another runtime or authored component.",
                                                  .remediationHint =
                                                      "Submit the handle only to the runtime and component owner that issued it."};
    const ErrorCodeDescriptor HandleStale{.domain = AnimationDomain,
                                          .code = ErrorCode{"animation.handle.stale"},
                                          .defaultSeverity = ErrorSeverity::Warning,
                                          .summary = "The animation handle targets a retired generation.",
                                          .remediationHint = "Discard cached handles and resolve the current runtime generation."};
    const ErrorCodeDescriptor GenerationExhausted{.domain = AnimationDomain,
                                                  .code = ErrorCode{"animation.generation.exhausted"},
                                                  .defaultSeverity = ErrorSeverity::Critical,
                                                  .summary = "An animation generation cannot advance without reuse.",
                                                  .remediationHint =
                                                      "Retire the owning runtime or slot instead of wrapping its generation."};
    const ErrorCodeDescriptor ComponentInvalid{.domain = AnimationDomain,
                                               .code = ErrorCode{"animation.component.invalid"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The animation component contract is invalid or contradictory.",
                                               .remediationHint = "Correct its typed source, policy, domain, state, and pose ordering."};
    const ErrorCodeDescriptor ComponentBindingMismatch{.domain = AnimationDomain,
                                                       .code = ErrorCode{"animation.component.binding_mismatch"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "The runtime animation projection does not match its authored binding.",
                                                       .remediationHint =
                                                           "Rebuild the runtime component from the current authored asset identities."};
    const ErrorCodeDescriptor ContractVersionUnsupported{.domain = AnimationDomain,
                                                         .code = ErrorCode{"animation.contract.version_unsupported"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "The animation component contract version is unsupported.",
                                                         .remediationHint =
                                                             "Migrate or recook the component for the current animation contract."};
    const ErrorCodeDescriptor SkeletonVersionUnsupported{AnimationDomain, ErrorCode{"animation.skeleton.version_unsupported"},
                                                         ErrorSeverity::Error, "The skeleton asset contract version is unsupported.",
                                                         "Migrate or recook the skeleton for the current contract."};
    const ErrorCodeDescriptor SkeletonAdmissionRejected{AnimationDomain, ErrorCode{"animation.skeleton.admission_rejected"},
                                                        ErrorSeverity::Warning, "The skeleton owner is not accepting validation work.",
                                                        "Retry only after a current asset owner resumes admission."};
    const ErrorCodeDescriptor SkeletonValidationCancelled{AnimationDomain, ErrorCode{"animation.skeleton.validation_cancelled"},
                                                          ErrorSeverity::Warning, "Skeleton validation was cancelled before publication.",
                                                          "Submit a new candidate under a current owner operation."};
    const ErrorCodeDescriptor SkeletonReloadMismatch{AnimationDomain, ErrorCode{"animation.skeleton.reload_mismatch"}, ErrorSeverity::Error,
                                                     "The skeleton reload candidate has a different stable identity.",
                                                     "Publish different identities as separate assets instead of a reload."};
    const ErrorCodeDescriptor SkeletonLimitExceeded{AnimationDomain, ErrorCode{"animation.skeleton.limit_exceeded"}, ErrorSeverity::Error,
                                                    "The skeleton exceeds a finite validation limit.",
                                                    "Reduce joints, depth, sockets, or metadata to the captured limits."};
    const ErrorCodeDescriptor SkeletonDuplicateIdentity{AnimationDomain, ErrorCode{"animation.skeleton.duplicate_identity"},
                                                        ErrorSeverity::Error, "The skeleton contains a duplicate stable identity.",
                                                        "Assign every joint and socket one unique stable identity."};
    const ErrorCodeDescriptor SkeletonJointMissing{AnimationDomain, ErrorCode{"animation.skeleton.joint_missing"}, ErrorSeverity::Error,
                                                   "Skeleton hierarchy or metadata references a missing joint.",
                                                   "Reference only joints declared by the same skeleton candidate."};
    const ErrorCodeDescriptor SkeletonHierarchyCycle{AnimationDomain, ErrorCode{"animation.skeleton.hierarchy_cycle"}, ErrorSeverity::Error,
                                                     "The skeleton parent hierarchy contains a cycle.",
                                                     "Break the cycle so every joint reaches one root."};
    const ErrorCodeDescriptor SkeletonTransformInvalid{AnimationDomain, ErrorCode{"animation.skeleton.transform_invalid"},
                                                       ErrorSeverity::Error, "Skeleton reference or inverse bind transforms are invalid.",
                                                       "Provide finite affine transforms and matching inverse bind matrices."};
    const ErrorCodeDescriptor SkeletonMetadataInvalid{AnimationDomain, ErrorCode{"animation.skeleton.metadata_invalid"},
                                                      ErrorSeverity::Error,
                                                      "Skeleton joint, mirror, retarget, or socket metadata is invalid.",
                                                      "Correct typed metadata and bounded advisory names."};
    const ErrorCodeDescriptor SkinningVersionUnsupported{AnimationDomain, ErrorCode{"animation.skinning.version_unsupported"},
                                                         ErrorSeverity::Error,
                                                         "The skeletal-mesh skinning contract version is unsupported.",
                                                         "Migrate or recook the mesh for the current skinning contract."};
    const ErrorCodeDescriptor SkinningAdmissionRejected{AnimationDomain, ErrorCode{"animation.skinning.admission_rejected"},
                                                        ErrorSeverity::Warning, "The skinning owner is not accepting validation work.",
                                                        "Retry only after a current asset owner resumes admission."};
    const ErrorCodeDescriptor SkinningValidationCancelled{AnimationDomain, ErrorCode{"animation.skinning.validation_cancelled"},
                                                          ErrorSeverity::Warning, "Skinning validation was cancelled before publication.",
                                                          "Submit a new candidate under a current owner operation."};
    const ErrorCodeDescriptor SkinningReloadMismatch{AnimationDomain, ErrorCode{"animation.skinning.reload_mismatch"}, ErrorSeverity::Error,
                                                     "The reload candidate has a different skeletal-mesh identity.",
                                                     "Publish different mesh identities as separate assets instead of a reload."};
    const ErrorCodeDescriptor SkinningSkeletonMismatch{AnimationDomain, ErrorCode{"animation.skinning.skeleton_mismatch"},
                                                       ErrorSeverity::Error, "The skinning binding targets an incompatible skeleton.",
                                                       "Bind the mesh to the exact validated skeleton identity and contract version."};
    const ErrorCodeDescriptor SkinningBindingStale{AnimationDomain, ErrorCode{"animation.skinning.binding_stale"}, ErrorSeverity::Warning,
                                                   "The skinning binding targets a retired skeleton generation.",
                                                   "Rebuild the binding against the current immutable skeleton publication."};
    const ErrorCodeDescriptor
        SkinningLimitExceeded{AnimationDomain, ErrorCode{"animation.skinning.limit_exceeded"}, ErrorSeverity::Error,
                              "The skinning asset exceeds a finite validation limit.",
                              "Reduce LODs, vertices, sections, joints, palettes, or influences to the captured limits."};
    const ErrorCodeDescriptor SkinningDuplicateIdentity{AnimationDomain, ErrorCode{"animation.skinning.duplicate_identity"},
                                                        ErrorSeverity::Error, "The skinning asset contains a duplicate stable identity.",
                                                        "Assign unique mesh joints, skeleton targets, sections, and LOD levels."};
    const ErrorCodeDescriptor SkinningJointMissing{AnimationDomain, ErrorCode{"animation.skinning.joint_missing"}, ErrorSeverity::Error,
                                                   "A skinning remap, palette, or influence references an absent joint.",
                                                   "Reference only joints declared by the binding and target skeleton."};
    const ErrorCodeDescriptor SkinningInfluenceInvalid{AnimationDomain, ErrorCode{"animation.skinning.influence_invalid"},
                                                       ErrorSeverity::Error, "A vertex influence set is malformed or cannot be normalized.",
                                                       "Provide unique finite positive weights for bounded palette joints."};
    const ErrorCodeDescriptor SkinningLayoutInvalid{AnimationDomain, ErrorCode{"animation.skinning.layout_invalid"}, ErrorSeverity::Error,
                                                    "Skeletal-mesh LOD, section, range, or bounds metadata is invalid.",
                                                    "Provide contiguous sections, valid bounds, and canonical LOD levels."};
    const ErrorCodeDescriptor PoseVersionUnsupported{AnimationDomain, ErrorCode{"animation.pose.version_unsupported"}, ErrorSeverity::Error,
                                                     "The pose-storage contract version is unsupported.",
                                                     "Create storage using the current pose contract version."};
    const ErrorCodeDescriptor PoseAdmissionRejected{AnimationDomain, ErrorCode{"animation.pose.admission_rejected"}, ErrorSeverity::Warning,
                                                    "The animation owner is not accepting pose work.",
                                                    "Submit work only to an active owner-controlled frame arena."};
    const ErrorCodeDescriptor PoseEvaluationCancelled{AnimationDomain, ErrorCode{"animation.pose.evaluation_cancelled"},
                                                      ErrorSeverity::Warning, "The active pose frame was cancelled before publication.",
                                                      "Begin a newer frame before submitting replacement work."};
    const ErrorCodeDescriptor PoseSkeletonMismatch{AnimationDomain, ErrorCode{"animation.pose.skeleton_mismatch"}, ErrorSeverity::Error,
                                                   "The pose arena targets another skeleton identity.",
                                                   "Use the exact immutable skeleton bound when the arena was created."};
    const ErrorCodeDescriptor PoseSkeletonStale{AnimationDomain, ErrorCode{"animation.pose.skeleton_stale"}, ErrorSeverity::Warning,
                                                "The pose arena targets a retired skeleton publication.",
                                                "Recreate pose storage for the current skeleton generation."};
    const ErrorCodeDescriptor PoseLimitExceeded{AnimationDomain, ErrorCode{"animation.pose.limit_exceeded"}, ErrorSeverity::Error,
                                                "Pose storage exceeds a finite capacity or hierarchy limit.",
                                                "Reduce pose or joint capacity to the captured hard limits."};
    const ErrorCodeDescriptor PoseArenaExhausted{AnimationDomain, ErrorCode{"animation.pose.arena_exhausted"}, ErrorSeverity::Warning,
                                                 "The preallocated frame pose arena is exhausted.",
                                                 "Increase bounded setup capacity or reduce poses produced this frame."};
    const ErrorCodeDescriptor PoseLeaseConflict{AnimationDomain, ErrorCode{"animation.pose.lease_conflict"}, ErrorSeverity::Warning,
                                                "An immutable lease pins storage required for mutation or retirement.",
                                                "Release the consumer lease at its documented synchronization point."};
    const ErrorCodeDescriptor PoseTransformInvalid{AnimationDomain, ErrorCode{"animation.pose.transform_invalid"}, ErrorSeverity::Error,
                                                   "A local pose transform is non-finite or has an invalid rotation.",
                                                   "Provide finite transform values and a normalizable rotation."};
    const ErrorCodeDescriptor PoseJointMissing{AnimationDomain, ErrorCode{"animation.pose.joint_missing"}, ErrorSeverity::Error,
                                               "Pose evaluation references a joint absent from the immutable hierarchy.",
                                               "Use stable joint identities declared by the arena skeleton."};
    const ErrorCodeDescriptor PoseNotEvaluated{AnimationDomain, ErrorCode{"animation.pose.not_evaluated"}, ErrorSeverity::Warning,
                                               "The requested model-space matrix remains dirty.",
                                               "Evaluate the requested joint and its ancestors before reading it."};
    const ErrorCodeDescriptor PoseFrameStale{AnimationDomain, ErrorCode{"animation.pose.frame_stale"}, ErrorSeverity::Warning,
                                             "The pose operation targets a retired or non-monotonic frame.",
                                             "Discard frame-local handles and begin a strictly newer frame."};
    const ErrorCodeDescriptor PoseThreadViolation{AnimationDomain, ErrorCode{"animation.pose.thread_violation"}, ErrorSeverity::Error,
                                                  "A mutable pose operation ran outside the arena owner thread.",
                                                  "Route arena mutation through the animation runtime owner thread."};
    const ErrorCodeDescriptor ClipVersionUnsupported{AnimationDomain, ErrorCode{"animation.clip.version_unsupported"}, ErrorSeverity::Error,
                                                     "The animation clip contract version is unsupported.",
                                                     "Migrate or recook the clip for the current animation contract."};
    const ErrorCodeDescriptor ClipAdmissionRejected{AnimationDomain, ErrorCode{"animation.clip.admission_rejected"}, ErrorSeverity::Warning,
                                                    "The animation owner is not accepting clip work.",
                                                    "Submit work only while the current animation owner admits it."};
    const ErrorCodeDescriptor ClipOperationCancelled{AnimationDomain, ErrorCode{"animation.clip.operation_cancelled"},
                                                     ErrorSeverity::Warning, "Animation clip work was cancelled before publication.",
                                                     "Submit a new operation against current immutable publications."};
    const ErrorCodeDescriptor ClipReloadMismatch{AnimationDomain, ErrorCode{"animation.clip.reload_mismatch"}, ErrorSeverity::Error,
                                                 "The animation clip reload candidate has a different stable identity.",
                                                 "Publish different clip identities as separate assets instead of a reload."};
    const ErrorCodeDescriptor ClipSkeletonMismatch{AnimationDomain, ErrorCode{"animation.clip.skeleton_mismatch"}, ErrorSeverity::Error,
                                                   "The animation clip targets an incompatible skeleton.",
                                                   "Bind the clip to the exact validated skeleton identity and contract."};
    const ErrorCodeDescriptor ClipBindingStale{AnimationDomain, ErrorCode{"animation.clip.binding_stale"}, ErrorSeverity::Warning,
                                               "The animation clip targets a retired immutable publication.",
                                               "Resolve the current clip, skeleton, and additive reference generations."};
    const ErrorCodeDescriptor ClipLimitExceeded{AnimationDomain, ErrorCode{"animation.clip.limit_exceeded"}, ErrorSeverity::Error,
                                                "The animation clip or traversal exceeds a finite limit.",
                                                "Reduce tracks, keys, duration, sample rate, or interval crossings."};
    const ErrorCodeDescriptor ClipDuplicateIdentity{AnimationDomain, ErrorCode{"animation.clip.duplicate_identity"}, ErrorSeverity::Error,
                                                    "The animation clip contains duplicate stable tracks or key times.",
                                                    "Assign one track per joint and one key per exact local time."};
    const ErrorCodeDescriptor ClipJointMissing{AnimationDomain, ErrorCode{"animation.clip.joint_missing"}, ErrorSeverity::Error,
                                               "An animation clip track references a missing skeleton joint.",
                                               "Reference only stable joints declared by the bound skeleton."};
    const ErrorCodeDescriptor ClipMalformed{AnimationDomain, ErrorCode{"animation.clip.malformed"}, ErrorSeverity::Error,
                                            "Animation clip time, transform, or typed metadata is malformed.",
                                            "Provide canonical finite keys and known typed clip metadata."};
    const ErrorCodeDescriptor ClipUnsupported{AnimationDomain, ErrorCode{"animation.clip.unsupported"}, ErrorSeverity::Error,
                                              "The requested animation clip feature is unsupported.",
                                              "Recook the clip with a supported interpolation, wrap, kind, and compression contract."};
    const ErrorCodeDescriptor ClipTimeOverflow{AnimationDomain, ErrorCode{"animation.clip.time_overflow"}, ErrorSeverity::Error,
                                               "Exact animation cursor advancement overflowed its portable time domain.",
                                               "Reduce the player rate or traversal interval before retrying the tick."};
    const ErrorCodeDescriptor ClipReferencePoseMismatch{AnimationDomain, ErrorCode{"animation.clip.reference_pose_mismatch"},
                                                        ErrorSeverity::Error,
                                                        "Additive sampling lacks the exact immutable reference-pose binding.",
                                                        "Resolve the authored reference pose identity and current generation."};
    const ErrorCodeDescriptor CompressionVersionUnsupported{AnimationDomain, ErrorCode{"animation.compression.version_unsupported"},
                                                            ErrorSeverity::Error,
                                                            "The animation compression contract version is unsupported.",
                                                            "Recook the clip using the current compression contract."};
    const ErrorCodeDescriptor CompressionAdmissionRejected{AnimationDomain, ErrorCode{"animation.compression.admission_rejected"},
                                                           ErrorSeverity::Warning, "The animation compression owner is not accepting work.",
                                                           "Retry only after the current owner resumes admission."};
    const ErrorCodeDescriptor CompressionOperationCancelled{AnimationDomain, ErrorCode{"animation.compression.operation_cancelled"},
                                                            ErrorSeverity::Warning,
                                                            "Animation compression work was cancelled before publication.",
                                                            "Submit a new cook against current immutable inputs."};
    const ErrorCodeDescriptor CompressionReloadMismatch{AnimationDomain, ErrorCode{"animation.compression.reload_mismatch"},
                                                        ErrorSeverity::Error,
                                                        "The compression reload targets another source or profile compatibility.",
                                                        "Publish incompatible source or profile bindings as a distinct artifact."};
    const ErrorCodeDescriptor CompressionBindingStale{AnimationDomain, ErrorCode{"animation.compression.binding_stale"},
                                                      ErrorSeverity::Warning,
                                                      "Compression work targets a retired clip or skeleton publication.",
                                                      "Resolve and cook against current immutable generations."};
    const ErrorCodeDescriptor CompressionProfileMalformed{AnimationDomain, ErrorCode{"animation.compression.profile_malformed"},
                                                          ErrorSeverity::Error, "The animation compression profile is malformed.",
                                                          "Use finite non-negative thresholds and bounded non-zero limits."};
    const ErrorCodeDescriptor CompressionUnsupported{AnimationDomain, ErrorCode{"animation.compression.unsupported"}, ErrorSeverity::Error,
                                                     "The requested animation compression representation is unsupported.",
                                                     "Select a supported typed tier and compression scheme."};
    const ErrorCodeDescriptor CompressionBudgetExceeded{AnimationDomain, ErrorCode{"animation.compression.budget_exceeded"},
                                                        ErrorSeverity::Warning,
                                                        "Animation compression exceeded its captured finite work budget.",
                                                        "Increase the bounded profile budget or reduce source data."};
    const ErrorCodeDescriptor GraphVersionUnsupported{AnimationDomain, ErrorCode{"animation.graph.version_unsupported"},
                                                      ErrorSeverity::Error, "Graph schema version requires an unavailable migration.",
                                                      "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphAdmissionRejected{AnimationDomain, ErrorCode{"animation.graph.admission_rejected"}, ErrorSeverity::Error,
                                                     "Graph compilation owner has closed admission.",
                                                     "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphOperationCancelled{AnimationDomain, ErrorCode{"animation.graph.operation_cancelled"},
                                                      ErrorSeverity::Error, "Graph compilation or migration was cancelled.",
                                                      "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphReloadMismatch{AnimationDomain, ErrorCode{"animation.graph.reload_mismatch"}, ErrorSeverity::Error,
                                                  "Graph replacement targets another stable identity.",
                                                  "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphLimitExceeded{AnimationDomain, ErrorCode{"animation.graph.limit_exceeded"}, ErrorSeverity::Error,
                                                 "Graph compilation exceeds a captured finite work limit.",
                                                 "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphMalformed{AnimationDomain, ErrorCode{"animation.graph.malformed"}, ErrorSeverity::Error,
                                             "Graph identities, node schemas or endpoints are malformed.",
                                             "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphTypeMismatch{AnimationDomain, ErrorCode{"animation.graph.type_mismatch"}, ErrorSeverity::Error,
                                                "Graph parameter, interface or connection types disagree.",
                                                "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor GraphCycle{AnimationDomain, ErrorCode{"animation.graph.cycle"}, ErrorSeverity::Error,
                                         "Graph nodes or subgraph calls contain a cycle.",
                                         "Correct the candidate or resume admission before compiling."};
    const ErrorCodeDescriptor
        GraphBindingMismatch{AnimationDomain, ErrorCode{"animation.graph.binding_mismatch"}, ErrorSeverity::Error,
                             "Graph dependencies do not match the exact immutable skeleton publication.",
                             "Resolve and pin the exact validated skeleton and clip dependencies before compilation."};
}  // namespace Horo::Animation::AnimationErrors
