#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingErrorDescriptor.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    using Internal::Describe;

    const ErrorCodeDescriptor CellDirectionInvalid =
        Describe("world_streaming.cell_direction.invalid", ErrorSeverity::Error,
                 "A direction owner, demand revision or participant set is malformed.",
                 "Supply exact queued operation evidence and a complete bounded participant set.", true);
    const ErrorCodeDescriptor CellDirectionUnsupported =
        Describe("world_streaming.cell_direction.unsupported", ErrorSeverity::Error, "A demand or direction command is unsupported.",
                 "Use the declared residency and owner stage-boundary commands.", true);
    const ErrorCodeDescriptor CellDirectionStale =
        Describe("world_streaming.cell_direction.stale", ErrorSeverity::Warning,
                 "Demand, completion or retirement evidence does not match the exact current attempt.",
                 "Route old cleanup to its original owner; never publish late work into a successor generation.", false);
    const ErrorCodeDescriptor CellDirectionCapacityExceeded =
        Describe("world_streaming.cell_direction.capacity_exceeded", ErrorSeverity::Warning,
                 "The complete retirement participant set exceeds its mandatory ceiling.",
                 "Admit sufficient bounded ownership before starting participant work.", false);
    const ErrorCodeDescriptor CellDirectionLifecycleUnavailable =
        Describe("world_streaming.cell_direction.lifecycle_unavailable", ErrorSeverity::Warning,
                 "Direction admission, progress, ownership transfer or terminal consumption is unavailable.",
                 "Drain exact participant acknowledgements before release and consume the immutable terminal result once.", false);
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
    const ErrorCodeDescriptor CellAssetRequestCancelled =
        Describe("world_streaming.cell_asset_request.cancelled", ErrorSeverity::Warning,
                 "Cancellation suppressed an unpublished cell asset batch after its children retired.",
                 "Retire the exact request and admit a fresh attempt if demand returns.", false);
    const ErrorCodeDescriptor CellAssetRequestConsumed =
        Describe("world_streaming.cell_asset_request.consumed", ErrorSeverity::Error, "The terminal aggregate result was already consumed.",
                 "Retain the owned batch returned by the first successful terminal take.", false);
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
