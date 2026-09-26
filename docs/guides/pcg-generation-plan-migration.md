# PCG Generation Plan Migration

## Point-cloud workspace migration (HORO-2022)

PCG evaluators obtain one `PCGPointCloudWorkspace` per admitted operation from the
exact `PCGCookedPlan`, declaring the schema and worst-case point count of every
PointSet output pin. The plan's `Tier()` accessor supplies the tier for matching
schemas and scratch reservations; cooked bytes and compiler version remain version 1.
Callers advance nodes in plan order, seal each output before finishing its node, and
read routed inputs only while evaluating their target node. Retain a previous
workspace's `ReservedBytes()` as replacement overlap until its workers and borrows
have retired. Existing immutable `PCGPointStorage` candidates remain the boundary for
detached published snapshots; they are not the intermediate allocation owner.

PCG evaluators must now return a detached `PCGGenerationPlanCandidate` instead of
issuing target commands or exposing mutable output containers. Target mutation remains
a later host-coordinated operation.

## Evaluator migration

1. Preserve the exact `ExecutionId`, deterministic seed, graph generation, cell,
   stable lineage and generated-set identities captured at evaluation admission.
2. Give every logical output a `GenerationLogicalOutputId` that remains stable across
   reevaluation. Keep the execution-local `GeneratedOutputId` as source provenance;
   do not derive logical identity from vector position, display name, pointer, or target
   handle.
3. Record each semantic dependency with a non-zero revision and fixed content digest.
4. Emit only `Create`, `Update`, and `Remove` entries. An unchanged output is
   retained by omission. Supply checked work and lifecycle-byte estimates for every
   emitted entry.

## Host and target migration

The host captures one `PCGGenerationTargetReceipt` from the explicit target owner and
passes the same receipt as `PCGGenerationPlanContext::currentTarget`. The target also
projects a bounded immutable `PCGOwnedGeneratedOutput` snapshot for the exact scope.
That borrowed snapshot only needs to outlive the creation call; the returned plan owns
all accepted values.

Update and removal succeed only when the logical output, lineage, set, cell, target
owner, ownership generation, expected prior set revision, and prior content digest
match the target-owned record. Creates use an expected prior set revision of zero.
Do not synthesize these records from graph IDs, names, tags, folders, hierarchy, or
spatial queries. A mismatch is a conflict and preserves existing state.

Call `ReplacePCGGenerationPlan` for reevaluation. It requires a distinct plan and
execution, the same graph/lineage/set/cell/owner, and a strictly newer set revision.
The previous plan stays memory-valid for readers until their ordinary value handles
are released. Cancellation or shutdown must pass the corresponding admission state;
the API then returns a typed lifecycle failure without publishing a candidate.
