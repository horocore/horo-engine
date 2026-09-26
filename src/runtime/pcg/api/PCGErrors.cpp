#include "Horo/PCG/PCGErrors.h"

namespace Horo::PCG::PCGErrors {
    namespace {
        const ErrorDomainId PcgDomain{"horo.pcg"};
    }

    const ErrorCodeDescriptor IdentityInvalid{.domain = PcgDomain,
                                              .code = ErrorCode{"pcg.identity.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The PCG identity uses a reserved representation.",
                                              .remediationHint = "Use identities issued by the owning PCG boundary."};
    const ErrorCodeDescriptor IdentityUnknown{.domain = PcgDomain,
                                              .code = ErrorCode{"pcg.identity.unknown"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "The PCG identity belongs to a different graph or logical owner.",
                                              .remediationHint = "Resolve the identity only through its exact graph owner."};
    const ErrorCodeDescriptor IdentityStale{.domain = PcgDomain,
                                            .code = ErrorCode{"pcg.identity.stale"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "The PCG identity belongs to a retired graph revision or execution.",
                                            .remediationHint = "Discard stale work and resolve the current generation explicitly."};
    const ErrorCodeDescriptor RevisionExhausted{.domain = PcgDomain,
                                                .code = ErrorCode{"pcg.revision.exhausted"},
                                                .defaultSeverity = ErrorSeverity::Critical,
                                                .summary = "The PCG graph revision cannot advance without wrapping.",
                                                .remediationHint = "Retire the graph identity permanently; never reuse a revision."};
    const ErrorCodeDescriptor SerializedIdentityInvalid{.domain = PcgDomain,
                                                        .code = ErrorCode{"pcg.identity.serialized_invalid"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Serialized PCG identity bytes contain a reserved value.",
                                                        .remediationHint = "Reject or recook the malformed PCG payload."};
    const ErrorCodeDescriptor PointSchemaInvalid{.domain = PcgDomain,
                                                 .code = ErrorCode{"pcg.point.schema_invalid"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The PCG point schema is invalid.",
                                                 .remediationHint = "Use bounded canonical namespaced keys and supported attribute types."};
    const ErrorCodeDescriptor PointAttributeDuplicate{.domain = PcgDomain,
                                                      .code = ErrorCode{"pcg.point.attribute_duplicate"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "A PCG point attribute is duplicated.",
                                                      .remediationHint = "Define and provide each canonical attribute exactly once."};
    const ErrorCodeDescriptor PointAttributeUnknown{.domain = PcgDomain,
                                                    .code = ErrorCode{"pcg.point.attribute_unknown"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "A PCG point column is not declared by its schema.",
                                                    .remediationHint = "Remove the unknown column or declare it in the immutable schema."};
    const ErrorCodeDescriptor PointAttributeTypeMismatch{.domain = PcgDomain,
                                                         .code = ErrorCode{"pcg.point.attribute_type_mismatch"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "A PCG point column type is incompatible with its schema.",
                                                         .remediationHint = "Supply the exact closed column type declared by the schema."};
    const ErrorCodeDescriptor PointDataInvalid{.domain = PcgDomain,
                                               .code = ErrorCode{"pcg.point.data_invalid"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "PCG point core or column data is invalid.",
                                               .remediationHint =
                                                   "Use finite transforms, valid bounds, density in [0,1], and equal column lengths."};
    const ErrorCodeDescriptor PointCapacityExceeded{.domain = PcgDomain,
                                                    .code = ErrorCode{"pcg.point.capacity_exceeded"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "A PCG point tier capacity was exceeded.",
                                                    .remediationHint =
                                                        "Reduce point, attribute, payload, or memory size before admission."};
    const ErrorCodeDescriptor PointSizeOverflow{.domain = PcgDomain,
                                                .code = ErrorCode{"pcg.point.size_overflow"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "PCG point byte accounting overflowed.",
                                                .remediationHint =
                                                    "Reject the candidate and compute all byte envelopes with checked arithmetic."};
    const ErrorCodeDescriptor
        SpatialInputInvalid{.domain = PcgDomain,
                            .code = ErrorCode{"pcg.spatial.input_invalid"},
                            .defaultSeverity = ErrorSeverity::Error,
                            .summary = "The PCG spatial snapshot or descriptor is invalid.",
                            .remediationHint =
                                "Provide finite canonical values, valid bounds, stable identities, and non-degenerate primitives."};
    const ErrorCodeDescriptor SpatialCoordinatesUnsupported{.domain = PcgDomain,
                                                            .code = ErrorCode{"pcg.spatial.coordinates_unsupported"},
                                                            .defaultSeverity = ErrorSeverity::Error,
                                                            .summary = "The PCG spatial coordinate contract is unsupported.",
                                                            .remediationHint = "Use the declared Horo axis convention, finite unit scale, "
                                                                               "supported precision, and a valid origin epoch."};
    const ErrorCodeDescriptor
        SpatialCoverageUnavailable{.domain = PcgDomain,
                                   .code = ErrorCode{"pcg.spatial.coverage_unavailable"},
                                   .defaultSeverity = ErrorSeverity::Warning,
                                   .summary = "Required PCG spatial coverage is missing or partial.",
                                   .remediationHint =
                                       "Capture complete committed coverage; never reinterpret unavailable coverage as an empty result."};
    const ErrorCodeDescriptor
        SpatialCapacityExceeded{.domain = PcgDomain,
                                .code = ErrorCode{"pcg.spatial.capacity_exceeded"},
                                .defaultSeverity = ErrorSeverity::Error,
                                .summary = "A PCG spatial snapshot capacity was exceeded.",
                                .remediationHint = "Reduce primitive, control-point, grid-point, or resident-byte demand before capture."};
    const ErrorCodeDescriptor
        SpatialSnapshotStale{.domain = PcgDomain,
                             .code = ErrorCode{"pcg.spatial.snapshot_stale"},
                             .defaultSeverity = ErrorSeverity::Warning,
                             .summary = "The PCG spatial snapshot is no longer logically current.",
                             .remediationHint = "Reject the candidate or explicitly recapture within the caller's bounded retry policy."};
    const ErrorCodeDescriptor
        SpatialReplacementInvalid{.domain = PcgDomain,
                                  .code = ErrorCode{"pcg.spatial.replacement_invalid"},
                                  .defaultSeverity = ErrorSeverity::Error,
                                  .summary = "The PCG spatial replacement does not preserve and advance its lineage.",
                                  .remediationHint =
                                      "Keep provider/source identity and publish a distinct snapshot with a strictly newer revision."};
    const ErrorCodeDescriptor RegistryDescriptorInvalid{.domain = PcgDomain,
                                                        .code = ErrorCode{"pcg.registry.descriptor_invalid"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "A PCG registry descriptor or configuration is invalid.",
                                                        .remediationHint =
                                                            "Compose bounded inert descriptors with valid typed identities."};
    const ErrorCodeDescriptor RegistryDuplicate{.domain = PcgDomain,
                                                .code = ErrorCode{"pcg.registry.duplicate"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "A PCG registry identity is duplicated.",
                                                .remediationHint = "Register each graph and semantic node runtime exactly once."};
    const ErrorCodeDescriptor RegistryCapacityExceeded{.domain = PcgDomain,
                                                       .code = ErrorCode{"pcg.registry.capacity_exceeded"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "A bounded PCG registry is full.",
                                                       .remediationHint = "Reduce host contributions or select an explicit larger bound."};
    const ErrorCodeDescriptor RegistryClosed{.domain = PcgDomain,
                                             .code = ErrorCode{"pcg.registry.closed"},
                                             .defaultSeverity = ErrorSeverity::Warning,
                                             .summary = "The PCG composition registry is closed.",
                                             .remediationHint = "Stop admission or compose a new independently generated registry."};
    const ErrorCodeDescriptor RegistryGenerationExhausted{.domain = PcgDomain,
                                                          .code = ErrorCode{"pcg.registry.generation_exhausted"},
                                                          .defaultSeverity = ErrorSeverity::Critical,
                                                          .summary = "The PCG registry publication generation is exhausted.",
                                                          .remediationHint =
                                                              "Retire the registry permanently instead of reusing a generation."};
    const ErrorCodeDescriptor RegistryHandleInvalid{.domain = PcgDomain,
                                                    .code = ErrorCode{"pcg.registry.handle_invalid"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "A PCG registry handle is malformed.",
                                                    .remediationHint = "Use a complete handle issued by an immutable registry snapshot."};
    const ErrorCodeDescriptor RegistryHandleStale{.domain = PcgDomain,
                                                  .code = ErrorCode{"pcg.registry.handle_stale"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "A PCG registry handle belongs to another publication generation.",
                                                  .remediationHint = "Discard the stale handle and query the intended immutable snapshot."};
    const ErrorCodeDescriptor UnsupportedCapability{.domain = PcgDomain,
                                                    .code = ErrorCode{"pcg.capability.unsupported"},
                                                    .defaultSeverity = ErrorSeverity::Warning,
                                                    .summary = "The exact PCG capability is not projected by this host.",
                                                    .remediationHint = "Install the required capability explicitly or reject the request."};
    const ErrorCodeDescriptor RuntimeUnavailable{.domain = PcgDomain,
                                                 .code = ErrorCode{"pcg.runtime.unavailable"},
                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                 .summary = "The exact PCG node runtime is unavailable.",
                                                 .remediationHint =
                                                     "Register that semantic runtime explicitly; never select another implementation."};
    const ErrorCodeDescriptor GraphSourceMalformed{.domain = PcgDomain,
                                                   .code = ErrorCode{"pcg.graph.source_malformed"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The PCG graph source is malformed.",
                                                   .remediationHint =
                                                       "Reject the source and repair its typed identities, fields, or values."};
    const ErrorCodeDescriptor GraphSourceDuplicate{.domain = PcgDomain,
                                                   .code = ErrorCode{"pcg.graph.source_duplicate"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The PCG graph source contains a duplicate semantic identity.",
                                                   .remediationHint =
                                                       "Assign every graph element and exposed key a unique stable identity."};
    const ErrorCodeDescriptor
        GraphSourceVersionUnsupported{.domain = PcgDomain,
                                      .code = ErrorCode{"pcg.graph.version_unsupported"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "The PCG graph source schema version is unsupported.",
                                      .remediationHint =
                                          "Use an explicit compatible migrator or a reader supporting the persisted schema."};
    const ErrorCodeDescriptor GraphSourceCapacityExceeded{.domain = PcgDomain,
                                                          .code = ErrorCode{"pcg.graph.capacity_exceeded"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "The PCG graph source exceeds a finite schema limit.",
                                                          .remediationHint = "Reduce graph structure or payload before admission."};
    const ErrorCodeDescriptor GraphTopologyInvalid{.domain = PcgDomain,
                                                   .code = ErrorCode{"pcg.graph.topology_invalid"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The PCG graph topology is invalid.",
                                                   .remediationHint = "Repair pin direction, type, cardinality, endpoints, or cycles."};
    const ErrorCodeDescriptor
        GraphNodeTypeUnknown{.domain = PcgDomain,
                             .code = ErrorCode{"pcg.graph.node_type_unknown"},
                             .defaultSeverity = ErrorSeverity::Error,
                             .summary = "The PCG graph references an unavailable node type.",
                             .remediationHint = "Install the exact catalog type or preserve it only for inert authoring round trips."};
    const ErrorCodeDescriptor GraphMigrationFailed{.domain = PcgDomain,
                                                   .code = ErrorCode{"pcg.graph.migration_failed"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The PCG graph source migration failed.",
                                                   .remediationHint =
                                                       "Use a bounded migrator that emits the exact current canonical schema."};
    const ErrorCodeDescriptor GraphReplacementInvalid{.domain = PcgDomain,
                                                      .code = ErrorCode{"pcg.graph.replacement_invalid"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "The PCG graph replacement does not preserve and advance its lineage.",
                                                      .remediationHint = "Preserve GraphId and publish a strictly newer GraphRevision."};
    const ErrorCodeDescriptor GraphLifecycleUnavailable{.domain = PcgDomain,
                                                        .code = ErrorCode{"pcg.graph.lifecycle_unavailable"},
                                                        .defaultSeverity = ErrorSeverity::Warning,
                                                        .summary = "PCG graph-source admission is closed.",
                                                        .remediationHint = "Do not begin source work after cancellation or shutdown."};
    const ErrorCodeDescriptor GraphValidationFailed{.domain = PcgDomain,
                                                    .code = ErrorCode{"pcg.graph.validation_failed"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "The PCG graph failed pre-compile validation.",
                                                    .remediationHint =
                                                        "Repair the typed cause and validate again before compiling or evaluating."};
    const ErrorCodeDescriptor
        GraphValidationCapacityExceeded{.domain = PcgDomain,
                                        .code = ErrorCode{"pcg.graph.validation_capacity_exceeded"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "The PCG graph validation pass exceeded a finite ceiling.",
                                        .remediationHint = "Reduce the graph or select an explicitly larger admitted validation bound."};
    const ErrorCodeDescriptor GenerationPlanInvalid{.domain = PcgDomain,
                                                    .code = ErrorCode{"pcg.generation_plan.invalid"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "A PCG generation plan or its provenance is invalid.",
                                                    .remediationHint =
                                                        "Build a complete typed plan from one exact evaluation and target receipt."};
    const ErrorCodeDescriptor GenerationPlanCapacityExceeded{.domain = PcgDomain,
                                                             .code = ErrorCode{"pcg.generation_plan.capacity_exceeded"},
                                                             .defaultSeverity = ErrorSeverity::Error,
                                                             .summary = "A PCG generation plan exceeds its finite resource envelope.",
                                                             .remediationHint =
                                                                 "Reduce outputs, dependencies, work, or charged bytes before admission."};
    const ErrorCodeDescriptor GenerationPlanStale{.domain = PcgDomain,
                                                  .code = ErrorCode{"pcg.generation_plan.stale"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "The PCG generation plan references stale target or replacement state.",
                                                  .remediationHint = "Recapture exact target evidence and rebuild the plan."};
    const ErrorCodeDescriptor
        GenerationOwnershipMismatch{.domain = PcgDomain,
                                    .code = ErrorCode{"pcg.generation_plan.ownership_mismatch"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "A PCG output delta is not authorized by exact target-owned provenance.",
                                    .remediationHint =
                                        "Update or remove only the exact lineage, set, scope, owner generation, and content."};
    const ErrorCodeDescriptor GenerationPlanLifecycleUnavailable{.domain = PcgDomain,
                                                                 .code = ErrorCode{"pcg.generation_plan.lifecycle_unavailable"},
                                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                                 .summary = "PCG generation-plan admission is closed.",
                                                                 .remediationHint =
                                                                     "Do not build plans after cancellation or shutdown begins."};
    const ErrorCodeDescriptor ProvenanceInvalid{.domain = PcgDomain,
                                                .code = ErrorCode{"pcg.provenance.invalid"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "PCG execution provenance is malformed.",
                                                .remediationHint =
                                                    "Capture valid stable identities, revisions, and canonical content digests."};
    const ErrorCodeDescriptor ProvenanceDuplicate{.domain = PcgDomain,
                                                  .code = ErrorCode{"pcg.provenance.duplicate"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "PCG provenance repeats a semantic identity.",
                                                  .remediationHint =
                                                      "Supply each input, provider source, and output identity exactly once."};
    const ErrorCodeDescriptor ProvenanceCapacityExceeded{.domain = PcgDomain,
                                                         .code = ErrorCode{"pcg.provenance.capacity_exceeded"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "PCG provenance exceeds a finite input or output ceiling.",
                                                         .remediationHint = "Partition or reduce the captured work before admission."};
    const ErrorCodeDescriptor
        ProvenanceTierUnsupported{.domain = PcgDomain,
                                  .code = ErrorCode{"pcg.provenance.tier_unsupported"},
                                  .defaultSeverity = ErrorSeverity::Error,
                                  .summary = "PCG inputs or numeric implementation cannot meet the determinism promise.",
                                  .remediationHint = "Use a certified numeric policy and deterministic inputs or isolate preview work."};
    const ErrorCodeDescriptor ProvenanceStale{.domain = PcgDomain,
                                              .code = ErrorCode{"pcg.provenance.stale"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "Authoritative PCG provenance changed after capture.",
                                              .remediationHint = "Recapture the graph, inputs, providers, and scope before reuse."};
    const ErrorCodeDescriptor ProvenanceLifecycleUnavailable{.domain = PcgDomain,
                                                             .code = ErrorCode{"pcg.provenance.lifecycle_unavailable"},
                                                             .defaultSeverity = ErrorSeverity::Warning,
                                                             .summary = "PCG provenance admission is closed.",
                                                             .remediationHint = "Stop new captures after cancellation or shutdown begins."};
}  // namespace Horo::PCG::PCGErrors
