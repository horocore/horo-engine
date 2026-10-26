# Animation preview session foundation

The implementation of [HORO-488](https://horo-engine.atlassian.net/browse/HORO-488)
([ANI-005.1 / #488](https://github.com/horocore/horo-engine/issues/488)) adds
`HoroEngine::AnimationEditorModel` and `Horo/Editor/AnimationPreviewSession.h`.
The transport target requires no renderer, editor GUI, gameplay world, file access
or audio runtime. `HoroEditorServices` additionally owns the new
`AnimationGraphDocument` authoring contract.

## Readiness evidence

On 2026-10-25 the GitHub issue was open in **M3 — Alpha**. Its native blocking
dependencies #15 (document persistence audit), #453 (skeleton assets) and #458
(clip assets/sampling) were all closed. The corresponding implementations and
tests are present on main `d00a6afa2055f80c2dd672e7e19a2d75c228a2cc`.
The open PR inventory contained no implementation for #488. Jira's linked
dependency statuses lag GitHub; this work does not rewrite those other tickets.
Project board field access was unavailable, so no exact `Roadmap State = Ready`
claim is made. Technical readiness was checked from dependencies and current code.

## Ownership and integration

Create one session on the document's owner thread. Bind a complete immutable
`AnimationClipAsset` with the source document's nonzero revision. Successful bind
increments the session generation, resets the playhead and pauses playback.
Capture `State().generation` after binding and pass it with every transport call.
Commands retained by an old view or asynchronous preparation fail after replacement,
even when the replacement uses the same asset and source revision.

The source document remains the only owner of dirty state, save acknowledgement,
history, autosave and recovery. Transport does not edit source or mark it dirty.
The host owns preview scene/mesh/camera preparation and provides compatible pose
storage to the canonical Animation sampler. No new Animation clock, traversal
algorithm, marker stream or root-motion producer is introduced.

`Seek` samples an exact playhead and pauses after success. `Advance` traverses only
while playing; paused calls refresh the current sample without consuming time.
Both operations validate the ordinary production sampler bindings, including
skeleton/clip generation, output bounds and cancellation. A failed sample or
traversal preserves the entire transport state and output pose. Loop endpoints
remain visible when scrubbed; resuming uses the canonical normalized loop phase.
Once playback pauses at its terminal boundary; seek explicitly resets that cursor.

Preparation/replacement may release an old clip allocation. Steady transport and
all hot failure paths use allocation-free `AnimationClipOutcome` and caller-owned
pose storage. Convert failures to owned diagnostics only at the host control boundary.
Close releases the clip and permanently closes admission, without teardown events.

## Remaining ticket scope

These models are a reviewable foundation, not completed ticket acceptance. Actual
workspace source publication/autosave/recovery routing, external-file observation,
isolated preview scene composition, mesh/skeleton selection, camera and GUI routing
still need integration. No existing caller migrates yet; future UI clients should
use these owners rather than duplicating graph mutation or clip traversal.

## Animation graph document ownership

`AnimationGraphDocument` accepts a host-issued Asset `DocumentIdentity` and a
detached typed graph. Animation's existing migration, topology validation and
source codec remain the sole authority. Documents never resolve runtime clips,
compile a live graph instance or select a backend. Supported source migration is
explicit at Open/Reload and leaves the document dirty until a durable save.

Coalesced commands insert, replace or remove stable graph parameters/definitions
and change the entry. Validation applies to the complete final graph, allowing
entry/reference replacement within one atomic transaction. Intermediate command
payloads are checked against the captured collection budgets before copying.
Successful publication reserves source bytes and semantic history first; invalid,
cancelled, oversized and allocation-failed edits preserve the complete prior root,
revision, dirty state and undo/redo cursor.

History uses the shared `SemanticDocumentHistory`, retaining only changed typed
parameters/definitions and entry identity. It retains at most 64 transactions and
8 MiB of charged semantic storage, including nested pins, masks, states,
transitions, conditions and string capacities. Project policy may lower these
bounds. Undo/redo advances the notification revision while restoring the exact
semantic state ID. Canonically equivalent no-op commands preserve revision and
redo; a successful divergent edit discards redo.

Dirty status compares canonical authored bytes to the exact saved capture, rather
than counting edits. Manually restoring the saved values is clean even when that
transaction receives a new state ID. Byte comparison happens at edit/replay/save
boundaries; the frame-facing dirty query is constant-time. The owner retains one
saved immutable root independently of bounded history; callers bound their own
detached snapshot retention.

Snapshots own immutable typed data and canonical source bytes through close and
destruction. Hosts write `Snapshot().SourceBytes()` using `PublishAssetSource`
under the shared project and per-source leases; they call `MarkSaved` only for a
successful **Durable** receipt. Reconciliation-required publication cannot mark a
document saved. A late save of an older capture leaves newer edits dirty. Private
owner tokens reject captures from another document even if a host accidentally
reuses an identical visible identity. Reload establishes a new capture barrier
and clears history only after complete replacement validation.

Autosave uses the same canonical source bytes inside `DocumentRecoveryService`;
it does not call `MarkSaved` or write the canonical source. After explicit recovery
acceptance, the host verifies the exact envelope/session/source identity and calls
`ApplyRecovery` with its payload. That method uses the existing bounded source
parser and applies one reversible semantic edit. It does not authenticate a recovery
envelope or grant disk-write authority. Host publication and recovery routing are
remaining integration work, not implicitly activated by constructing this model.

All document calls remain on their owner thread. Detached snapshots may cross
threads after synchronized handoff. Preview revision and source revision stay
separate; seek/play/pause never mutate this source or its history.

The document allocation-failure regression exposed an existing codec failure:
`ordered_json` container destruction can allocate a flattening stack inside a
non-throwing destructor. A failed allocation there terminated the process instead
of propagating `std::bad_alloc`. The Animation-owned encoder now writes bounded
canonical text directly, using the existing JSON library only for primitive
spelling/escaping. It preserves field/dependency order, signed-zero normalization,
source version and byte/token/depth policy. The decoder builds expected dependency
identities without a second JSON tree, uses stable map nodes while parsing, and
empties its admitted-depth parsed tree before destruction, without allocating.
String tags compare borrowed text directly, avoiding JSON scalar comparison's
non-throwing wrapper around an allocating temporary string.
Both editor edit and recovery transactions
are tested by injecting a failure at every allocation until the operation succeeds.
This is an exception-safety correction, not a new source format or codec authority.

`HoroEditorServices` now directly and publicly declares `AnimationGraphSourceCodec`.
Its existing clients retain source compatibility and must rebuild to consume the
new public header. The public-header ownership registry and generated consumer
include the new contract.

## Verification

`HoroAnimationPreviewTests` exercises production-validated clips through loop,
reverse, Once and PingPong playback; exact endpoint seek; cancellation and stale
bindings; failed-output atomicity; replacement/close; independent sessions; and
allocation observation during transport. The generated
`HoroAnimationEditorModelPublicHeaderConsumer` checks the staged public boundary.
These are headless model checks, not GPU or actual preview-scene qualification.

`HoroAnimationDocumentTests` covers canonical codec interoperability, semantic
undo/redo, atomic reference replacement, no-op/invalid/stale/cancelled commands,
late and cross-session save acknowledgement, migration, external reload,
reversible recovery, limits, owner-thread rejection, retained snapshot lifetime
and injected allocation failure through the complete edit transaction.
Existing `HoroAnimationApiTests` additionally qualify codec, state-machine and pose
composition round trips and exact parser/encoder token and depth boundaries.
