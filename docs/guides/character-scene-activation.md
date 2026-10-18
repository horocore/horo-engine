# Character Scene authoring and activation

`Runtime::CharacterControllerComponent` is durable authoring data. It contains an
object-local component ID, capsule geometry, world-space up/gravity, movement and
platform policies, stable collision-profile/query-channel IDs, and a physical
material asset plus material slot. It contains no controller/body handle, runtime
Scene ID, native object, or filter/origin generation. Those values are issued by
the exact prepared owner and are never written back to source.

Adding the component through the Inspector creates a disabled, unconfigured
component. Geometry and policies must remain valid while disabled. Enabling also
requires explicit profile, query-channel, material asset and slot bindings. No
project default profile, built-in material, native backend or missing body is
inferred. Missing legacy components stay absent. A disabled component remains
present through source conversion and cook so a stop/restart does not lose its
configuration.

The Inspector uses shared editor controls and the document's existing history
authority. `SceneOptionalComponentCommand` unifies the existing add/remove
commands and typed Character replacement in one fixed-payload variant. Existing
typed callers convert to the sum command; callers taking an executor member
pointer must migrate to its sum-command signature and rebuild. Replacement
preserves the existing stable component ID. Invalid or missing replacements do
not mutate document state or consume undo/redo history. This is an internal
editor contract, not a second executor or runtime authority.

## Private candidate placement

Scene activation has two explicit host preparation phases:

1. `InitialPlacement` owners prepare native resources and return immutable,
   exact-`EntityRef` world-pose facts.
2. Scene validates all producer facts together, computes representable local
   transforms against the candidate hierarchy, and applies them to its private
   candidate. `DerivedResources` owners then prepare against these final poses.

Every existing participant defaults to `DerivedResources`. Physics explicitly
selects `InitialPlacement`; VFX mutable preparation and other pose-dependent
resources therefore run afterward, regardless of registration order. Scene does
not discover a Physics backend or interpret Character movement policy. Each
producer owns completeness and semantic validation of its facts. Scene owns
duplicate/foreign/stale entity checks, finite unit rotations, hierarchy mapping,
visual-scale preservation and rejection of singular or nonrepresentable shear.

The Physics participant owns one paired Physics/Character candidate. Stable
project/schema evidence and real retained material bytes are validated before
native staging. The non-installed `CharacterScenePlacement` interface performs
the existing guarded bounded spawn recovery with the authored world heading.
Its initial-query admission checks the exact candidate Scene/Physics/Character
pair, owner thread, Prepared Character state, unticked Physics state and unbound
query generation. It never advances a fixed tick or moves a body. Recovery uses
the real Physics query adapter over already staged colliders.

All projection storage, fallible native preparation and TRS validation precede
the aggregate publication barrier. Publication stays allocation-free and
no-fail. Failure retires prepared Character resources before their paired
Physics resources and preserves the complete old Scene. Recovery changes only
the private runtime candidate: authored document transforms, source bytes and
undo history are unchanged, including after stopping and restarting play.

Enabled body/collider contributors or another enabled controller below a
controller root currently require unsupported dependent restaging and are
rejected before native allocation. Ordinary visual/static parents and visual
children remain supported. Active structural creation/removal or ancestor-pose
changes affecting an enabled Character are rejected before topology commit;
replace the complete Scene or use the existing Character movement authority.
The optional `PhysicsCellAttachment` mode rejects enabled Character before
native staging; it does not implement streamed structural Character activation.

## Rebuild and recook migration

Legacy Scene source without a Character component keeps its exact schema-1
canonical representation. Character-bearing source uses document schema 2;
schema-1 readers must reject it rather than silently discard a required
component. The Character payload itself has schema 1. Removing the final
Character component permits a schema-1 document again.

Cooked Scene format `horo.scene.runtime.v3`, CoreEcs provider codec version 3,
and `Horo.SceneCellCook.Key.v3` include the new typed field. Recook older Scene,
prefab-inline and CoreEcs artifacts; old provider payloads are rejected. Do not
reinterpret an older cache key or publish data after dropping the new field.

The host `SceneActivationParticipant` vtable and `RuntimeComponentSet` change.
Rebuild all engine/editor/Scene/Physics, streaming, prefab, VFX, gameplay and
other host participant consumers together. The phase revision is
`SceneActivationParticipantContractVersion = 2`. Host plugins must not mix old and new
participant binaries.

The supported Gameplay SDK export lists are defined in the top-level
`CMakeLists.txt`: the common set contains `SceneIdentity`, Assets and Runtime;
the optional Physics set contains Gameplay Physics/Character integration,
Physics and PhysicsModel. `RuntimeScene`, SceneModel and
PhysicsSceneIntegration are host-only, and
`HoroCharacterScenePlacementInternal` is neither installed nor exported. The
new PhysicsModel authoring header and factored policy header are additive;
existing Character controller descriptors, Gameplay runtime context layouts
and exported module function signatures remain unchanged. This host migration
does not itself change the main-provided Gameplay SDK 21 epoch. Staged external
Physics/Character consumer compilation remains required evidence, separately
from this source audit.

## Qualification boundaries

Source codec, cooked conversion, document history, generic phase/projection,
and real native Physics/Character lifecycle tests cover different contracts.
Generic participant fixtures qualify Scene's transaction and ordering rules;
they do not prove a native controller. Native cases must use the canonical
Physics backend, real project schema/material data and actual collider queries
before tick zero. Product qualification must additionally exercise explicitly
configured application composition through the real authored/cooked Scene
loader. A headless native run does not prove a visible editor interaction or
GPU presentation. Validation receipts must state which scopes actually ran.

## Host-assigned Scene identity and borrowed native access

A generation-owned headless Scene service can be constructed with an explicit
`SceneRuntimeId`; asset-backed construction takes the seed after the existing
asset limits argument. Existing constructors still start at 1. A zero seed is
not coerced or thrown from construction: each preparation entry rejects it with
`scene.runtime.invalid_candidate` before publication checks, restore callbacks,
asset work or participant preparation. Complete private aggregate preparation consumes the identity; failed restore or
participant preparation retains it for retry, as required by the Scene lifecycle
contract. Subsequent cancellation or publication rejection does not reuse that
completed candidate identity. Replacement advances monotonically;
staging UINT64_MAX exhausts admission instead of reusing zero or another live
identity. Hosts must supply their actual Scene-owned identity rather than
creating a parallel Physics or gameplay identity authority.

The non-installed PhysicsSceneHostAccess boundary borrows the published paired
Physics and Character worlds synchronously on their owner lane. An application
observer must withdraw its borrowed pointer before the Scene candidate shuts
down or rolls back; this interface does not transfer native ownership. The
application's generation bundle retains asset services through candidate and
world retirement. Service-backed gameplay can be constructed without a prefab
provider when the host withholds prefab permission; an explicit prefab grant
without that provider is rejected before gameplay activation.

Cooked asset hosts verify the AssetCook envelope's exact asset identity, type,
target and digests before calling DecodeCookedSceneWithEmbeddedRevision. Scene
then validates the canonical bounded payload, expected stable Scene ID and
nonzero embedded revision. This entry does not claim current-source freshness;
the existing exact-revision decoder continues to enforce its caller's expected
revision for CAS and revision-sensitive loading.

The initial-identity migration also corrects the previous increment occurring
before restore and participant preparation completed. The increment now follows
successful preparation of the entire private aggregate, matching the normative
Scene lifecycle contract. Active aggregate identity and ownership remain intact
on failed preparation, including a failed candidate using UINT64_MAX. The
headless WorldRecord path permits one preparation only; travel constructs a new
record using the Scene owner's supplied fresh identity. The caller remains
responsible for global freshness, and the host rejects collisions with all
currently retained records before native staging. It never manufactures a new
identity from an overlapping local service counter.

`HoroScenePhysicsHost` is a non-installed application composition target shared
by the Editor and headless host. Its single staged header exposes only the
declared Physics internal boundary. Its participant wraps the actual Physics
candidate and forwards both initial-placement facts and canonical dataset
metadata. Publication installs an observer; rollback/retirement withdraws it
before native cleanup. Replacement publishes the new observer before retiring
the old candidate, and old cleanup cannot clear the new observer. The observer
must be declared before its Scene service and outlive all produced candidates.
It grants no second ownership, retains no mutable Scene borrow and performs no
backend discovery. Runtime fixed-step integration must borrow synchronously and
release the scope before any Scene lifecycle operation.

The canonical stable cooked Scene identity is now provided by
`SceneCook::SceneDefinitionIdForAsset`. Cooker and host loader use the same
existing SHA-256 low-eight-byte little-endian derivation (zero maps to one);
this is a compact stable identity, not the process-local runtime incarnation or
proof of collision freedom. Hosts still verify asset envelope provenance and
containing-Scene identity collisions.

The app-private headless Scene load boundary is explicitly blocking only during
startup or requested travel preparation. One absolute caller deadline and copied
cancellation token cover all roles and retries. Only the Scene deferred-lifecycle
phase advances; normal frames, gameplay updates and Physics ticks never drive
asset readiness. The product bundle starts a nonzero CPU worker pool before
constructing AssetLoadService (its jobs use the CPU lane) and retains that pool,
provider, registry and loader until all world candidates retire. Cancellation,
deadline and original asset failures remain distinct; private-service shutdown
cancels/joins only its own accepted handles before returning the primary failure.
A ready private Scene aggregate does not expose a new network role: the mode
owner keeps old role views published until every replacement service is ready
and activated. The Physics factory consumes the already-owned pair and never
activates or retires a second world.

Editor startup accepts explicit `--physics null` or `--physics canonical` (also
`--physics=...`). Absence preserves the existing Null policy; malformed or
missing values reject before native frontend creation. Canonical selection
requires an explicit project root and is preserved across renderer restarts.
The collision document never selects a backend. An unavailable selected driver
fails with its original typed diagnostic and is never replaced by Null.
The project-owned collision/material capture and activation evidence are wired
only after the published project settings dependency is incorporated.

Whole-product native and Sonar workflow job budgets are 120 minutes, matching
the actual cold validation closure. Per-test timeouts, executed suites, coverage
requirements and analyzer rules remain unchanged.

## Explicit application composition

The default Editor remains `--physics null`. Canonical Scene activation is an
explicit startup policy: `HoroEditor --project <absolute-root> --physics canonical`.
When the selected Scene needs cooked resources, also supply
`--cooked-root <absolute-target-root> --cook-target <target-id>`. A Scene without
resource dependencies does not require a cook selection. Neither the presence of
`collision.json` nor a failed Canonical admission selects another backend.

Editor preview transfers its one prepared `RuntimeSceneService` into RuntimeHost;
the application retains its project registry, verified payloads, load service and
native authority until RuntimeHost is shut down. Play receives a separately owned
service through the non-installed `EditorPlaySceneHost` interface. Its factory is
bound to the exact workspace project before asynchronous admission. Starting
retains the existing module registry, and reload waits for preparation to finish.
Stop cancels owned asset work, stops behaviors, revokes native borrows and retires
the Scene before releasing providers. Preview IDs remain below the high-bit
namespace divider; Play IDs start above it. Both admission ranges are checked
before Physics native preparation, rather than relying on the convention alone.

The private Play contract now exposes an optional read-only Scene view instead
of transferring a mutable clone. Internal callers must rebuild; module SDK types
and exported descriptors are unaffected. The existing screen fixed-update seam
supplies seconds, which the application converts once to the nearest nanosecond
for native fixed stages. Loading never uses a simulation tick as progress.

The terminal's existing positional mode/endpoints and five-value gameplay suffix
remain valid synthetic network-fixture modes. Real project activation adds
`--project <absolute-root> --scene source:<registered-project-path>` or
`--scene cooked:<Scene-AssetId>`, with explicit `--physics null|canonical` (default
Null). These are new Scene selectors, not a reinterpretation of an existing Scene
argument. Optional cook selection uses the same paired `--cooked-root` and
`--cook-target` flags. `--load-timeout-ms` bounds startup/travel preparation to
1..60000 ms, default 30000; one absolute deadline and cancellation ancestry cover
source/cook capture and all roles, without resetting for each record. A host that
uses explicit Travel replaces its borrowed load boundary once before the whole
synchronous Travel operation, never between role preparations. Admitted records
retain their original cancellation ancestry, so a later failed load cannot
replace the old active Scene's frame token. Normal
frames never wait for loading. The CLI owns running CPU workers before asset
submission and retains the actual provider until NetworkProductHost teardown.

Each real headless WorldRecord admits exactly its requested nonzero Scene ID once.
Travel creates another record; it does not ask a seeded service to issue local
replacement IDs. The normal CLI creates a single mode host per process and issues
distinct initial role IDs. Other application callers remain responsible for
fresh globally nonreused IDs. Admission retains a bounded set of the two current
plus two replacement role receipts, including any record still retained by a
service, and rejects known cross-role collisions before native staging.

Committed resources come from one resolved `current.json` generation and its
verified manifest/envelopes. The application retains the actual identity, type,
target, source/cache/payload digests and payload bytes, and populates the ordinary
MemoryAssetProvider only from those verified bytes. Cooked `core.scene` assets
use SceneCook's one stable AssetId-to-SceneId derivation and canonical embedded
revision decoder. Source mode uses root-owned native reads and canonical Scene
conversion; its runtime definition revision identifies the captured source
content, not a claimed cooked revision or active generation.

`ProjectPhysicsCapture` now owns the existing exact material/project capture
implementation independently of PrefabCookHost/EditorServices. The former
`PhysicsTriggerVolumeHost` header and convenience target include/link this owner,
so existing callers and symbols retain their contract. The split is needed by
real headless application composition, not to duplicate material decoding or
weaken consumer coverage. Public header ownership, source compatibility and both
headless/Editor target closures must be verified together.

These application changes are source-only until the ticket's explicit validation
receipt records actual builds, runtime cases and external consumer results.

### Published project settings integration

This change targets the published HORO-908 branch, which owns `.horo/collision.json`,
its canonical codec, mutation/CAS authority and collision settings UI. Character
extends that same typed reference inventory: primary/required profiles, required
layers and query intents remain persistent references even when the component is
disabled. Retirement reviews carry exact typed IDs and preserve the old draft on
rejection. Character parsing participates in the decoder's existing recognition
count; it is not classified as an opaque extension or independently rescanned JSON.

The application supplies allocation-free, owner-lane borrowed capture receipts.
Only actual aggregate publication produces Active; rollback/retirement withdraws
that receipt before native storage is released. The workspace copies project
identity only when evidence changes. Preview and Play must agree on exact project,
collision document revision and schema fingerprint; missing or mixed receipts are
Unknown and cannot authorize semantic replacement. Independent generations remain
explicitly attached to their actual owners. Null is reported only for the actual
successfully created Null peer. Receipts are internal and are not SDK capabilities.

Canonical edit preview pins its startup project's immutable registry, schema and
committed assets. The project-open contributor verifies native filesystem identity
before installing another project's derived state. Opening a different project
requires an explicit editor restart with that project's `--project` selection;
it never reuses the previous project's schema/provider. Null authoring keeps its
existing project-switch behavior. New committed generations/schema changes are
adopted through explicit restart/capture, not silent mutation of a live world.

## Prepared placement and exact transform preservation

Initial Character recovery runs while its private world is Prepared. Placement
retains the exact entry lifecycle state across every real overlap callback and
through final controller publication; activation, shutdown, stale generation or
reentrant mutation invalidates the operation. This preserves ordinary Active
spawn/teleport behavior without activating the initial candidate early. Successful
prepared spawn publishes its one initial transform at tick zero; aggregate Publish
activates the world once after every owner is ready.

The general math TRS decomposer may approximate a sheared basis. Scene initial
placement therefore recomposes each decomposed world/local transform and checks
equivalence with the established finite Math tolerance. Nonrepresentable shear
is rejected before any private local transform is assigned or owner publishes.
Mirrored and representable nonuniform rotated parents remain supported; singular
parents fail rather than replacing authored visual scale.

### Initial fixed-tick query handoff

The exact activated Scene-owned Physics/Character pair retains its prepared
Character query descriptor until Physics has produced its first real nonzero
publication. This is only admitted while both completed-tick counters remain
zero and the borrowed Scene/Physics identities match. It creates no Physics
publication, revision, or synthetic fixed tick. Later ticks refresh the descriptor
from the actual Physics publication; progressed, retired, foreign, or replaced
pairs cannot reuse this initial handoff. Static initial support already has no
moving-platform attachment; moving support still requires real captured evidence.

Initial placement retains exact authored local visual scale when that scale
recomposes the same already validated local pose within the established Math
tolerance. This prevents decomposition roundoff from rewriting authored scale,
while retaining existing shear rejection and the shared Math implementation.

### Authored source read profile

The headless source Scene path uses the retained-handle `SourceText` capability
with the Scene decoder's 16 MiB ceiling. Interactive `Text`/`Files` queries remain
limited to 1 MiB; requesting the Scene budget through that query profile is an
invalid request even for a small file. Cooked input retains its existing decoder.
The source operation keeps the original absolute load deadline and checks stop
fences between native transfers. It does not replace handle-relative access with
an absolute-path reopen or accept links and multiple hardlinks.

`ProjectSourceReadLimits` and the appended `IProjectReadFiles::SourceText` virtual
operation require rebuilding platform-reader consumers. Existing restricted
implementations retain their query behavior and return `Unavailable` for source
capture until explicitly implemented. Native composition supplies the real
operation; no Scene data migration, GameModule SDK change or query-limit increase
is involved. Platform ceiling/stop/path regressions and the actual CLI test with
an authored Scene larger than 1 MiB qualify this boundary.
