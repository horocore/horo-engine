# PCG Generation Plan Migration

## CPU evaluator migration (HORO-2019)

The pure `EvaluatePCGCpu` boundary dispatches only explicit version-1 PCG built-ins
over cooked pins and one validated immutable spatial snapshot. Hosts must compose
matching inert runtime descriptors, declare one bounded PointSet output shape per
cooked output pin, grant every required capability, and supply a nonzero numeric
profile fingerprint when a profile-deterministic node is present. The cooked plan
now exposes its already-encoded authored seed without changing canonical bytes.

The result owns detached immutable point candidates plus exact source, seed, snapshot
and profile evidence; it is not an authority to publish scene or target state. A
generation-plan adapter must still derive typed target intents and exact ownership
receipts at the later host/commit boundary. Old snapshots and candidates remain
valid after a source/provider replacement; callers charge old/new overlap and do not
reinterpret an old candidate as current merely because its storage remains alive.

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

CPU evaluator callers must now supply stable world/cell scope, numeric policy,
certified profile and a digest of the captured provider's canonical content in
`PCGCpuEvaluationLimits`. Do not replace content evidence with a revision hash.
Optional exposed overrides carry their owner-issued revision; cooked defaults are
bound to the graph revision. Detached candidates retain canonical per-node roots
for reuse validation. `RequiredBytes` permits complete operation admission before
point-column allocation; the scratch slice includes workers and provenance too.

Provenance input admission now accepts 512 individual stamps, matching the High
graph tier (Standard requires 128). Provider admission stays at 64. Existing
caller signatures, public ownership, schema-1 encoding, keys and sample streams
for previously accepted records are unchanged. The only runtime capture caller
is the CPU evaluator; provenance unit tests also exercise direct captures.
Callers must continue to charge all captured records against operation budgets.
