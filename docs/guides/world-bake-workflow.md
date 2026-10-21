# World Composition bake workflow (HORO-1636)

`HoroWorldBakeApplication` is a non-installed host composition above EditorModel,
SceneRuntimeConversion, SceneCellPayload, WorldStreaming, Assets and the root-bound
ProjectReadFilesApi capability and existing ProjectMutationGuard. Native reader
construction remains explicit host/test composition; this target does not export
the concrete PlatformProjectReadFiles implementation. It is deliberately separate from HoroApplication and
from the editor model. No lower owner depends on this target. Its public header is
owned by this target alone, with an independent public consumer. The controller
owns the service; detaching World Composition or Global Dock presentation does not
cancel accepted work or release its captures.

## Capture and real production path

The graphical executable composes `WorkspaceReadSourceFactory` before starting
`GuiScreenHost`. Each project activation receives a fresh workspace identity and
retained registry/world readers, both bound to that identity at generation one.
`HoroEditor` selects the native implementation; `HoroGui` consumes only the injected
capability and has no PlatformProjectReadFiles edge or native reader/entropy fallback.
Missing factories, relative roots and partial pairs fail closed; original adapter
errors cross the bridge intact. Other hosts must explicitly compose their own
native or restricted capture. Existing document, viewport and source ownership is
unchanged; no ambient reader or second world store is introduced.

The controller captures the canonical `WorldDocuments()` immutable root, including
owner, generation, publication and exact page heads. Loaded pages retain actual
immutable SceneDocument leases. Unloaded pages resolve their source AssetIds through
the captured registry to safe relative paths. Application admission retains the
existing `ProjectMutationLease` through result adoption, preventing cooperating
save/import/package writers from changing captured project state. Replacements
share only this controller's live lease; rejected admission preserves older work.
Workers read actual bytes through the
retained root authority, whose owner/generation remain separate from project and
schema revisions, reject unknown component owners and pass the real authored
values through `ConvertSceneSourceToRuntime`. The source SHA-256 values are retained
and re-read after cooking. These reads do not load or pin pages in the editable
working set. Missing payloads, paths, provider/schema evidence or dependencies fail
with typed diagnostics; no empty substitute is cooked.

`WorldBakeProjectCapture` owns its independent project revision, schema digest,
asset registry revision, byte-affecting settings, inert supported schemas, optional
prefab/trigger captures, and verified dependency evidence. Hosts may supply a richer
capture through `SetWorldBakeProject`; the same project requires a strictly newer,
non-wrapping revision. Reusing a revision rejects the whole capture without changing
accepted work; valid replacement fences older publication. The default
GUI capture admits built-in source content without opaque project schemas or
unverified external artifacts. `CaptureWorldBakeDependencies` reads the actual
pinned Assets generation, verifies its complete manifest/envelopes through Assets,
and returns real identity/type/digest evidence. Hosts retain responsibility for
supplying provider-owned dependency edges and source publication revisions; this
adapter never invents them or discovers/activates providers.

The editor host also carries its explicit `--cooked-root` / `--cook-target` selection
into `WorldBakeDependencySource`, with an owned filesystem capability. The I/O worker
resolves the real committed generation, verifies every envelope and known registry
type, and rechecks the current pointer after cooking. Its complete manifest digest
joins the cache settings hash, conservatively invalidating all resource inventory
changes when provider-private graph edges are opaque. Output retains the exact
Assets generation receipt. Supplying both a selection and captured evidence rejects
admission; feature code never guesses a root, target or fallback generation.

Placement uses the same `CreateForAuthoring` and `WorldSpanningObjectPlan` contracts
as the cell browser and inspector. Each actual Scene entity must have exact authored
placement metadata. Unknown expanded prefab membership and provider-split geometry
without a supplied spatial projection are explicit typed failures. A split directive
never duplicates a complete object while claiming geometry was split. Always-present
objects remain in the retained complete converted source scenes, outside streaming
cell output, for explicit base-content packaging.

One attempt-local `IncrementalSceneCellCook` invokes the actual Scene cell cooker.
Complete parent and Physics body references are captured for `ValidateWorldBuild`
(#1575). Valid results are encoded by `EncodeSceneCellPayload` and
`EncodeStreamingCellArtifact`; retained output includes actual canonical HOROCELL
bytes and immutable semantic payloads. Empty/base-only worlds produce no invented
streaming cell. All source, object, assignment, cell, schema, dependency and output
budgets are finite. Logical payload budgets do not claim measured allocator usage;
source scenes, semantic output, encoded output and transient candidates can coexist.

The result is an application-owned **offline bake generation**, not a durable Assets
release or live runtime activation. Source scenes retain base content explicitly.
Packaging a complete index/dependency sibling and installing a release must use
`PublishWorldCook`/Assets with their own package policy, writer lease and final source
reservation. The bake owner introduces no second filesystem current pointer.

## Stable identity and caller migration

Source pages remain independent from cooked cells. Equal object IDs in different
pages are qualified by a versioned `Horo.WorldBake.Object.v1` SHA-256 stream over the
page UUID plus an eight-byte little-endian object ID. The low eight digest bytes
form the nonzero Scene ID; the complete world rejects collisions. Typed parent and
Physics body endpoints and trigger migration evidence are remapped together.
AI and opaque provider identities keep their own authored validation rules.

Chunk IDs use a distinct `Horo.WorldBake.Cell.v1` domain, partition UUID and canonical
ADR-023 serialized cell tuple; collisions reject the complete generation. Cell Scene
identity is derived from its stable chunk, and its authored revision is recorded
alongside exact world/project/source receipts. No native layout, runtime handle or
operation ID enters this identity stream.

Existing metadata, residency, source-edit, selection and browser APIs are unchanged.
New GUI commands carry `WorldDocumentFence` and are routed after drawing. Only the
controller queues work. Existing Build Output source navigation consumes the same
`BuildOutputRecord::source`; Operations consumes the same OperationStore entries.
New public consumers need only `HoroEngine::WorldBakeApplication`.

## Operation and publication lifecycle

The application keeps one active job and one replaceable pending capture. Every
accepted submission gets a fresh store-issued operation and diagnostic session ID.
Replacement cancels older unpublished work; rejection before admission preserves it.
A worker owns its request, cancellation observer, cache and result storage; it never
captures the controller, panel, facade or stack references. The result mutex protects
worker handoff and linearizes cancellation against the host publication seal.

`Pump` checks the actual current canonical document and independent project/schema/
registry fences. Obsolete or cancelled results append no diagnostics and cannot
replace output. Only a fresh successful Bake replaces the last immutable generation;
Validate runs the same real pipeline without replacing it. Validation findings
prevent successful publication and retain navigable origins. Failure preserves its
original typed cause. Progress comes from real conversion, cook, qualification and
encoding stages; shared Operations and Build Output remain the presentation stores.

Host-thread close rejects admission and requests cancellation without blocking the
UI. The application scheduler must subsequently drain before the borrowed process
stores are destroyed. Workers and old immutable output leases survive facade/source
owner teardown safely. Cancellation after the publication seal cannot roll back an
already committed in-memory generation. External disk edits after source verification
are not a durable save/installation receipt; the source digest records the exact
bytes used and future publication must acquire the durable owner's source reservation.

## Qualification

`HoroWorldBakeApplicationTests` and `HoroWorldBakeApplicationPublicHeaderConsumer`
are explicit Windows and Sonar coverage members. Tests exercise populated real
SceneDocuments, equal page-local IDs, deterministic encoded bytes, native unloaded
source capture including spaces/non-ASCII paths, changed source bytes, aggregate
budgets, navigable validation findings, cancellation, replacement, new operation
IDs, stale source/project/schema and shutdown/retained output. The actual controller
route is covered by `HoroEditorWorkspaceControllerTests`; localization coverage
checks both supported catalogs. Runtime/GPU activation and durable release
publication are separate owner qualifications.
