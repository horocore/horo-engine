#include "Horo/Cinematic/CinematicErrors.h"

namespace Horo::Cinematic::CinematicErrors {
    namespace {
        const ErrorDomainId CinematicDomain{"horo.cinematic"};
    }

    const ErrorCodeDescriptor IdentityInvalid{.domain = CinematicDomain,
                                              .code = ErrorCode{"cinematic.identity.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The cinematic identity uses a reserved representation.",
                                              .remediationHint = "Use an identity issued by the owning sequence document."};
    const ErrorCodeDescriptor IdentityUnknown{.domain = CinematicDomain,
                                              .code = ErrorCode{"cinematic.identity.unknown"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The cinematic identity does not name the requested authored object.",
                                              .remediationHint = "Resolve the identity against its exact sequence owner."};
    const ErrorCodeDescriptor IdentityStale{.domain = CinematicDomain,
                                            .code = ErrorCode{"cinematic.identity.stale"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "The cinematic identity names a retired generation.",
                                            .remediationHint = "Discard the stale reference and obtain the current generation."};
    const ErrorCodeDescriptor GenerationExhausted{.domain = CinematicDomain,
                                                  .code = ErrorCode{"cinematic.identity.generation_exhausted"},
                                                  .defaultSeverity = ErrorSeverity::Critical,
                                                  .summary = "The cinematic identity generation cannot advance without wrapping.",
                                                  .remediationHint =
                                                      "Retire the exhausted identity permanently; never wrap its generation."};
    const ErrorCodeDescriptor SerializedIdentityInvalid{.domain = CinematicDomain,
                                                        .code = ErrorCode{"cinematic.identity.serialized_invalid"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "The serialized cinematic identity contains a reserved value.",
                                                        .remediationHint = "Reject or recook the malformed sequence payload."};
    const ErrorCodeDescriptor SequenceSchemaMalformed{.domain = CinematicDomain,
                                                      .code = ErrorCode{"cinematic.sequence_schema.malformed"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "The sequence source schema is malformed.",
                                                      .remediationHint = "Repair the reported sequence field and save the asset again.",
                                                      .userActionable = true};
    const ErrorCodeDescriptor SequenceSchemaDuplicate{.domain = CinematicDomain,
                                                      .code = ErrorCode{"cinematic.sequence_schema.duplicate"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "The sequence source contains an ambiguous duplicate identity.",
                                                      .remediationHint = "Regenerate the duplicate track or dependency identity.",
                                                      .userActionable = true};
    const ErrorCodeDescriptor SequenceSchemaVersionUnsupported{.domain = CinematicDomain,
                                                               .code = ErrorCode{"cinematic.sequence_schema.version_unsupported"},
                                                               .defaultSeverity = ErrorSeverity::Error,
                                                               .summary = "The sequence source schema version is not directly readable.",
                                                               .remediationHint =
                                                                   "Run the matching sequence migration or upgrade the engine.",
                                                               .userActionable = true};
    const ErrorCodeDescriptor SequenceSchemaLimitExceeded{.domain = CinematicDomain,
                                                          .code = ErrorCode{"cinematic.sequence_schema.limit_exceeded"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "The sequence source exceeds a parser safety limit.",
                                                          .remediationHint = "Reduce the reported source, track, key, or dependency count.",
                                                          .userActionable = true};
    const ErrorCodeDescriptor SequenceCookTierExceeded{.domain = CinematicDomain,
                                                       .code = ErrorCode{"cinematic.sequence_cook.tier_exceeded"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "The sequence exceeds the selected cook tier.",
                                                       .remediationHint =
                                                           "Reduce sequence complexity or explicitly select a larger admitted tier.",
                                                       .userActionable = true};
    const ErrorCodeDescriptor SequenceReferenceMissing{.domain = CinematicDomain,
                                                       .code = ErrorCode{"cinematic.sequence_reference.missing"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "A sequence dependency is missing from the cook snapshot.",
                                                       .remediationHint = "Restore or explicitly repair the referenced asset.",
                                                       .userActionable = true};
    const ErrorCodeDescriptor SequenceReferenceMoved{.domain = CinematicDomain,
                                                     .code = ErrorCode{"cinematic.sequence_reference.move_pending"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "A sequence dependency move is not reconciled in the cook snapshot.",
                                                     .remediationHint =
                                                         "Refresh the Asset Registry and cook again using the same stable AssetId.",
                                                     .retryable = true,
                                                     .userActionable = true};
    const ErrorCodeDescriptor SequenceReferenceUnloadable{.domain = CinematicDomain,
                                                          .code = ErrorCode{"cinematic.sequence_reference.unloadable"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "A sequence dependency cannot be loaded for cooking.",
                                                          .remediationHint = "Repair or republish the referenced asset before cooking.",
                                                          .retryable = true,
                                                          .userActionable = true};
    const ErrorCodeDescriptor SequenceReferenceTypeMismatch{.domain = CinematicDomain,
                                                            .code = ErrorCode{"cinematic.sequence_reference.type_mismatch"},
                                                            .defaultSeverity = ErrorSeverity::Error,
                                                            .summary = "A sequence dependency has an incompatible asset type.",
                                                            .remediationHint = "Assign an asset of the type required by the owning track.",
                                                            .userActionable = true};
    const ErrorCodeDescriptor SequenceReferenceCycle{.domain = CinematicDomain,
                                                     .code = ErrorCode{"cinematic.sequence_reference.cycle"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "The reachable sub-sequence graph contains a cycle.",
                                                     .remediationHint = "Remove one sub-sequence edge from the reported cycle.",
                                                     .userActionable = true};
    const ErrorCodeDescriptor CurveMalformed{.domain = CinematicDomain,
                                             .code = ErrorCode{"cinematic.curve.malformed"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The cinematic curve is malformed.",
                                             .remediationHint = "Repair the key ordering or curve policy and cook again.",
                                             .userActionable = true};
    const ErrorCodeDescriptor CurveNonFinite{.domain = CinematicDomain,
                                             .code = ErrorCode{"cinematic.curve.non_finite"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The cinematic curve contains a non-finite value.",
                                             .remediationHint = "Replace NaN or infinite key and tangent values before cooking.",
                                             .userActionable = true};
    const ErrorCodeDescriptor CurveTangentInvalid{.domain = CinematicDomain,
                                                  .code = ErrorCode{"cinematic.curve.tangent_invalid"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "The cinematic curve has an invalid tangent.",
                                                  .remediationHint = "Keep time handles monotonic and inside the owning segment.",
                                                  .userActionable = true};
    const ErrorCodeDescriptor CurveLimitExceeded{.domain = CinematicDomain,
                                                 .code = ErrorCode{"cinematic.curve.limit_exceeded"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The cinematic curve exceeds its sampling capacity.",
                                                 .remediationHint = "Reduce the curve key count or split the authored track.",
                                                 .userActionable = true};
    const ErrorCodeDescriptor TransformVersionUnsupported{.domain = CinematicDomain,
                                                          .code = ErrorCode{"cinematic.transform.version_unsupported"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "The transform-track version is unsupported.",
                                                          .remediationHint =
                                                              "Migrate or recook the transform track for this engine version.",
                                                          .userActionable = true};
    const ErrorCodeDescriptor TransformBindingStale{.domain = CinematicDomain,
                                                    .code = ErrorCode{"cinematic.transform.binding_stale"},
                                                    .defaultSeverity = ErrorSeverity::Warning,
                                                    .summary = "The transform binding belongs to a retired scene generation.",
                                                    .remediationHint = "Rebuild the evaluation plan from the active scene snapshot.",
                                                    .retryable = true};
    const ErrorCodeDescriptor TransformBindingMissing{.domain = CinematicDomain,
                                                      .code = ErrorCode{"cinematic.transform.binding_missing"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "A required transform binding is missing.",
                                                      .remediationHint = "Restore the bound object or repair the track hierarchy.",
                                                      .userActionable = true};
    const ErrorCodeDescriptor TransformHierarchyCycle{.domain = CinematicDomain,
                                                      .code = ErrorCode{"cinematic.transform.hierarchy_cycle"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "The transform binding hierarchy contains a cycle.",
                                                      .remediationHint = "Remove one parent edge from the reported hierarchy.",
                                                      .userActionable = true};
    const ErrorCodeDescriptor TransformMalformed{.domain = CinematicDomain,
                                                 .code = ErrorCode{"cinematic.transform.malformed"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The transform-track contract is malformed.",
                                                 .remediationHint =
                                                     "Repair track identities, anchors, or binding relationships and recook.",
                                                 .userActionable = true};
    const ErrorCodeDescriptor TransformLimitExceeded{.domain = CinematicDomain,
                                                     .code = ErrorCode{"cinematic.transform.limit_exceeded"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Transform evaluation exceeds a bounded capacity.",
                                                     .remediationHint = "Reduce admitted tracks or provide the required output capacity."};
    const ErrorCodeDescriptor TransformSampleInvalid{.domain = CinematicDomain,
                                                     .code = ErrorCode{"cinematic.transform.sample_invalid"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Sampled transform channels do not form a finite transform.",
                                                     .remediationHint =
                                                         "Repair curve values, quaternion channels, or the active origin frame.",
                                                     .userActionable = true};
    const ErrorCodeDescriptor PropertyVersionUnsupported{.domain = CinematicDomain,
                                                         .code = ErrorCode{"cinematic.property.version_unsupported"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "The property-track version is unsupported.",
                                                         .remediationHint = "Migrate or recook the property track for this engine version.",
                                                         .userActionable = true};
    const ErrorCodeDescriptor PropertyBindingMissing{.domain = CinematicDomain,
                                                     .code = ErrorCode{"cinematic.property.binding_missing"},
                                                     .defaultSeverity = ErrorSeverity::Warning,
                                                     .summary = "The property track binding is not available in the active registry.",
                                                     .remediationHint =
                                                         "Revalidate the scene schema and acquire the current binding generation.",
                                                     .retryable = true,
                                                     .userActionable = true};
    const ErrorCodeDescriptor PropertyRegistryUnfrozen{.domain = CinematicDomain,
                                                       .code = ErrorCode{"cinematic.property.registry_unfrozen"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Property activation requires a frozen binding registry.",
                                                       .remediationHint =
                                                           "Complete host composition and freeze the binding snapshot before activation.",
                                                       .userActionable = true};
    const ErrorCodeDescriptor PropertyBindingStale{.domain = CinematicDomain,
                                                   .code = ErrorCode{"cinematic.property.binding_stale"},
                                                   .defaultSeverity = ErrorSeverity::Warning,
                                                   .summary = "The property target belongs to a retired scene or component generation.",
                                                   .remediationHint =
                                                       "Rebuild the property evaluation plan from the active scene snapshot.",
                                                   .retryable = true};
    const ErrorCodeDescriptor PropertyBindingTargetMissing{.domain = CinematicDomain,
                                                           .code = ErrorCode{"cinematic.property.target_missing"},
                                                           .defaultSeverity = ErrorSeverity::Warning,
                                                           .summary =
                                                               "The property track target is not present in the active scene snapshot.",
                                                           .remediationHint = "Restore the target object or repair the authored binding.",
                                                           .retryable = true,
                                                           .userActionable = true};
    const ErrorCodeDescriptor PropertyComponentMismatch{.domain = CinematicDomain,
                                                        .code = ErrorCode{"cinematic.property.component_mismatch"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "The property target component type does not match its binding.",
                                                        .remediationHint =
                                                            "Resolve the target against the exact component type declared by the binding.",
                                                        .userActionable = true};
    const ErrorCodeDescriptor PropertyTypeMismatch{.domain = CinematicDomain,
                                                   .code = ErrorCode{"cinematic.property.type_mismatch"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The property track type does not match its binding descriptor.",
                                                   .remediationHint = "Use curves with the binding's declared typed value shape.",
                                                   .userActionable = true};
    const ErrorCodeDescriptor PropertyMalformed{.domain = CinematicDomain,
                                                .code = ErrorCode{"cinematic.property.malformed"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The property-track contract is malformed.",
                                                .remediationHint =
                                                    "Repair identities, target metadata, or typed curve channels and recook.",
                                                .userActionable = true};
    const ErrorCodeDescriptor PropertyLimitExceeded{.domain = CinematicDomain,
                                                    .code = ErrorCode{"cinematic.property.limit_exceeded"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Property evaluation exceeds a bounded capacity.",
                                                    .remediationHint =
                                                        "Reduce admitted tracks or provide the required caller output capacity."};
    const ErrorCodeDescriptor PropertySampleInvalid{.domain = CinematicDomain,
                                                    .code = ErrorCode{"cinematic.property.sample_invalid"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Sampled property channels are invalid for the binding.",
                                                    .remediationHint =
                                                        "Repair non-finite curve values or the binding's finite range constraint.",
                                                    .userActionable = true};
    const ErrorCodeDescriptor PropertyWriteRejected{.domain = CinematicDomain,
                                                    .code = ErrorCode{"cinematic.property.write_rejected"},
                                                    .defaultSeverity = ErrorSeverity::Warning,
                                                    .summary = "The property owner rejected a typed cinematic write.",
                                                    .remediationHint =
                                                        "Use the owner's admitted write phase or inspect the surfaced binding diagnostic.",
                                                    .retryable = true};
}  // namespace Horo::Cinematic::CinematicErrors
