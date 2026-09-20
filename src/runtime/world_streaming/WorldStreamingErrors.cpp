#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    namespace {
        const ErrorDomainId Domain{"horo.world_streaming"};

        /** @brief Builds an immutable descriptor in the world-streaming error domain. */
        [[nodiscard]] ErrorCodeDescriptor Describe(const char *code, const ErrorSeverity severity, const char *summary,
                                                   const char *remediationHint, const bool userActionable) {
            return {
                .domain = Domain,
                .code = ErrorCode{code},
                .defaultSeverity = severity,
                .summary = summary,
                .remediationHint = remediationHint,
                .retryable = false,
                .userActionable = userActionable,
            };
        }
    }  // namespace

    const ErrorCodeDescriptor NetworkStreamingAuthorityInvalid =
        Describe("world_streaming.network_authority.invalid", ErrorSeverity::Error,
                 "A network-streaming authority configuration, command, report, or readiness proof is malformed.",
                 "Provide complete typed peer, partition, cell, sequence, local-owner and readiness evidence.", true);
    const ErrorCodeDescriptor NetworkStreamingAuthorityUnsupported =
        Describe("world_streaming.network_authority.unsupported", ErrorSeverity::Error,
                 "A network-streaming intent, readiness disposition, or local residency proof is unsupported.",
                 "Use a supported server intent and a terminal client readiness result satisfying the current requirement.", true);
    const ErrorCodeDescriptor NetworkStreamingAuthorityStale =
        Describe("world_streaming.network_authority.stale", ErrorSeverity::Warning,
                 "A network-streaming command or report no longer names the current peer, sequence, partition, cell, or local epoch.",
                 "Refresh the peer intent snapshot and local residency proof before retrying.", false);
    const ErrorCodeDescriptor NetworkStreamingAuthorityCapacityExceeded =
        Describe("world_streaming.network_authority.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded client relevance snapshot cannot admit another cell.",
                 "Release an obsolete relevance command or select a supported larger host ceiling.", false);
    const ErrorCodeDescriptor NetworkStreamingAuthorityLifecycleUnavailable =
        Describe("world_streaming.network_authority.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Network-streaming server-intent admission is cancelled or protocol admission is shut down.",
                 "Use an active peer and mounted client-partition owner lifetime.", false);
    const ErrorCodeDescriptor NetworkStreamingReadinessInvalid =
        Describe("world_streaming.network_authority.readiness_invalid", ErrorSeverity::Error,
                 "The client readiness result does not carry a current local residency proof satisfying server intent.",
                 "Report Ready only with the exact local cell fence and sufficient Resident or Active state.", false);

    const ErrorCodeDescriptor OriginFrameInvalid =
        Describe("world_streaming.origin_frame.invalid", ErrorSeverity::Error,
                 "An origin-frame binding or externally supplied local coordinate is malformed.",
                 "Provide non-zero typed frame identities and finite millimeter-aligned local coordinates.", true);
    const ErrorCodeDescriptor OriginFrameStale =
        Describe("world_streaming.origin_frame.stale", ErrorSeverity::Warning,
                 "An origin-frame candidate, local coordinate, or lease no longer names the active generation.",
                 "Capture the current origin-frame publication and regenerate the local coordinate before retrying.", false);
    const ErrorCodeDescriptor OriginFrameRangeExceeded =
        Describe("world_streaming.origin_frame.range_exceeded", ErrorSeverity::Error,
                 "A global/local conversion exceeds the supported local frame or signed global coordinate range.",
                 "Rebase to a nearer canonical origin and keep every local axis inside the declared half-extent.", false);
    const ErrorCodeDescriptor OriginFramePrecisionLoss =
        Describe("world_streaming.origin_frame.precision_loss", ErrorSeverity::Error,
                 "An externally supplied local coordinate cannot preserve canonical millimeter precision.",
                 "Provide finite local meters aligned to canonical millimeters or derive the value from an origin frame.", true);
    const ErrorCodeDescriptor OriginFrameLifecycleUnavailable =
        Describe("world_streaming.origin_frame.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The origin-frame owner is closed or has no staged replacement to publish.",
                 "Use the active owner lifecycle and stage a validated successor before publication.", false);
    const ErrorCodeDescriptor OriginFrameStorageUnavailable =
        Describe("world_streaming.origin_frame.storage_unavailable", ErrorSeverity::Error,
                 "Storage required for an origin-frame owner or replacement lease is unavailable.",
                 "Release retained frame leases or retry at a later owner safe point.", false);

    const ErrorCodeDescriptor DiagnosticProjectionInvalid =
        Describe("world_streaming.diagnostic_projection.invalid", ErrorSeverity::Error,
                 "A World Streaming diagnostic snapshot contains malformed or incoherent authority or decision facts.",
                 "Capture one complete authority-safe-point projection with valid owner, queue, budget and typed decision facts.", true);
    const ErrorCodeDescriptor DiagnosticProjectionStale =
        Describe("world_streaming.diagnostic_projection.stale", ErrorSeverity::Warning,
                 "A World Streaming diagnostic row no longer belongs to the captured authority or snapshot revision.",
                 "Discard the stale row and capture the current owner, partition epoch, fence and revisions together.", false);
    const ErrorCodeDescriptor DiagnosticProjectionUnsupported =
        Describe("world_streaming.diagnostic_projection.unsupported", ErrorSeverity::Error,
                 "A World Streaming diagnostic projection contains an unsupported typed state, decision, severity or reason.",
                 "Use only the lifecycle, residency, decision and terminal-reason values declared by this contract version.", true);
    const ErrorCodeDescriptor DiagnosticProjectionIdentityConflict =
        Describe("world_streaming.diagnostic_projection.identity_conflict", ErrorSeverity::Error,
                 "A World Streaming diagnostic projection repeats a stable source, cell, failure, event, sequence or context identity.",
                 "Publish unique canonical rows and context keys for each captured authority decision.", true);
    const ErrorCodeDescriptor DiagnosticProjectionCapacityExceeded =
        Describe("world_streaming.diagnostic_projection.capacity_exceeded", ErrorSeverity::Warning,
                 "A World Streaming diagnostic projection exceeds an admitted bounded row or queue capacity.",
                 "Reduce captured rows or configure a larger supported diagnostic ceiling before capture.", false);

    const ErrorCodeDescriptor CellStateInvalid =
        Describe("world_streaming.cell_state.invalid", ErrorSeverity::Error,
                 "A World Streaming cell-state ledger, owner, fence, or record is malformed.",
                 "Use the exact valid partition owner, operation handle and bounded ledger configuration.", true);
    const ErrorCodeDescriptor CellStateUnresolved =
        Describe("world_streaming.cell_state.unresolved", ErrorSeverity::Info,
                 "No residency record exists for the exact mounted cell attempt.",
                 "Resolve the manifest cell and admit a fenced load operation before querying runtime residency.", false);
    const ErrorCodeDescriptor CellStateStale =
        Describe("world_streaming.cell_state.stale", ErrorSeverity::Warning,
                 "A cell-state request names foreign ownership or a stale attempt generation.",
                 "Route the canonical operation to its exact partition authority and retained generation.", false);
    const ErrorCodeDescriptor CellStateUnsupported =
        Describe("world_streaming.cell_state.unsupported", ErrorSeverity::Error,
                 "A cell operation phase or outcome cannot be projected onto canonical residency.",
                 "Use a canonical admitted operation snapshot and keep provider barriers separate from residency.", true);
    const ErrorCodeDescriptor CellStateTransitionInvalid =
        Describe("world_streaming.cell_state.transition_invalid", ErrorSeverity::Error,
                 "A projected cell residency transition is illegal from the current state.",
                 "Follow the fenced Unloaded, Loading, Resident, Active, Evicting and Failed lifecycle.", false);
    const ErrorCodeDescriptor CellStateCapacityExceeded =
        Describe("world_streaming.cell_state.capacity_exceeded", ErrorSeverity::Warning,
                 "The cell-state ledger cannot retain another current or retiring attempt.",
                 "Finish exact retirement acknowledgement or explicitly admit a larger bounded ledger before mounting.", false);
    const ErrorCodeDescriptor CellStateLifecycleUnavailable =
        Describe("world_streaming.cell_state.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The cell-state ledger no longer accepts loading or publication.",
                 "During shutdown, reconcile only retirement and cleanup-complete terminal snapshots.", false);
    const ErrorCodeDescriptor RuntimeCompositionInvalid =
        Describe("world_streaming.runtime_composition.invalid", ErrorSeverity::Error, "A World Streaming runtime composition is malformed.",
                 "Provide one explicit planner, asset provider and Scene runtime plus bounded feature adapters and a valid scheduler.",
                 true);
    const ErrorCodeDescriptor RuntimeCompositionIdentityConflict =
        Describe("world_streaming.runtime_composition.identity_conflict", ErrorSeverity::Error,
                 "A World Streaming runtime composition repeats a service identity.",
                 "Assign every borrowed service binding one unique stable identity.", true);
    const ErrorCodeDescriptor RuntimeCompositionCapacityExceeded =
        Describe("world_streaming.runtime_composition.capacity_exceeded", ErrorSeverity::Error,
                 "World Streaming runtime service bindings exceed their configured ceiling.",
                 "Reduce feature adapters or choose an explicitly larger supported ceiling before composition.", true);
    const ErrorCodeDescriptor RuntimeCompositionRevisionStale =
        Describe("world_streaming.runtime_composition.revision_stale", ErrorSeverity::Warning,
                 "A World Streaming runtime composition command names stale ownership or revision facts.",
                 "Capture the active owner and revision before lookup, replacement, cancellation or shutdown.", false);
    const ErrorCodeDescriptor RuntimeCompositionLifecycleUnavailable =
        Describe("world_streaming.runtime_composition.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The World Streaming runtime composition lifecycle cannot accept this operation.",
                 "Finish retained scheduler work before replacement, or create a new composition after shutdown.", false);

    const ErrorCodeDescriptor FallbackProviderInvalid =
        Describe("world_streaming.fallback_provider.invalid", ErrorSeverity::Error,
                 "A fallback streaming provider descriptor is malformed.",
                 "Provide one valid owner/revision and exactly one cell for SingleCell or no cell for Null.", true);
    const ErrorCodeDescriptor FallbackProviderUnsupported =
        Describe("world_streaming.fallback_provider.unsupported", ErrorSeverity::Error,
                 "The fallback streaming provider mode is unsupported.", "Select the declared SingleCell or Null composition explicitly.",
                 true);
    const ErrorCodeDescriptor FallbackProviderCapacityExceeded =
        Describe("world_streaming.fallback_provider.capacity_exceeded", ErrorSeverity::Error,
                 "The configured fallback provider cannot publish its single cell within the mandatory ceiling.",
                 "Allow one published cell or select the explicit Null composition.", true);
    const ErrorCodeDescriptor FallbackProviderStale =
        Describe("world_streaming.fallback_provider.stale", ErrorSeverity::Warning,
                 "A fallback provider operation names a stale owner or configuration revision.",
                 "Capture the current owner lifetime and issue a strictly newer replacement revision.", false);
    const ErrorCodeDescriptor FallbackProviderLifecycleUnavailable =
        Describe("world_streaming.fallback_provider.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The fallback provider lifecycle no longer accepts this operation.",
                 "Finish shutdown or create a new provider for the next owner lifetime.", false);

    const ErrorCodeDescriptor IdentityInvalid =
        Describe("world_streaming.identity.invalid", ErrorSeverity::Error,
                 "A world-partition identity uses its reserved invalid representation.",
                 "Use an identity issued by the manifest, authoring boundary, or active partition authority.", true);
    const ErrorCodeDescriptor SerializedIdentityInvalid =
        Describe("world_streaming.identity.serialized_invalid", ErrorSeverity::Error,
                 "A serialized world-partition identity is malformed or reserved.",
                 "Rebuild the world index or source descriptor with canonical little-endian identity bytes.", true);
    const ErrorCodeDescriptor GenerationExhausted =
        Describe("world_streaming.generation.exhausted", ErrorSeverity::Critical,
                 "A world-partition epoch, cell generation, runtime-source revision, or authoring-page revision cannot advance.",
                 "Retire the exhausted incarnation, slot, source, or page; never wrap an issued world-streaming counter.", false);
    const ErrorCodeDescriptor CellOperationInvalid =
        Describe("world_streaming.cell_operation.invalid", ErrorSeverity::Error,
                 "A cell operation has a malformed identity, fence, or initial representation.",
                 "Use a non-zero operation identity and the exact valid fence issued for the cell attempt.", true);
    const ErrorCodeDescriptor CellOperationStale =
        Describe("world_streaming.cell_operation.stale", ErrorSeverity::Warning,
                 "A command or completion does not name the exact cell operation and fence.",
                 "Discard stale publication while routing retirement acknowledgement to its matching retained operation.", false);
    const ErrorCodeDescriptor CellOperationUnsupported =
        Describe("world_streaming.cell_operation.unsupported", ErrorSeverity::Error,
                 "A cell operation transition value is unsupported by this contract version.",
                 "Use one of the declared typed cell-operation transitions.", true);
    const ErrorCodeDescriptor CellOperationTransitionInvalid =
        Describe("world_streaming.cell_operation.transition_invalid", ErrorSeverity::Error,
                 "A known cell operation transition is not legal from the current phase.",
                 "Follow the queued, admitted, preparing, activating, retiring, and terminal lifecycle order.", false);
    const ErrorCodeDescriptor SchedulerAdmissionInvalid =
        Describe("world_streaming.scheduler.admission_invalid", ErrorSeverity::Error,
                 "A scheduler admission ledger, request, or reservation is malformed.",
                 "Use a valid ledger owner, positive bounded limits, and a valid queued operation with a positive capacity charge.", true);
    const ErrorCodeDescriptor SchedulerCapacityExceeded =
        Describe("world_streaming.scheduler.capacity_exceeded", ErrorSeverity::Warning,
                 "The scheduler cannot reserve the requested operation count or generic capacity.",
                 "Wait for an admitted operation to reach acknowledged terminal retirement before retrying.", false);
    const ErrorCodeDescriptor SchedulerReservationConflict =
        Describe("world_streaming.scheduler.reservation_conflict", ErrorSeverity::Error,
                 "The cell operation already owns a reservation in this scheduler ledger.",
                 "Reuse the retained reservation instead of admitting the exact operation twice.", false);
    const ErrorCodeDescriptor SchedulerReservationStale =
        Describe("world_streaming.scheduler.reservation_stale", ErrorSeverity::Warning,
                 "A scheduler command does not name the exact owner-scoped reservation and operation fence.",
                 "Route the command to the owning ledger with its exact reservation token.", false);
    const ErrorCodeDescriptor SchedulerLifecycleUnavailable =
        Describe("world_streaming.scheduler.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Scheduler admission is draining, closed, or waiting for an operation to retire.",
                 "Do not admit during shutdown and retain capacity until exact canonical operations become terminal.", false);
    const ErrorCodeDescriptor BudgetModelInvalid =
        Describe("world_streaming.budget.model_invalid", ErrorSeverity::Error,
                 "A multidimensional budget vector, policy, request, or evaluation context is malformed.",
                 "Provide every supported dimension exactly once with valid limits, revisions, timing, and positive requested work.", true);
    const ErrorCodeDescriptor BudgetDimensionUnsupported =
        Describe("world_streaming.budget.dimension_unsupported", ErrorSeverity::Error,
                 "A budget vector or policy names an unsupported resource dimension.",
                 "Use exactly the typed dimensions declared by this world-streaming contract version.", true);
    const ErrorCodeDescriptor BudgetRevisionStale =
        Describe("world_streaming.budget.revision_stale", ErrorSeverity::Warning,
                 "A budget policy or usage sample revision no longer matches current authority state.",
                 "Capture the current immutable policy and usage sample before retrying evaluation.", false);
    const ErrorCodeDescriptor BudgetSampleInvalid =
        Describe("world_streaming.budget.sample_invalid", ErrorSeverity::Error,
                 "A budget usage sample has malformed monotonic window timing.",
                 "Use a non-negative window start and an observation inside the policy's positive half-open sampling window.", true);
    const ErrorCodeDescriptor BudgetSampleStale =
        Describe("world_streaming.budget.sample_stale", ErrorSeverity::Warning,
                 "A budget usage sample belongs to an earlier completed sampling window.",
                 "Capture a current deterministic usage sample before evaluating new work.", false);
    const ErrorCodeDescriptor BudgetCapacityExceeded =
        Describe("world_streaming.budget.capacity_exceeded", ErrorSeverity::Warning,
                 "Projected usage overflows or exceeds an independent hard resource limit.",
                 "Defer work, retire charged resources, or select a validated policy that can contain the request.", false);
    const ErrorCodeDescriptor QuantizationPolicyInvalid =
        Describe("world_streaming.quantization.policy_invalid", ErrorSeverity::Error,
                 "The world-cell quantization policy has invalid size, bounds, or LOD limits.",
                 "Use a positive millimeter cell size, ordered int32 grid bounds, and at least one supported LOD.", true);
    const ErrorCodeDescriptor CoordinateOutOfRange =
        Describe("world_streaming.quantization.coordinate_out_of_range", ErrorSeverity::Error,
                 "The world coordinate cannot be translated into the grid's signed 64-bit relative frame.",
                 "Use a grid origin and world coordinate whose exact millimeter difference is representable.", true);
    const ErrorCodeDescriptor LodUnsupported = Describe("world_streaming.quantization.lod_unsupported", ErrorSeverity::Error,
                                                        "The requested world-cell LOD is not declared by the grid policy.",
                                                        "Request a LOD below the validated manifest lodLevels value.", true);
    const ErrorCodeDescriptor CellOutOfBounds =
        Describe("world_streaming.quantization.cell_out_of_bounds", ErrorSeverity::Error,
                 "The quantized world cell is outside the manifest's inclusive grid bounds.",
                 "Reject the spatial request or use a validated partition whose bounds contain the coordinate.", false);
    const ErrorCodeDescriptor PartitionDescriptorInvalid =
        Describe("world_streaming.partition.descriptor_invalid", ErrorSeverity::Error,
                 "A world-partition descriptor is incomplete or contains malformed fields.",
                 "Provide a valid partition identity, layers, cells, package references and versioned field values.", true);
    const ErrorCodeDescriptor PartitionVersionUnsupported =
        Describe("world_streaming.partition.version_unsupported", ErrorSeverity::Error,
                 "The world-partition descriptor schema version is unsupported.",
                 "Migrate the world index to the exact schema version supported by this runtime.", true);
    const ErrorCodeDescriptor PartitionBoundsInvalid =
        Describe("world_streaming.partition.bounds_invalid", ErrorSeverity::Error,
                 "World content bounds are unordered or outside the representable grid envelope.",
                 "Use ordered exact bounds fully contained by the validated level-zero partition grid.", true);
    const ErrorCodeDescriptor PartitionCapacityExceeded =
        Describe("world_streaming.partition.capacity_exceeded", ErrorSeverity::Error,
                 "The world-partition descriptor exceeds a mandatory host storage limit.",
                 "Reduce manifest layer, cell, or layer-name data, or choose an explicitly larger supported limit.", true);
    const ErrorCodeDescriptor PartitionIdentityConflict =
        Describe("world_streaming.partition.identity_conflict", ErrorSeverity::Error,
                 "The world-partition descriptor repeats a layer or exact cell identity.",
                 "Remove duplicate identities and regenerate the canonical world index.", true);
    const ErrorCodeDescriptor PartitionRegistryInvalid =
        Describe("world_streaming.partition_registry.invalid", ErrorSeverity::Error,
                 "A partition-registry identity, binding, query, handle, or limit is malformed.",
                 "Provide valid typed registry and owner identities, ordered bounds, and positive bounded limits.", true);
    const ErrorCodeDescriptor PartitionRegistryUnavailable =
        Describe("world_streaming.partition_registry.unavailable", ErrorSeverity::Info,
                 "No current partition-registry publication or requested manifest cell is available.",
                 "Publish a complete partition descriptor or query a cell declared by the pinned snapshot.", false);
    const ErrorCodeDescriptor PartitionRegistryStale =
        Describe("world_streaming.partition_registry.stale", ErrorSeverity::Warning,
                 "A partition-registry revision or cell handle names an obsolete immutable publication.",
                 "Capture the current snapshot and repeat lookup against its exact registry revision and partition epoch.", false);
    const ErrorCodeDescriptor PartitionRegistryCapacityExceeded =
        Describe("world_streaming.partition_registry.capacity_exceeded", ErrorSeverity::Error,
                 "A partition publication or spatial query exceeds its mandatory bounded ceiling.",
                 "Reduce indexed cells or query scope, or select explicitly larger supported limits before publication.", true);
    const ErrorCodeDescriptor PartitionRegistryUnsupported =
        Describe("world_streaming.partition_registry.unsupported", ErrorSeverity::Error,
                 "A partition query filter or indexed cell extent is unsupported by this registry contract.",
                 "Use a declared layer and LOD whose canonical millimeter cell extent is representable.", true);
    const ErrorCodeDescriptor PartitionRegistryLifecycleUnavailable =
        Describe("world_streaming.partition_registry.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Partition-registry publication and new snapshot capture are cancelling or closed.",
                 "Retain an already issued immutable snapshot or create a registry for the next mounted owner lifetime.", false);
    const ErrorCodeDescriptor PartitionRegistryStorageUnavailable =
        Describe("world_streaming.partition_registry.storage_unavailable", ErrorSeverity::Error,
                 "Storage required for an immutable partition-registry publication is unavailable.",
                 "Release retained snapshots or retry publication at a later owner safe point.", false);
    const ErrorCodeDescriptor CookedManifestInvalid =
        Describe("world_streaming.cooked_manifest.invalid", ErrorSeverity::Error,
                 "A cooked world-index manifest is incomplete or contains malformed cell metadata.",
                 "Provide one non-empty cooked record for every validated partition cell.", true);
    const ErrorCodeDescriptor CookedManifestCapacityExceeded =
        Describe("world_streaming.cooked_manifest.capacity_exceeded", ErrorSeverity::Error,
                 "A cooked world-index manifest exceeds a mandatory count or byte ceiling.",
                 "Reduce cell metadata or choose an explicitly larger supported manifest limit.", true);
    const ErrorCodeDescriptor CookedManifestIdentityConflict =
        Describe("world_streaming.cooked_manifest.identity_conflict", ErrorSeverity::Error,
                 "Cooked cell metadata does not map one-to-one to the authoritative partition cells.",
                 "Regenerate the cooked index with exactly one record for each descriptor cell.", true);
    const ErrorCodeDescriptor CookedManifestDependencyInvalid =
        Describe("world_streaming.cooked_manifest.dependency_invalid", ErrorSeverity::Error,
                 "A cooked cell dependency is invalid, duplicated, self-referential, or absent from the manifest.",
                 "Emit unique required cell identities that belong to the same cooked partition.", true);
    const ErrorCodeDescriptor CellCandidateInvalid =
        Describe("world_streaming.cell_candidate.invalid", ErrorSeverity::Error,
                 "A cell candidate context, fixed header, or payload table is malformed.",
                 "Reparse the complete canonical cell header and submit valid bounded load-operation evidence.", true);
    const ErrorCodeDescriptor CellCandidateUnsupported =
        Describe("world_streaming.cell_candidate.unsupported", ErrorSeverity::Error,
                 "A cell candidate requests an unsupported format, provider contract, or operation phase.",
                 "Recook the cell for this runtime format or use an explicitly supported optional provider payload.", true);
    const ErrorCodeDescriptor CellCandidateStale =
        Describe("world_streaming.cell_candidate.stale", ErrorSeverity::Warning,
                 "A cell candidate does not match its exact manifest record, operation, partition, or generation.",
                 "Discard the candidate and prepare again from the current manifest and load-operation fence.", false);
    const ErrorCodeDescriptor CellCandidateCapacityExceeded =
        Describe("world_streaming.cell_candidate.capacity_exceeded", ErrorSeverity::Error,
                 "Cell candidate header or payload storage exceeds a mandatory caller ceiling.",
                 "Reject the artifact or admit it under explicitly larger streaming reservations.", true);
    const ErrorCodeDescriptor CellCandidateUnavailable =
        Describe("world_streaming.cell_candidate.unavailable", ErrorSeverity::Error,
                 "The requested cell is absent from the immutable cooked world index.",
                 "Resolve a cell declared by the pinned manifest publication before starting I/O.", false);
    const ErrorCodeDescriptor CellCandidateLifecycleUnavailable =
        Describe("world_streaming.cell_candidate.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Candidate preparation is closed by cancellation or shutdown.",
                 "Retire the submitted operation or prepare under a new active owner lifetime.", false);
    const ErrorCodeDescriptor CellActivationInvalid =
        Describe("world_streaming.cell_activation.invalid", ErrorSeverity::Error,
                 "A cell activation identity, operation, requirement, or prepared receipt is malformed.",
                 "Submit one valid Activating operation and a canonical complete receipt set.", true);
    const ErrorCodeDescriptor CellActivationStale =
        Describe("world_streaming.cell_activation.stale", ErrorSeverity::Warning,
                 "A prepared receipt or commit command names another operation, generation, or service revision.",
                 "Roll back the stale receipts and prepare again for the current cell attempt.", false);
    const ErrorCodeDescriptor CellActivationIncomplete =
        Describe("world_streaming.cell_activation.incomplete", ErrorSeverity::Error,
                 "The required Scene and provider receipt set is incomplete or mismatched.",
                 "Acquire exactly one matching prepared receipt for every required participant.", true);
    const ErrorCodeDescriptor CellActivationCapacityExceeded =
        Describe("world_streaming.cell_activation.capacity_exceeded", ErrorSeverity::Error,
                 "A required activation receipt set exceeds its mandatory admission ceiling.",
                 "Reduce required providers or explicitly admit a larger supported receipt ceiling.", true);
    const ErrorCodeDescriptor CellActivationLifecycleUnavailable =
        Describe("world_streaming.cell_activation.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Cell activation is cancelled, closed, already terminal, or unavailable during shutdown.",
                 "Retire the prepared attempt or create a fresh activation under an active owner lifetime.", false);
    const ErrorCodeDescriptor CellActivationSafePointUnavailable =
        Describe("world_streaming.cell_activation.safe_point_unavailable", ErrorSeverity::Warning,
                 "Prepared cell publication was requested outside CommitDeferredLifecycleChanges.",
                 "Defer the complete transaction to the Scene structural commit phase.", false);
    const ErrorCodeDescriptor SpatialAssignmentInvalid =
        Describe("world_streaming.spatial_assignment.invalid", ErrorSeverity::Error,
                 "A spatial-assignment request is empty, malformed, or outside the partition content bounds.",
                 "Provide valid page, object, revision, layer and LOD data with ordered canonical bounds.", true);
    const ErrorCodeDescriptor SpatialAssignmentIdentityConflict =
        Describe("world_streaming.spatial_assignment.identity_conflict", ErrorSeverity::Error,
                 "A spatial-assignment request repeats one stable authored-object address.",
                 "Submit exactly one immutable revision for each page-scoped authored object.", true);
    const ErrorCodeDescriptor SpatialAssignmentCapacityExceeded =
        Describe("world_streaming.spatial_assignment.capacity_exceeded", ErrorSeverity::Error,
                 "Spatial assignment exceeds a mandatory object or cell-count ceiling.",
                 "Reduce authored-object coverage or choose explicitly larger supported cook limits.", true);
    const ErrorCodeDescriptor SpatialAssignmentUnsupported =
        Describe("world_streaming.spatial_assignment.unsupported", ErrorSeverity::Error,
                 "A spatial-assignment request names a layer absent from the partition descriptor.",
                 "Assign the object to a layer declared by the authoritative partition descriptor.", true);
    const ErrorCodeDescriptor SpatialAssignmentCellUnavailable =
        Describe("world_streaming.spatial_assignment.cell_unavailable", ErrorSeverity::Error,
                 "A quantized authored object intersects a cell absent from the partition descriptor.",
                 "Declare every intersected cell or apply an explicit spanning-object cook policy.", true);
    const ErrorCodeDescriptor SpanningObjectPlanInvalid =
        Describe("world_streaming.spanning_object.invalid", ErrorSeverity::Error,
                 "A spanning-object plan or directive is malformed, missing, or applied to a direct object.",
                 "Provide one valid explicit directive only for each object exceeding the direct-cell threshold.", true);
    const ErrorCodeDescriptor SpanningObjectPlanIdentityConflict =
        Describe("world_streaming.spanning_object.identity_conflict", ErrorSeverity::Error,
                 "A spanning-object directive is duplicated or names an object absent from spatial assignment.",
                 "Provide at most one directive for each exact object admitted by the spatial-assignment result.", true);
    const ErrorCodeDescriptor SpanningObjectPlanRevisionStale =
        Describe("world_streaming.spanning_object.revision_stale", ErrorSeverity::Warning,
                 "A spanning-object directive does not match the admitted immutable object revision.",
                 "Rebuild policy directives from the exact spatial-assignment snapshot being cooked.", false);
    const ErrorCodeDescriptor SpanningObjectPlanUnsupported =
        Describe("world_streaming.spanning_object.unsupported", ErrorSeverity::Error,
                 "A spanning-object directive uses an unsupported cook policy.",
                 "Use explicit single-cell ownership, per-cell splitting, or non-spatial placement.", true);
    const ErrorCodeDescriptor SpanningObjectPlanCapacityExceeded =
        Describe("world_streaming.spanning_object.capacity_exceeded", ErrorSeverity::Error,
                 "A spanning-object plan exceeds a mandatory object or placement-cell ceiling.",
                 "Reduce object coverage or choose explicitly larger supported cook limits.", true);
    const ErrorCodeDescriptor SpanningObjectPlanOwnerUnavailable =
        Describe("world_streaming.spanning_object.owner_unavailable", ErrorSeverity::Error,
                 "The explicit single-cell owner is not covered by the source spatial assignment.",
                 "Choose one canonical cell from the exact object's spatial-assignment coverage.", true);
    const ErrorCodeDescriptor DependencyPlanInvalid =
        Describe("world_streaming.dependency_plan.invalid", ErrorSeverity::Error,
                 "A dependency-plan request contains malformed, duplicated, self-referential, or missing source data.",
                 "Provide unique directed edges from admitted authored objects with valid exact endpoints.", true);
    const ErrorCodeDescriptor DependencyPlanRevisionStale =
        Describe("world_streaming.dependency_plan.revision_stale", ErrorSeverity::Warning,
                 "A dependency endpoint revision does not match the admitted spatial-assignment revision.",
                 "Rebuild the authored graph from the same immutable object revisions used for spatial assignment.", false);
    const ErrorCodeDescriptor DependencyPlanCapacityExceeded =
        Describe("world_streaming.dependency_plan.capacity_exceeded", ErrorSeverity::Error,
                 "A dependency plan exceeds a mandatory edge, bundle, or reference ceiling.",
                 "Reduce graph density or choose explicitly larger supported cook limits.", true);
    const ErrorCodeDescriptor DependencyPlanHardTargetMissing =
        Describe("world_streaming.dependency_plan.hard_target_missing", ErrorSeverity::Error,
                 "A hard dependency target is absent from the admitted spatial assignments.",
                 "Include the exact target revision in spatial assignment or change the authored edge to a soft reference.", true);
    const ErrorCodeDescriptor DependencyPlanAmbiguous =
        Describe("world_streaming.dependency_plan.ambiguous", ErrorSeverity::Error,
                 "A deferred reference connects objects already joined by the transitive hard co-load policy.",
                 "Remove the soft edge or replace the conflicting hard path with one unambiguous dependency policy.", true);
    const ErrorCodeDescriptor EntityFixupInvalid =
        Describe("world_streaming.entity_fixup.invalid", ErrorSeverity::Error,
                 "A stable entity-reference fixup request, mapping, result, or limit is malformed.",
                 "Provide valid stable endpoints, revisions, runtime tokens, and bounded activation evidence.", true);
    const ErrorCodeDescriptor EntityFixupUnsupported =
        Describe("world_streaming.entity_fixup.unsupported", ErrorSeverity::Error,
                 "A stable entity-reference fixup policy or result value is unsupported.",
                 "Use the declared hard or soft reference policy and typed fixup lifecycle values.", true);
    const ErrorCodeDescriptor EntityFixupStale =
        Describe("world_streaming.entity_fixup.stale", ErrorSeverity::Warning,
                 "A stable entity-reference fixup names an obsolete owner, revision, or activation mapping.",
                 "Capture the current owner and exact endpoint revisions before retrying the fixup.", false);
    const ErrorCodeDescriptor EntityFixupIdentityConflict =
        Describe("world_streaming.entity_fixup.identity_conflict", ErrorSeverity::Error,
                 "An activation batch repeats a stable entity endpoint or runtime mapping identity.",
                 "Publish one exact runtime mapping for each stable endpoint and runtime token.", true);
    const ErrorCodeDescriptor EntityFixupCapacityExceeded =
        Describe("world_streaming.entity_fixup.capacity_exceeded", ErrorSeverity::Warning,
                 "The bounded entity-reference fixup owner cannot retain another soft reference or activation mapping.",
                 "Drain pending fixups or admit an explicitly larger bounded capacity before activation.", false);
    const ErrorCodeDescriptor EntityFixupLifecycleUnavailable =
        Describe("world_streaming.entity_fixup.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Entity-reference fixup admission or activation is closed by cancellation or shutdown.",
                 "Drain retained references through cancellation/failure or create a ledger for the next owner lifetime.", false);
    const ErrorCodeDescriptor EntityFixupSourceUnavailable =
        Describe("world_streaming.entity_fixup.source_unavailable", ErrorSeverity::Warning,
                 "The source entity is not present in the exact activation mapping.",
                 "Activate the source under the matching endpoint revision before submitting its reference fixups.", false);
    const ErrorCodeDescriptor EntityFixupTargetUnavailable =
        Describe("world_streaming.entity_fixup.target_unavailable", ErrorSeverity::Warning,
                 "A hard entity-reference target is not present in the exact activation mapping.",
                 "Co-load and activate the target for a hard reference, or author the relationship as a soft reference.", false);
    const ErrorCodeDescriptor SourceDescriptorInvalid =
        Describe("world_streaming.source.descriptor_invalid", ErrorSeverity::Error,
                 "A streaming source descriptor or admission context is structurally invalid.",
                 "Provide valid source, owner and revision identities with a finite non-negative priority.", true);
    const ErrorCodeDescriptor SourceIntentUnsupported =
        Describe("world_streaming.source.intent_unsupported", ErrorSeverity::Error,
                 "The streaming source intent is not supported by this contract version.",
                 "Use a declared camera, gameplay, network-relevance or preload intent.", true);
    const ErrorCodeDescriptor SourceOwnerStale =
        Describe("world_streaming.source.owner_stale", ErrorSeverity::Warning,
                 "The streaming source owner token no longer names the active owner lifetime.",
                 "Discard the stale request and resolve the current partition owner token before retrying.", false);
    const ErrorCodeDescriptor SourceRevisionStale =
        Describe("world_streaming.source.revision_stale", ErrorSeverity::Warning,
                 "The streaming source update does not advance the admitted revision.",
                 "Issue a strictly newer non-wrapping revision for the same stable source identity.", false);
    const ErrorCodeDescriptor SourceCapacityExceeded =
        Describe("world_streaming.source.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded streaming source capacity cannot admit another identity.",
                 "Release an existing source or increase the host-configured source capacity.", false);
    const ErrorCodeDescriptor SourceLifecycleUnavailable =
        Describe("world_streaming.source.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The streaming source owner is cancelling or closed to new admission.",
                 "Finish owner retirement or submit the source to a new active owner lifetime.", false);
    const ErrorCodeDescriptor SourceShapeInvalid =
        Describe("world_streaming.source.shape_invalid", ErrorSeverity::Error,
                 "A streaming source shape is malformed or exceeds the bounded evaluation contract.",
                 "Use ordered finite geometry within the declared source range limits.", true);
    const ErrorCodeDescriptor SourceShapeUnsupported =
        Describe("world_streaming.source.shape_unsupported", ErrorSeverity::Error,
                 "The evaluating host does not support the requested streaming source shape.",
                 "Enable the shape capability or submit a supported bounded source shape.", true);
    const ErrorCodeDescriptor PrefetchInvalid =
        Describe("world_streaming.prefetch.invalid", ErrorSeverity::Error,
                 "A velocity-prefetch policy, context, or canonical kinematic sample is malformed.",
                 "Provide valid policy and partition fences, bounded exact velocity, and coherent source admission evidence.", true);
    const ErrorCodeDescriptor PrefetchUnsupported =
        Describe("world_streaming.prefetch.unsupported", ErrorSeverity::Error,
                 "A velocity-prefetch contract version or source category is unsupported.",
                 "Use the current contract with a camera or gameplay source and an explicitly supported bounded path.", true);
    const ErrorCodeDescriptor PrefetchStale =
        Describe("world_streaming.prefetch.stale", ErrorSeverity::Warning,
                 "Velocity-prefetch evidence names a replaced policy, partition, owner, or expired kinematic sample.",
                 "Capture the current policy, mounted partition and source owner before evaluating a recent sample.", true);
    const ErrorCodeDescriptor PrefetchLifecycleUnavailable =
        Describe("world_streaming.prefetch.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Velocity-prefetch evaluation is unavailable during cancellation or after shutdown.",
                 "Stop emitting predicted demand and let the source owner retire its admitted revision.", false);
    const ErrorCodeDescriptor SourceDesiredStateInvalid =
        Describe("world_streaming.source.desired_state_invalid", ErrorSeverity::Error,
                 "A streaming source desired state combines residency and retention inconsistently.",
                 "Use releasable retention for Unloaded, or request Loaded or Activated before pinning.", true);
    const ErrorCodeDescriptor SourceDesiredStateUnsupported =
        Describe("world_streaming.source.desired_state_unsupported", ErrorSeverity::Error,
                 "A streaming source desired-state value is not supported by this contract version.",
                 "Use Unloaded, Loaded or Activated residency with Releasable or Pinned retention.", true);
    const ErrorCodeDescriptor SourceReductionInvalid =
        Describe("world_streaming.source_reduction.invalid", ErrorSeverity::Error,
                 "A desired-state reduction context or mandatory limit is malformed.",
                 "Provide a valid partition incarnation and cell with a positive contributor ceiling.", true);
    const ErrorCodeDescriptor SourceReductionIdentityConflict =
        Describe("world_streaming.source_reduction.identity_conflict", ErrorSeverity::Error,
                 "A desired-state reduction repeats one stable source identity.",
                 "Present exactly one admitted immutable revision for each contributing source.", true);
    const ErrorCodeDescriptor SourceReductionCapacityExceeded =
        Describe("world_streaming.source_reduction.capacity_exceeded", ErrorSeverity::Error,
                 "A desired-state reduction exceeds its bounded contributor ceiling.",
                 "Reduce overlapping source demand or increase the host-configured per-cell contributor limit.", false);
    const ErrorCodeDescriptor PriorityPolicyInvalid =
        Describe("world_streaming.priority_policy.invalid", ErrorSeverity::Error,
                 "A streaming priority policy, evaluation context, or candidate is malformed.",
                 "Provide finite bounded policy factors, valid identities, canonical cells, and an override in [0.5, 2.0].", true);
    const ErrorCodeDescriptor PriorityPolicyUnsupported =
        Describe("world_streaming.priority_policy.unsupported", ErrorSeverity::Error,
                 "A streaming priority policy contract version is unsupported.",
                 "Migrate the project policy to the current typed priority contract version.", true);
    const ErrorCodeDescriptor PriorityPolicyCapacityExceeded =
        Describe("world_streaming.priority_policy.capacity_exceeded", ErrorSeverity::Error,
                 "A streaming priority ranking request exceeds its immutable row or output ceiling.",
                 "Reduce the candidate snapshot or provide storage up to the configured bounded ceiling.", false);
    const ErrorCodeDescriptor PriorityPolicyStale =
        Describe("world_streaming.priority_policy.stale", ErrorSeverity::Warning,
                 "A ranking pass references a replaced streaming priority policy publication.",
                 "Capture the current policy identity and revision before evaluating the next immutable candidate snapshot.", true);
    const ErrorCodeDescriptor PriorityPolicyLifecycleUnavailable =
        Describe("world_streaming.priority_policy.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Streaming priority ranking is unavailable during cancellation or after shutdown.",
                 "Stop producing ranking snapshots and retain already admitted work until canonical retirement completes.", true);
    const ErrorCodeDescriptor CellStabilityInvalid =
        Describe("world_streaming.cell_stability.invalid", ErrorSeverity::Error,
                 "A cell-stability policy, context, observation, or retained state is malformed.",
                 "Provide valid policy and partition fences, monotonic service time, bounded margins, and coherent desired residency.",
                 true);
    const ErrorCodeDescriptor CellStabilityUnsupported =
        Describe("world_streaming.cell_stability.unsupported", ErrorSeverity::Error,
                 "A cell-stability contract version or closed enum value is unsupported.",
                 "Migrate the policy or producer to the current typed cell-stability contract.", true);
    const ErrorCodeDescriptor CellStabilityCapacityExceeded =
        Describe("world_streaming.cell_stability.capacity_exceeded", ErrorSeverity::Error,
                 "A new anti-thrash record exceeds the authority's immutable tracked-cell ceiling.",
                 "Retire an expired record or configure a supported larger ceiling before admitting new demand.", false);
    const ErrorCodeDescriptor CellStabilityStale =
        Describe("world_streaming.cell_stability.stale", ErrorSeverity::Warning,
                 "Cell-stability evidence names a replaced policy or partition publication, or moves service time backward.",
                 "Evaluate against the current policy and partition generation using monotonic service time.", true);
    const ErrorCodeDescriptor CellStabilityLifecycleUnavailable =
        Describe("world_streaming.cell_stability.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Cell-stability evaluation is unavailable during cancellation or after shutdown.",
                 "Stop admitting stability transitions and let the streaming authority retire its retained records.", true);
    const ErrorCodeDescriptor AuthoringContractInvalid =
        Describe("world_streaming.authoring.contract_invalid", ErrorSeverity::Error,
                 "A world-authoring contract, page request, or authority snapshot is malformed.",
                 "Provide valid partition, page asset and revision identities with consistent bounded owner state.", true);
    const ErrorCodeDescriptor AuthoringVersionUnsupported =
        Describe("world_streaming.authoring.version_unsupported", ErrorSeverity::Error,
                 "The world-authoring contract schema version is unsupported.",
                 "Migrate the authoring contract to the exact version supported by this editor.", true);
    const ErrorCodeDescriptor AuthoringPolicyUnsupported =
        Describe("world_streaming.authoring.policy_unsupported", ErrorSeverity::Error,
                 "The requested authoring granularity or collaboration authority is unsupported.",
                 "Use spatial authoring pages with revision-checked publication.", true);
    const ErrorCodeDescriptor AuthoringIdentityConflict =
        Describe("world_streaming.authoring.identity_conflict", ErrorSeverity::Error,
                 "The authoring request does not match the active page identity or partition.",
                 "Resolve the exact partition and stable page asset before submitting the request again.", false);
    const ErrorCodeDescriptor AuthoringRevisionStale =
        Describe("world_streaming.authoring.revision_stale", ErrorSeverity::Warning,
                 "The authoring request does not replace the expected immutable page revision.",
                 "Reload the current page head and publish its exact non-wrapping successor revision.", false);
    const ErrorCodeDescriptor AuthoringCapacityExceeded =
        Describe("world_streaming.authoring.capacity_exceeded", ErrorSeverity::Error,
                 "The authoring owner cannot admit another page within its configured capacity.",
                 "Close an existing authoring page or select a larger supported owner capacity.", false);
    const ErrorCodeDescriptor AuthoringLifecycleUnavailable =
        Describe("world_streaming.authoring.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The authoring owner is cancelling or closed to page admission.",
                 "Finish retirement or submit the page to a new active authoring owner.", false);
    const ErrorCodeDescriptor SpatialObjectDescriptorInvalid =
        Describe("world_streaming.spatial_object.descriptor_invalid", ErrorSeverity::Error,
                 "A spatial-object descriptor, request, or owner snapshot is malformed.",
                 "Provide valid stable identities, ordered canonical bounds, revisions and a positive owner capacity.", true);
    const ErrorCodeDescriptor SpatialObjectVersionUnsupported =
        Describe("world_streaming.spatial_object.version_unsupported", ErrorSeverity::Error,
                 "The spatial-object descriptor schema version is unsupported.",
                 "Migrate the descriptor to the exact version supported by this world-streaming build.", true);
    const ErrorCodeDescriptor SpatialObjectPlacementUnsupported =
        Describe("world_streaming.spatial_object.placement_unsupported", ErrorSeverity::Error,
                 "The authored spatial-object placement class is unsupported.",
                 "Use a spatial or always-present authored placement class supported by schema version one.", true);
    const ErrorCodeDescriptor SpatialObjectIdentityConflict =
        Describe("world_streaming.spatial_object.identity_conflict", ErrorSeverity::Error,
                 "A replacement does not name the currently admitted authored-object identity.",
                 "Resolve the exact page-scoped object address before retrying the replacement.", false);
    const ErrorCodeDescriptor SpatialObjectRevisionStale =
        Describe("world_streaming.spatial_object.revision_stale", ErrorSeverity::Warning,
                 "A spatial-object replacement is missing the current revision or is not its exact successor.",
                 "Reload the current descriptor and submit its exact non-wrapping successor revision.", false);
    const ErrorCodeDescriptor SpatialObjectCapacityExceeded =
        Describe("world_streaming.spatial_object.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded spatial-object registry cannot admit another identity.",
                 "Retire an existing descriptor or increase the host-configured descriptor capacity.", false);
    const ErrorCodeDescriptor SpatialObjectLifecycleUnavailable =
        Describe("world_streaming.spatial_object.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The spatial-object owner is cancelling or closed to new admission.",
                 "Finish owner retirement or submit the descriptor to a new active owner.", false);
    const ErrorCodeDescriptor ObjectOwnershipInvalid =
        Describe("world_streaming.object_ownership.invalid", ErrorSeverity::Error,
                 "An object-ownership descriptor, request, or owner snapshot is malformed.",
                 "Provide one valid object identity, an exact owner binding, a revision, and positive bounded capacity.", true);
    const ErrorCodeDescriptor ObjectOwnershipUnsupported =
        Describe("world_streaming.object_ownership.unsupported", ErrorSeverity::Error,
                 "An object class, owner kind, or cell-exit policy is unsupported or contradictory.",
                 "Use world ownership for authored always-present content, cell ownership for authored spatial content, and an explicit "
                 "runtime-spawned policy.",
                 true);
    const ErrorCodeDescriptor ObjectOwnershipIdentityConflict =
        Describe("world_streaming.object_ownership.identity_conflict", ErrorSeverity::Error,
                 "An ownership replacement names a different authored or runtime-spawned object.",
                 "Resolve the exact current object identity before publishing its ownership successor.", false);
    const ErrorCodeDescriptor ObjectOwnershipRevisionStale =
        Describe("world_streaming.object_ownership.revision_stale", ErrorSeverity::Warning,
                 "An ownership publication is missing the current revision or is not its exact successor.",
                 "Reload the current ownership fact and submit its next non-wrapping revision.", false);
    const ErrorCodeDescriptor ObjectOwnershipOwnerStale =
        Describe("world_streaming.object_ownership.owner_stale", ErrorSeverity::Warning,
                 "An ownership fact does not belong to the active mounted-world owner lifetime.",
                 "Discard the stale fact and rebuild it from the current partition epoch and runtime owner token.", false);
    const ErrorCodeDescriptor ObjectOwnershipCapacityExceeded =
        Describe("world_streaming.object_ownership.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded ownership authority cannot admit another object identity.",
                 "Retire an existing ownership record or increase the host-configured capacity.", false);
    const ErrorCodeDescriptor ObjectOwnershipLifecycleUnavailable =
        Describe("world_streaming.object_ownership.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The ownership authority is cancelling or closed to publication.",
                 "Finish retirement or publish to a new active mounted-world authority.", false);
    const ErrorCodeDescriptor LayerOwnershipInvalid =
        Describe("world_streaming.layer_ownership.invalid", ErrorSeverity::Error,
                 "A layer-ownership descriptor, request, or owner snapshot is malformed.",
                 "Provide a stable layer identity, revision, coherent classification, exact owner, and positive bounded capacity.", true);
    const ErrorCodeDescriptor LayerOwnershipUnsupported =
        Describe("world_streaming.layer_ownership.unsupported", ErrorSeverity::Error,
                 "A layer classification and control-owner combination is unsupported or contradictory.",
                 "Use World Streaming for runtime persistent/streamed layers, Editor for editor-only layers, and an explicit gameplay "
                 "or replication authority for runtime-controlled layers.",
                 true);
    const ErrorCodeDescriptor LayerOwnershipIdentityConflict =
        Describe("world_streaming.layer_ownership.identity_conflict", ErrorSeverity::Error,
                 "A layer replacement names a different stable layer identity.",
                 "Resolve the exact current layer identity before publishing its successor.", false);
    const ErrorCodeDescriptor LayerOwnershipRevisionStale =
        Describe("world_streaming.layer_ownership.revision_stale", ErrorSeverity::Warning,
                 "A layer publication is missing the current revision or is not its exact successor.",
                 "Reload the current layer fact and submit its next non-wrapping revision.", false);
    const ErrorCodeDescriptor LayerOwnershipOwnerStale =
        Describe("world_streaming.layer_ownership.owner_stale", ErrorSeverity::Warning,
                 "A layer fact does not belong to the active mounted-world owner lifetime.",
                 "Discard the stale fact and rebuild it for the current partition, epoch, and runtime owner.", false);
    const ErrorCodeDescriptor LayerOwnershipCapacityExceeded =
        Describe("world_streaming.layer_ownership.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded layer-ownership authority cannot admit another stable layer.",
                 "Retire an existing layer fact or increase the host-configured capacity.", false);
    const ErrorCodeDescriptor LayerOwnershipLifecycleUnavailable =
        Describe("world_streaming.layer_ownership.lifecycle_unavailable", ErrorSeverity::Warning,
                 "The layer-ownership authority is cancelling or closed to publication.",
                 "Finish retirement or publish to a new active mounted-world authority.", false);
    const ErrorCodeDescriptor LayerFilterInvalid =
        Describe("world_streaming.layer_filter.invalid", ErrorSeverity::Error,
                 "A layer-filter policy, context, candidate snapshot, or output request is malformed.",
                 "Provide exact typed identities, canonical unique candidates, positive limits, and complete caller-owned output.", true);
    const ErrorCodeDescriptor LayerFilterUnsupported =
        Describe("world_streaming.layer_filter.unsupported", ErrorSeverity::Error,
                 "A layer target, optional policy, flag set, or classification mapping is unsupported or contradictory.",
                 "Use a supported editor/client/server target and keep persistent policy consistent with manifest flags.", true);
    const ErrorCodeDescriptor LayerFilterStale =
        Describe("world_streaming.layer_filter.stale", ErrorSeverity::Warning,
                 "A layer-filter pass no longer names the current mounted world or immutable policy revision.",
                 "Capture the current world owner and filter-policy publication before retrying.", false);
    const ErrorCodeDescriptor LayerFilterIdentityConflict =
        Describe("world_streaming.layer_filter.identity_conflict", ErrorSeverity::Error,
                 "A layer-filter input snapshot repeats one stable source layer identity.",
                 "Supply each stable layer identity exactly once in canonical ascending order.", true);
    const ErrorCodeDescriptor LayerFilterCapacityExceeded =
        Describe("world_streaming.layer_filter.capacity_exceeded", ErrorSeverity::Error,
                 "A layer-filter pass exceeds its bounded candidate or output decision capacity.",
                 "Reduce the immutable candidate snapshot or supply complete output storage within the configured ceiling.", false);
    const ErrorCodeDescriptor LayerFilterLifecycleUnavailable =
        Describe("world_streaming.layer_filter.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Layer filtering is unavailable while its authority is cancelling or closed.",
                 "Use an active mounted-world filtering authority and current policy snapshot.", false);
    const ErrorCodeDescriptor LayerStateInvalid =
        Describe("world_streaming.layer_state.invalid", ErrorSeverity::Error,
                 "A layer-state record, fence, or authority snapshot is malformed.",
                 "Provide exact world, layer, ownership and state revisions with positive bounded capacity.", true);
    const ErrorCodeDescriptor LayerStateUnsupported =
        Describe("world_streaming.layer_state.unsupported", ErrorSeverity::Error,
                 "A layer-state value or transition command is unsupported.",
                 "Use the closed load, activation, deactivation, unload, cancellation, and failure transition contract.", true);
    const ErrorCodeDescriptor LayerStateStale =
        Describe("world_streaming.layer_state.stale", ErrorSeverity::Warning,
                 "A layer-state command no longer names the current world, layer, ownership revision, or state revision.",
                 "Capture the current layer-state fence before retrying the command.", false);
    const ErrorCodeDescriptor LayerStateCapacityExceeded =
        Describe("world_streaming.layer_state.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded layer-state authority cannot admit another stable layer.",
                 "Retire an existing layer record or increase the host-configured capacity.", false);
    const ErrorCodeDescriptor LayerStateTransitionInvalid =
        Describe("world_streaming.layer_state.transition_invalid", ErrorSeverity::Error,
                 "The requested layer-state transition is illegal from the current ordered state.",
                 "Reload the current state and request the next legal load, activation, deactivation, or unload edge.", false);
    const ErrorCodeDescriptor LayerStateLifecycleUnavailable =
        Describe("world_streaming.layer_state.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Layer-state admission or forward progress is unavailable during cancellation or after shutdown.",
                 "Drain admitted work to Unloaded or create a state record under a new active owner lifetime.", false);
    const ErrorCodeDescriptor RuntimeEntityCellExitInvalid =
        Describe("world_streaming.runtime_entity_cell_exit.invalid", ErrorSeverity::Error,
                 "A runtime-entity cell-exit request, context, handle, or capacity is malformed.",
                 "Provide one exact operation, runtime entity, retiring-cell fence, current ownership fact and positive capacity.", true);
    const ErrorCodeDescriptor RuntimeEntityCellExitUnsupported =
        Describe("world_streaming.runtime_entity_cell_exit.unsupported", ErrorSeverity::Error,
                 "A runtime-entity cell-exit policy, ownership successor, or transition value is unsupported.",
                 "Retire only Retire-policy entities and hand off RequireHandoff entities through an exact ownership successor.", true);
    const ErrorCodeDescriptor RuntimeEntityCellExitStale =
        Describe("world_streaming.runtime_entity_cell_exit.stale", ErrorSeverity::Warning,
                 "A cell-exit request or command no longer names the current runtime entity, ownership revision, or source-cell "
                 "generation.",
                 "Reload the current ownership fact and retiring-cell fence before creating or advancing the transaction.", false);
    const ErrorCodeDescriptor RuntimeEntityCellExitCapacityExceeded =
        Describe("world_streaming.runtime_entity_cell_exit.capacity_exceeded", ErrorSeverity::Error,
                 "The bounded runtime-entity cell-exit owner cannot admit another in-flight transaction.",
                 "Finish or roll back an existing cell-exit transaction before retrying.", false);
    const ErrorCodeDescriptor RuntimeEntityCellExitLifecycleUnavailable =
        Describe("world_streaming.runtime_entity_cell_exit.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Runtime-entity cell-exit admission is cancelling or closed.",
                 "Drain existing committed retirements and submit new work only to an active mounted-world owner.", false);
    const ErrorCodeDescriptor RuntimeEntityCellExitTransitionInvalid =
        Describe("world_streaming.runtime_entity_cell_exit.transition_invalid", ErrorSeverity::Error,
                 "A known runtime-entity cell-exit transition is illegal from the current transaction phase.",
                 "Follow the retire or destination-prepare, accept and source-retire sequence for the exact operation.", false);
    const ErrorCodeDescriptor PartitionSettingsInvalid =
        Describe("world_streaming.partition_settings.invalid", ErrorSeverity::Error,
                 "A partition capability snapshot or project-settings request is malformed.",
                 "Provide known profile, precision and package values with non-zero revisions and positive coherent limits.", true);
    const ErrorCodeDescriptor PartitionSettingsUnsupported =
        Describe("world_streaming.partition_settings.unsupported", ErrorSeverity::Error,
                 "The requested precision or package mode is not supported by the selected profile and host.",
                 "Select an explicitly supported precision and package representation; no fallback is applied.", true);
    const ErrorCodeDescriptor PartitionSettingsCapacityExceeded =
        Describe("world_streaming.partition_settings.capacity_exceeded", ErrorSeverity::Error,
                 "Requested world-partition grid or storage limits exceed host capabilities.",
                 "Reduce the project limits or choose a product artifact with sufficient declared capability.", true);
    const ErrorCodeDescriptor PartitionSettingsStale =
        Describe("world_streaming.partition_settings.stale", ErrorSeverity::Warning,
                 "Project settings or host capability evidence is stale.",
                 "Resolve the project settings again against the current immutable capability snapshot.", false);
    const ErrorCodeDescriptor PartitionSettingsLifecycleUnavailable =
        Describe("world_streaming.partition_settings.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Partition settings admission is cancelling or closed.",
                 "Wait for a new active project settings owner before admitting dependent work.", false);
    const ErrorCodeDescriptor CellAssetRequestInvalid =
        Describe("world_streaming.cell_asset_request.invalid", ErrorSeverity::Error,
                 "A cell asset request identity, fence, or mandatory limit is malformed.",
                 "Provide a valid request identity, exact operation fence, and positive bounded request ceiling.", true);
    const ErrorCodeDescriptor CellAssetRequestStale =
        Describe("world_streaming.cell_asset_request.stale", ErrorSeverity::Warning,
                 "A cell asset request names a replaced candidate or mounted partition.",
                 "Discard it and submit from the current generation-pinned candidate.", false);
    const ErrorCodeDescriptor CellAssetRequestUnavailable =
        Describe("world_streaming.cell_asset_request.unavailable", ErrorSeverity::Error,
                 "A manifest dependency or registered cooked asset cannot be resolved.",
                 "Rebuild the manifest and registry so every hard dependency has one exact cooked package.", false);
    const ErrorCodeDescriptor CellAssetRequestCapacityExceeded =
        Describe("world_streaming.cell_asset_request.capacity_exceeded", ErrorSeverity::Error,
                 "The candidate dependency tree exceeds its explicit request ceiling.",
                 "Raise the supported ceiling or reduce the canonical hard-dependency set before admission.", false);
    const ErrorCodeDescriptor CellAssetRequestLifecycleUnavailable =
        Describe("world_streaming.cell_asset_request.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Asset request admission is cancelling, closed, or no longer controllable.",
                 "Wait for retirement or submit through a new active request owner.", false);
    const ErrorCodeDescriptor CellAssetRequestNotReady =
        Describe("world_streaming.cell_asset_request.not_ready", ErrorSeverity::Info, "The aggregate still has provider work in flight.",
                 "Poll without blocking and consume only after the request reaches a terminal state.", true);
    const ErrorCodeDescriptor CellAssetRequestConsumed =
        Describe("world_streaming.cell_asset_request.consumed", ErrorSeverity::Error, "The terminal aggregate result was already consumed.",
                 "Retain the owned batch returned by the first successful terminal take.", false);
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
