#pragma once

/**
 * @file WorldStreamingErrors.h
 * @brief Stable errors for world-partition identity and spatial contracts.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    /** @brief A streaming trace binding, stage, subject, parent, or terminal request is malformed. */
    extern const ErrorCodeDescriptor TraceInvalid;
    /** @brief A streaming trace stage or terminal status is unknown to this contract version. */
    extern const ErrorCodeDescriptor TraceUnsupported;
    /** @brief A streaming trace command names a foreign or superseded binding revision. */
    extern const ErrorCodeDescriptor TraceStale;
    /** @brief A streaming trace repeats a span identity or names an unavailable parent. */
    extern const ErrorCodeDescriptor TraceIdentityConflict;
    /** @brief A streaming trace cannot admit another stage within its mandatory lifetime ceiling. */
    extern const ErrorCodeDescriptor TraceCapacityExceeded;
    /** @brief Streaming trace admission, completion, or replacement is unavailable in the current lifecycle. */
    extern const ErrorCodeDescriptor TraceLifecycleUnavailable;
    /** @brief Storage required for a streaming trace binding could not be allocated. */
    extern const ErrorCodeDescriptor TraceStorageUnavailable;
    /** @brief A streaming trace mutation ran outside its declaring authority owner thread. */
    extern const ErrorCodeDescriptor TraceThreadAffinityViolation;
    /** @brief A streaming metric binding, policy, handle set or sample is malformed. */
    extern const ErrorCodeDescriptor MetricInvalid;
    /** @brief A streaming metric availability, requirement, collection level or lifecycle value is unsupported. */
    extern const ErrorCodeDescriptor MetricUnsupported;
    /** @brief A metric sample or replacement no longer matches the active owner or revisions. */
    extern const ErrorCodeDescriptor MetricStale;
    /** @brief A complete metric sample exceeds one of its admitted measurement maxima. */
    extern const ErrorCodeDescriptor MetricCapacityExceeded;
    /** @brief Required metric collection is unavailable from host composition. */
    extern const ErrorCodeDescriptor MetricCapabilityUnavailable;
    /** @brief Metric publication or replacement is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor MetricLifecycleUnavailable;
    /** @brief A metric-binding operation ran outside its declaring authority owner thread. */
    extern const ErrorCodeDescriptor MetricThreadAffinityViolation;
    /** @brief A network-streaming authority config, command, report, or proof is malformed. */
    extern const ErrorCodeDescriptor NetworkStreamingAuthorityInvalid;
    /** @brief A network-streaming intent, readiness value, or state proof is unsupported. */
    extern const ErrorCodeDescriptor NetworkStreamingAuthorityUnsupported;
    /** @brief A network-streaming command, report, session, partition, sequence, or local fence is stale. */
    extern const ErrorCodeDescriptor NetworkStreamingAuthorityStale;
    /** @brief The client snapshot cannot admit another server-relevant cell within its mandatory bound. */
    extern const ErrorCodeDescriptor NetworkStreamingAuthorityCapacityExceeded;
    /** @brief Server-intent admission is cancelled, or all protocol admission is closed by shutdown. */
    extern const ErrorCodeDescriptor NetworkStreamingAuthorityLifecycleUnavailable;
    /** @brief A Ready report lacks a current satisfying local residency fence, or terminal failure carries one. */
    extern const ErrorCodeDescriptor NetworkStreamingReadinessInvalid;
    /** @brief An origin-frame binding or externally supplied local coordinate is malformed. */
    extern const ErrorCodeDescriptor OriginFrameInvalid;
    /** @brief A local coordinate, candidate, or lease belongs to a non-active origin generation. */
    extern const ErrorCodeDescriptor OriginFrameStale;
    /** @brief A global/local conversion exceeds the supported local frame or signed global range. */
    extern const ErrorCodeDescriptor OriginFrameRangeExceeded;
    /** @brief An externally supplied local coordinate cannot preserve canonical millimeter precision. */
    extern const ErrorCodeDescriptor OriginFramePrecisionLoss;
    /** @brief An origin-frame operation is unavailable because no candidate exists or the owner is closed. */
    extern const ErrorCodeDescriptor OriginFrameLifecycleUnavailable;
    /** @brief Storage required for an origin-frame owner or replacement lease could not be allocated. */
    extern const ErrorCodeDescriptor OriginFrameStorageUnavailable;
    /** @brief A World Streaming diagnostic snapshot, decision row or aggregate queue fact is malformed. */
    extern const ErrorCodeDescriptor DiagnosticProjectionInvalid;
    /** @brief A diagnostic row names a foreign owner, partition epoch, operation or revision. */
    extern const ErrorCodeDescriptor DiagnosticProjectionStale;
    /** @brief A diagnostic lifecycle, cell-state, event or failure value is unsupported. */
    extern const ErrorCodeDescriptor DiagnosticProjectionUnsupported;
    /** @brief A diagnostic snapshot repeats a stable source, cell, failure, event, sequence or context identity. */
    extern const ErrorCodeDescriptor DiagnosticProjectionIdentityConflict;
    /** @brief A diagnostic snapshot exceeds a configured or implementation-owned bound. */
    extern const ErrorCodeDescriptor DiagnosticProjectionCapacityExceeded;
    /** @brief A cell-state ledger, owner, fence, or record is malformed. */
    extern const ErrorCodeDescriptor CellStateInvalid;
    /** @brief No tracked residency record exists for the exact mounted cell attempt. */
    extern const ErrorCodeDescriptor CellStateUnresolved;
    /** @brief A cell-state request names a foreign owner, epoch, cell, or non-successor generation. */
    extern const ErrorCodeDescriptor CellStateStale;
    /** @brief A canonical operation phase/outcome cannot be projected by this residency contract version. */
    extern const ErrorCodeDescriptor CellStateUnsupported;
    /** @brief A projected residency transition is illegal from the current canonical state. */
    extern const ErrorCodeDescriptor CellStateTransitionInvalid;
    /** @brief A fresh or overlapping cell attempt cannot fit the ledger's mandatory tracked-attempt ceiling. */
    extern const ErrorCodeDescriptor CellStateCapacityExceeded;
    /** @brief A draining or closed cell-state ledger rejects new loading or publication. */
    extern const ErrorCodeDescriptor CellStateLifecycleUnavailable;
    /** @brief A runtime composition omits or malforms its explicit owner, scheduler, core service, or adapter facts. */
    extern const ErrorCodeDescriptor RuntimeCompositionInvalid;
    /** @brief Runtime composition service identities are duplicated. */
    extern const ErrorCodeDescriptor RuntimeCompositionIdentityConflict;
    /** @brief Runtime composition bindings exceed their mandatory feature-adapter ceiling. */
    extern const ErrorCodeDescriptor RuntimeCompositionCapacityExceeded;
    /** @brief A runtime composition command names a foreign owner or stale/non-successor revision. */
    extern const ErrorCodeDescriptor RuntimeCompositionRevisionStale;
    /** @brief Runtime composition replacement or cancellation is unavailable while draining, closed, or retaining work. */
    extern const ErrorCodeDescriptor RuntimeCompositionLifecycleUnavailable;
    /** @brief A fallback provider descriptor has malformed ownership, revision, mode-specific or cell data. */
    extern const ErrorCodeDescriptor FallbackProviderInvalid;
    /** @brief A fallback provider mode is unknown to this contract version. */
    extern const ErrorCodeDescriptor FallbackProviderUnsupported;
    /** @brief A single-cell fallback cannot fit the caller's mandatory published-cell ceiling. */
    extern const ErrorCodeDescriptor FallbackProviderCapacityExceeded;
    /** @brief A fallback operation names a foreign owner or non-successor configuration revision. */
    extern const ErrorCodeDescriptor FallbackProviderStale;
    /** @brief A cancelling or closed fallback provider rejects replacement or cancellation admission. */
    extern const ErrorCodeDescriptor FallbackProviderLifecycleUnavailable;
    /** @brief A world-partition identity uses its reserved invalid representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief A canonical serialized identity is malformed or contains a reserved value. */
    extern const ErrorCodeDescriptor SerializedIdentityInvalid;
    /** @brief A partition epoch, cell-attempt generation, runtime-source revision, or authoring-page revision cannot advance. */
    extern const ErrorCodeDescriptor GenerationExhausted;
    /** @brief A cell operation has a malformed identity, fence, or initial representation. */
    extern const ErrorCodeDescriptor CellOperationInvalid;
    /** @brief A completion or command does not name the exact cell operation and fence. */
    extern const ErrorCodeDescriptor CellOperationStale;
    /** @brief A cell operation transition value is unknown to this contract version. */
    extern const ErrorCodeDescriptor CellOperationUnsupported;
    /** @brief A known cell operation transition is not legal from the current phase. */
    extern const ErrorCodeDescriptor CellOperationTransitionInvalid;
    /** @brief A scheduler admission ledger, request, or reservation is malformed. */
    extern const ErrorCodeDescriptor SchedulerAdmissionInvalid;
    /** @brief Scheduler operation count or generic capacity cannot be reserved within configured ceilings. */
    extern const ErrorCodeDescriptor SchedulerCapacityExceeded;
    /** @brief A scheduler operation already owns a reservation in this ledger. */
    extern const ErrorCodeDescriptor SchedulerReservationConflict;
    /** @brief A scheduler command does not name the exact owner-scoped reservation and operation fence. */
    extern const ErrorCodeDescriptor SchedulerReservationStale;
    /** @brief Scheduler admission is draining, closed, or waiting for an operation to retire. */
    extern const ErrorCodeDescriptor SchedulerLifecycleUnavailable;
    /** @brief A multidimensional budget vector, policy, request, or evaluation context is malformed. */
    extern const ErrorCodeDescriptor BudgetModelInvalid;
    /** @brief A budget vector or policy names a resource dimension unsupported by this contract version. */
    extern const ErrorCodeDescriptor BudgetDimensionUnsupported;
    /** @brief A budget policy or usage sample revision no longer matches current authority state. */
    extern const ErrorCodeDescriptor BudgetRevisionStale;
    /** @brief A budget sample has malformed window timing. */
    extern const ErrorCodeDescriptor BudgetSampleInvalid;
    /** @brief A budget sample belongs to an earlier completed sampling window. */
    extern const ErrorCodeDescriptor BudgetSampleStale;
    /** @brief Projected usage overflows or exceeds one independent hard resource limit. */
    extern const ErrorCodeDescriptor BudgetCapacityExceeded;
    /** @brief A world-cell grid has zero/overflowing cell size, inverted bounds, or no LODs. */
    extern const ErrorCodeDescriptor QuantizationPolicyInvalid;
    /** @brief A coordinate cannot be translated relative to the grid origin without signed overflow. */
    extern const ErrorCodeDescriptor CoordinateOutOfRange;
    /** @brief The requested LOD is not declared by the grid policy. */
    extern const ErrorCodeDescriptor LodUnsupported;
    /** @brief The deterministic cell coordinate falls outside the inclusive manifest grid bounds. */
    extern const ErrorCodeDescriptor CellOutOfBounds;
    /** @brief A world-partition descriptor is incomplete or contains malformed fields. */
    extern const ErrorCodeDescriptor PartitionDescriptorInvalid;
    /** @brief A world-partition descriptor uses an unsupported schema version. */
    extern const ErrorCodeDescriptor PartitionVersionUnsupported;
    /** @brief World content bounds are unordered, overflow the grid envelope, or lie outside it. */
    extern const ErrorCodeDescriptor PartitionBoundsInvalid;
    /** @brief A partition descriptor exceeds mandatory host storage limits. */
    extern const ErrorCodeDescriptor PartitionCapacityExceeded;
    /** @brief A partition descriptor repeats a layer or exact cell identity. */
    extern const ErrorCodeDescriptor PartitionIdentityConflict;
    /** @brief A partition-registry identity, binding, query, handle, or limit is malformed. */
    extern const ErrorCodeDescriptor PartitionRegistryInvalid;
    /** @brief No current partition publication or requested cell is available. */
    extern const ErrorCodeDescriptor PartitionRegistryUnavailable;
    /** @brief A partition-registry revision or cell handle names an obsolete publication. */
    extern const ErrorCodeDescriptor PartitionRegistryStale;
    /** @brief A partition publication or query exceeds its mandatory bounded ceiling. */
    extern const ErrorCodeDescriptor PartitionRegistryCapacityExceeded;
    /** @brief A partition query filter or indexed cell representation is unsupported. */
    extern const ErrorCodeDescriptor PartitionRegistryUnsupported;
    /** @brief New partition publication or snapshot capture is cancelling or closed. */
    extern const ErrorCodeDescriptor PartitionRegistryLifecycleUnavailable;
    /** @brief Immutable partition-registry publication storage could not be allocated. */
    extern const ErrorCodeDescriptor PartitionRegistryStorageUnavailable;
    /** @brief A cooked world-index manifest is incomplete or contains malformed metadata. */
    extern const ErrorCodeDescriptor CookedManifestInvalid;
    /** @brief A cooked world-index manifest exceeds a mandatory count or byte ceiling. */
    extern const ErrorCodeDescriptor CookedManifestCapacityExceeded;
    /** @brief Cooked metadata does not map one-to-one to the authoritative descriptor cells. */
    extern const ErrorCodeDescriptor CookedManifestIdentityConflict;
    /** @brief A cooked cell dependency is invalid, duplicated, self-referential, or absent from the manifest. */
    extern const ErrorCodeDescriptor CookedManifestDependencyInvalid;
    /** @brief A cell candidate context, fixed header, or payload table is malformed. */
    extern const ErrorCodeDescriptor CellCandidateInvalid;
    /** @brief A cell candidate requests an unsupported format, provider contract, or operation phase. */
    extern const ErrorCodeDescriptor CellCandidateUnsupported;
    /** @brief A cell candidate does not match its exact manifest record, operation, partition, or generation. */
    extern const ErrorCodeDescriptor CellCandidateStale;
    /** @brief Cell candidate header or payload storage exceeds a mandatory caller ceiling. */
    extern const ErrorCodeDescriptor CellCandidateCapacityExceeded;
    /** @brief The requested manifest cell is absent from the immutable cooked index. */
    extern const ErrorCodeDescriptor CellCandidateUnavailable;
    /** @brief Candidate preparation is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor CellCandidateLifecycleUnavailable;
    /** @brief A cell activation identity, fence, requirement or receipt is malformed. */
    extern const ErrorCodeDescriptor CellActivationInvalid;
    /** @brief A prepared receipt or commit command names another operation, generation or service revision. */
    extern const ErrorCodeDescriptor CellActivationStale;
    /** @brief The required Scene/provider receipt set is missing, duplicated or otherwise incomplete. */
    extern const ErrorCodeDescriptor CellActivationIncomplete;
    /** @brief A required receipt set exceeds its mandatory admission ceiling. */
    extern const ErrorCodeDescriptor CellActivationCapacityExceeded;
    /** @brief Cell activation admission or publication is closed by cancellation, shutdown or terminal ownership. */
    extern const ErrorCodeDescriptor CellActivationLifecycleUnavailable;
    /** @brief Publication was requested outside CommitDeferredLifecycleChanges. */
    extern const ErrorCodeDescriptor CellActivationSafePointUnavailable;
    /** @brief A spatial-assignment request is empty, malformed, or has unordered/out-of-partition bounds. */
    extern const ErrorCodeDescriptor SpatialAssignmentInvalid;
    /** @brief A spatial-assignment request repeats one stable authored-object address. */
    extern const ErrorCodeDescriptor SpatialAssignmentIdentityConflict;
    /** @brief Spatial assignment exceeds an object-count, per-object-cell, or total-assignment ceiling. */
    extern const ErrorCodeDescriptor SpatialAssignmentCapacityExceeded;
    /** @brief A requested spatial-assignment layer is not declared by the authoritative descriptor. */
    extern const ErrorCodeDescriptor SpatialAssignmentUnsupported;
    /** @brief A quantized object intersects a cell absent from the authoritative descriptor. */
    extern const ErrorCodeDescriptor SpatialAssignmentCellUnavailable;
    /** @brief A spanning-object plan or directive is malformed, missing, or applied below its threshold. */
    extern const ErrorCodeDescriptor SpanningObjectPlanInvalid;
    /** @brief A spanning-object directive repeats or names an object absent from spatial assignment. */
    extern const ErrorCodeDescriptor SpanningObjectPlanIdentityConflict;
    /** @brief A spanning-object directive does not match the assigned immutable object revision. */
    extern const ErrorCodeDescriptor SpanningObjectPlanRevisionStale;
    /** @brief A spanning-object directive uses an unknown policy value. */
    extern const ErrorCodeDescriptor SpanningObjectPlanUnsupported;
    /** @brief A spanning-object plan exceeds its object or aggregate placement-cell ceiling. */
    extern const ErrorCodeDescriptor SpanningObjectPlanCapacityExceeded;
    /** @brief An explicit single-cell owner is not covered by the source spatial assignment. */
    extern const ErrorCodeDescriptor SpanningObjectPlanOwnerUnavailable;
    /** @brief A dependency-plan request has malformed, unknown, self-referential, duplicated, or missing source data. */
    extern const ErrorCodeDescriptor DependencyPlanInvalid;
    /** @brief A dependency endpoint revision does not match the admitted spatial-assignment revision. */
    extern const ErrorCodeDescriptor DependencyPlanRevisionStale;
    /** @brief A dependency plan exceeds an edge, per-object, bundle-member, or soft-reference ceiling. */
    extern const ErrorCodeDescriptor DependencyPlanCapacityExceeded;
    /** @brief A hard dependency target is absent from the admitted spatial assignments. */
    extern const ErrorCodeDescriptor DependencyPlanHardTargetMissing;
    /** @brief A soft reference contradicts the transitive hard co-load policy for the same objects. */
    extern const ErrorCodeDescriptor DependencyPlanAmbiguous;
    /** @brief A streaming source descriptor or admission context is structurally invalid. */
    extern const ErrorCodeDescriptor SourceDescriptorInvalid;
    /** @brief A streaming source intent is not supported by this contract version. */
    extern const ErrorCodeDescriptor SourceIntentUnsupported;
    /** @brief A streaming source owner token no longer names the active owner lifetime. */
    extern const ErrorCodeDescriptor SourceOwnerStale;
    /** @brief A source update does not advance the currently admitted revision. */
    extern const ErrorCodeDescriptor SourceRevisionStale;
    /** @brief A new source cannot be admitted within the configured bounded capacity. */
    extern const ErrorCodeDescriptor SourceCapacityExceeded;
    /** @brief Source admission is closed because its owner is cancelling or shut down. */
    extern const ErrorCodeDescriptor SourceLifecycleUnavailable;
    /** @brief A source shape is malformed, unbounded, or violates its representation contract. */
    extern const ErrorCodeDescriptor SourceShapeInvalid;
    /** @brief The evaluating host does not support the requested source shape category. */
    extern const ErrorCodeDescriptor SourceShapeUnsupported;
    /** @brief A velocity-prefetch policy, context, or kinematic sample is malformed. */
    extern const ErrorCodeDescriptor PrefetchInvalid;
    /** @brief A velocity-prefetch contract version or source category is unsupported. */
    extern const ErrorCodeDescriptor PrefetchUnsupported;
    /** @brief Velocity-prefetch evidence names a replaced policy, partition, owner, or expired sample. */
    extern const ErrorCodeDescriptor PrefetchStale;
    /** @brief Velocity-prefetch evaluation is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor PrefetchLifecycleUnavailable;
    /** @brief A source desired state combines residency and retention inconsistently. */
    extern const ErrorCodeDescriptor SourceDesiredStateInvalid;
    /** @brief A source desired-state residency or retention value is not supported by this contract version. */
    extern const ErrorCodeDescriptor SourceDesiredStateUnsupported;
    /** @brief A desired-state reduction context or mandatory limit is malformed. */
    extern const ErrorCodeDescriptor SourceReductionInvalid;
    /** @brief A desired-state reduction repeats one stable source identity. */
    extern const ErrorCodeDescriptor SourceReductionIdentityConflict;
    /** @brief A desired-state reduction exceeds its bounded contributor ceiling. */
    extern const ErrorCodeDescriptor SourceReductionCapacityExceeded;
    /** @brief A priority policy, ranking context, or candidate row is malformed. */
    extern const ErrorCodeDescriptor PriorityPolicyInvalid;
    /** @brief A priority contract version is not supported. */
    extern const ErrorCodeDescriptor PriorityPolicyUnsupported;
    /** @brief A priority ranking request exceeds its immutable candidate or output ceiling. */
    extern const ErrorCodeDescriptor PriorityPolicyCapacityExceeded;
    /** @brief Ranking evidence names a replaced policy publication. */
    extern const ErrorCodeDescriptor PriorityPolicyStale;
    /** @brief Priority ranking is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor PriorityPolicyLifecycleUnavailable;
    /** @brief A cell-stability policy, context, observation, or retained state is malformed. */
    extern const ErrorCodeDescriptor CellStabilityInvalid;
    /** @brief A cell-stability contract version or closed enum value is unsupported. */
    extern const ErrorCodeDescriptor CellStabilityUnsupported;
    /** @brief A new anti-thrash record exceeds the authority's immutable cell ceiling. */
    extern const ErrorCodeDescriptor CellStabilityCapacityExceeded;
    /** @brief Cell-stability evidence names a replaced policy or partition publication. */
    extern const ErrorCodeDescriptor CellStabilityStale;
    /** @brief Cell-stability evaluation is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor CellStabilityLifecycleUnavailable;
    /** @brief A world-authoring contract, page descriptor, request, or authority snapshot is malformed. */
    extern const ErrorCodeDescriptor AuthoringContractInvalid;
    /** @brief The requested world-authoring contract schema version is unsupported. */
    extern const ErrorCodeDescriptor AuthoringVersionUnsupported;
    /** @brief The requested authoring granularity or collaboration authority is unsupported. */
    extern const ErrorCodeDescriptor AuthoringPolicyUnsupported;
    /** @brief The authoring request does not match the current page identity or partition. */
    extern const ErrorCodeDescriptor AuthoringIdentityConflict;
    /** @brief The authoring request carries a stale or non-successor page revision. */
    extern const ErrorCodeDescriptor AuthoringRevisionStale;
    /** @brief The bounded authoring-page capacity cannot admit another open page. */
    extern const ErrorCodeDescriptor AuthoringCapacityExceeded;
    /** @brief Authoring admission is closed because the owner is cancelling or shut down. */
    extern const ErrorCodeDescriptor AuthoringLifecycleUnavailable;
    /** @brief A spatial-object descriptor, request, or owner snapshot is structurally invalid. */
    extern const ErrorCodeDescriptor SpatialObjectDescriptorInvalid;
    /** @brief The spatial-object descriptor schema version is unsupported. */
    extern const ErrorCodeDescriptor SpatialObjectVersionUnsupported;
    /** @brief The spatial-object placement class is unsupported by this contract version. */
    extern const ErrorCodeDescriptor SpatialObjectPlacementUnsupported;
    /** @brief A replacement does not name the currently admitted authored-object identity. */
    extern const ErrorCodeDescriptor SpatialObjectIdentityConflict;
    /** @brief A replacement carries a stale, missing, or non-successor authoring revision. */
    extern const ErrorCodeDescriptor SpatialObjectRevisionStale;
    /** @brief A new spatial-object descriptor exceeds the bounded owner capacity. */
    extern const ErrorCodeDescriptor SpatialObjectCapacityExceeded;
    /** @brief Spatial-object admission is closed because its owner is cancelling or shut down. */
    extern const ErrorCodeDescriptor SpatialObjectLifecycleUnavailable;
    /** @brief An object-ownership descriptor, request, or owner snapshot is structurally invalid. */
    extern const ErrorCodeDescriptor ObjectOwnershipInvalid;
    /** @brief An object class, owner kind, or cell-exit policy is unsupported or incoherent. */
    extern const ErrorCodeDescriptor ObjectOwnershipUnsupported;
    /** @brief An ownership replacement names a different authored or runtime-spawned object. */
    extern const ErrorCodeDescriptor ObjectOwnershipIdentityConflict;
    /** @brief An ownership publication is missing the current revision or is not its exact successor. */
    extern const ErrorCodeDescriptor ObjectOwnershipRevisionStale;
    /** @brief An ownership fact does not belong to the active mounted-world owner lifetime. */
    extern const ErrorCodeDescriptor ObjectOwnershipOwnerStale;
    /** @brief A new ownership record exceeds the bounded owner capacity. */
    extern const ErrorCodeDescriptor ObjectOwnershipCapacityExceeded;
    /** @brief Ownership admission is closed because its authority is cancelling or shut down. */
    extern const ErrorCodeDescriptor ObjectOwnershipLifecycleUnavailable;
    /** @brief A layer-ownership descriptor, request, or owner snapshot is structurally invalid. */
    extern const ErrorCodeDescriptor LayerOwnershipInvalid;
    /** @brief A layer classification and control-owner combination is unsupported or incoherent. */
    extern const ErrorCodeDescriptor LayerOwnershipUnsupported;
    /** @brief A layer replacement does not name the currently admitted stable layer identity. */
    extern const ErrorCodeDescriptor LayerOwnershipIdentityConflict;
    /** @brief A layer publication is missing the current revision or is not its exact successor. */
    extern const ErrorCodeDescriptor LayerOwnershipRevisionStale;
    /** @brief A layer fact does not belong to the active mounted-world owner lifetime. */
    extern const ErrorCodeDescriptor LayerOwnershipOwnerStale;
    /** @brief A new layer fact exceeds the bounded owner capacity. */
    extern const ErrorCodeDescriptor LayerOwnershipCapacityExceeded;
    /** @brief Layer admission is closed because its authority is cancelling or shut down. */
    extern const ErrorCodeDescriptor LayerOwnershipLifecycleUnavailable;
    /** @brief A layer-filter policy, context, candidate, or output request is structurally invalid. */
    extern const ErrorCodeDescriptor LayerFilterInvalid;
    /** @brief A layer target, optional policy, flag set, or classification mapping is unsupported. */
    extern const ErrorCodeDescriptor LayerFilterUnsupported;
    /** @brief A filter pass no longer names the current world or policy revision. */
    extern const ErrorCodeDescriptor LayerFilterStale;
    /** @brief A filter input snapshot repeats one stable layer identity. */
    extern const ErrorCodeDescriptor LayerFilterIdentityConflict;
    /** @brief A filter pass exceeds its candidate or output decision ceiling. */
    extern const ErrorCodeDescriptor LayerFilterCapacityExceeded;
    /** @brief Layer filtering is unavailable because its authority is cancelling or closed. */
    extern const ErrorCodeDescriptor LayerFilterLifecycleUnavailable;
    /** @brief A layer-state record, fence, or authority snapshot is structurally invalid. */
    extern const ErrorCodeDescriptor LayerStateInvalid;
    /** @brief A layer-state enum or transition command is unsupported. */
    extern const ErrorCodeDescriptor LayerStateUnsupported;
    /** @brief A layer-state command no longer names the current owner or revisions. */
    extern const ErrorCodeDescriptor LayerStateStale;
    /** @brief The bounded layer-state authority cannot admit another layer. */
    extern const ErrorCodeDescriptor LayerStateCapacityExceeded;
    /** @brief A requested layer-state edge is illegal from the current state. */
    extern const ErrorCodeDescriptor LayerStateTransitionInvalid;
    /** @brief Layer-state admission or forward progress is closed by cancellation or shutdown. */
    extern const ErrorCodeDescriptor LayerStateLifecycleUnavailable;
    /** @brief A runtime-entity cell-exit request, context, handle, or capacity is malformed. */
    extern const ErrorCodeDescriptor RuntimeEntityCellExitInvalid;
    /** @brief A runtime-entity cell-exit policy, successor, or transition value is unsupported. */
    extern const ErrorCodeDescriptor RuntimeEntityCellExitUnsupported;
    /** @brief A cell-exit command or source observation does not name the current entity and cell generation. */
    extern const ErrorCodeDescriptor RuntimeEntityCellExitStale;
    /** @brief The bounded cell-exit owner cannot admit another in-flight transaction. */
    extern const ErrorCodeDescriptor RuntimeEntityCellExitCapacityExceeded;
    /** @brief Runtime-entity cell-exit admission is cancelling or closed. */
    extern const ErrorCodeDescriptor RuntimeEntityCellExitLifecycleUnavailable;
    /** @brief A known runtime-entity cell-exit transition is illegal from the current phase. */
    extern const ErrorCodeDescriptor RuntimeEntityCellExitTransitionInvalid;
    /** @brief A partition capability snapshot or project-settings request is structurally invalid. */
    extern const ErrorCodeDescriptor PartitionSettingsInvalid;
    /** @brief A precision or package mode is unsupported by the selected project profile or host. */
    extern const ErrorCodeDescriptor PartitionSettingsUnsupported;
    /** @brief Requested grid or storage limits exceed the exact host capability snapshot. */
    extern const ErrorCodeDescriptor PartitionSettingsCapacityExceeded;
    /** @brief Settings or capability evidence no longer names the active immutable revision. */
    extern const ErrorCodeDescriptor PartitionSettingsStale;
    /** @brief Settings-dependent work is unavailable because admission is cancelling or closed. */
    extern const ErrorCodeDescriptor PartitionSettingsLifecycleUnavailable;
    /** @brief A cell asset request identity, fence, or mandatory limit is malformed. */
    extern const ErrorCodeDescriptor CellAssetRequestInvalid;
    /** @brief A cell asset request names a replaced candidate or mounted partition. */
    extern const ErrorCodeDescriptor CellAssetRequestStale;
    /** @brief A manifest dependency or registered cooked asset cannot be resolved. */
    extern const ErrorCodeDescriptor CellAssetRequestUnavailable;
    /** @brief The candidate dependency tree exceeds its explicit request ceiling. */
    extern const ErrorCodeDescriptor CellAssetRequestCapacityExceeded;
    /** @brief Asset request admission is cancelling, closed, or no longer controllable. */
    extern const ErrorCodeDescriptor CellAssetRequestLifecycleUnavailable;
    /** @brief The aggregate still has provider work in flight. */
    extern const ErrorCodeDescriptor CellAssetRequestNotReady;
    /** @brief The terminal aggregate result was already consumed. */
    extern const ErrorCodeDescriptor CellAssetRequestConsumed;
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
