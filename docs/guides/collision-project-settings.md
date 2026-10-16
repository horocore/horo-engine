# Project collision settings

`HORO-908 #908 [PHY-008.2]` implements the Project authority required by ADR-086.
The one portable source is `.horo/collision.json`; it is not an editor preference,
Physics registry, backend selector or runtime fallback.

## Authoring workflow

Open **Settings > Project Collision** in an admitted project workspace. The form
copies its editable projection on opening or an explicit reset/replay/commit, never
on every frame. Names and descriptions are presentation keyed by persistent typed
UUIDs. A rename retains identity and semantic fingerprint. Layer/profile/channel
creation requests a fresh ID only for that explicit command; a newly created layer
gets one explicit response for each enabled pair, and a new channel gets one
explicit response in every profile. The initial response and new-profile layer
are visible choices. The default profile applies only to explicit authoring
creation; it never repairs a missing runtime reference.

Matrix controls edit unordered layer pairs once, so the response is symmetric.
Profile controls edit its exact layer, simulation/query/event flags and every
query response. Canonical `NormalizedCollisionSchema::Create` still admits the
whole graph: incomplete tables, unsupported features, cross-type IDs, duplicate
IDs, illegal participation and missing references reject. Limits remain 64 enabled
layers, 64 enabled query channels, 1024 profiles and 4096 retained definitions.
Deleting a layer does not silently change profiles which still refer to it: assign
those profiles to an explicit remaining layer before review can succeed.

**Remove from candidate** changes only the transient form. **Review** captures all
registered owners, physical source identities and the exact active authored Scene
opening/revision/state before **Commit** can publish a session history root. The
impact list includes exact typed collider profiles and cooked trigger capture
proof. Referenced profiles remain blocked until their owning typed references are
migrated. Closed/unloaded registered Scenes use the one canonical Scene decoder.
Prefab placements, behavior/custom payload owners and unhandled component owners
are enumerated as unsupported and block deletion. They are never counted as zero
references or searched with an editor JSON/text parser. Unsupported owners need a
typed provider or an explicit owning-source repair; this feature does not claim
automatic cross-document or opaque-payload migration.

The app retains the real registry build status and exact published revision.
Complete status must match the current registry; failed/degraded refresh, missing
index/sidecars, new assets, deleted sources and stale physical bytes invalidate the
review. Use the Content Browser refresh after source changes. A successful strict
Scene parse does not grant runtime trust or replace separate Scene graph admission.
Physical-source opaque-owner evidence survives use of the active canonical Scene
snapshot. A malformed, duplicate-key or unsupported-version source is an error,
not an empty ownership inventory.

Undo/redo rechecks the same source, inventory, Scene and host evidence. All roots
and history capacity are prepared before the final freshness fence and no-fail
owner swap. Closing a workspace revokes the exact opening before retiring Scene
owners; a settings modal retains only a weak session and cannot adopt a reopened
project. Save Copy preserves the active owner. Save As/reload blocks dirty collision
settings; clean owner closure happens only at the actual Scene replacement boundary.

## Durable publication and lifecycle

**Save collision document** is explicit and separate from local editor settings.
It acquires the shared Project mutation lease and a source publication lease,
rechecks the complete physical membership using the owning Assets read-only scanner
and reviewed source identities, writes a complete sibling temporary and
atomically replaces the single collision file. Revisions are consecutive and the
last accepted exact file identity supplies CAS. All receipt storage is prepared
before replacement. A post-replacement directory-sync failure advances source
identity but keeps dirty state, permitting an explicit retry without overwriting
an unobserved source. Preparation failure changes no durable bytes or session root.

The guarantee is atomic publication of this one document under cooperating Project
mutation ownership. It is not an atomic transaction across Scene files, sidecars
and project settings, nor protection against an uncooperative external writer
changing another file after its final identity check. Such writers must be
reconciled by a new review; no multi-file atomicity is claimed.

Runtime evidence is explicit: Unknown, No Physics, Inactive or Active. Unknown
blocks semantic replacement while retaining pending drafts. No Physics is derived
only from the app's explicit Null selection; absence of a world does not imply
Inactive. Active feedback names the captured project ID/document revision,
semantic fingerprint and host-issued generation. Stop Play before semantic edits;
saving never updates an existing world or releases its immutable capture. An active
generation with different semantics requires Stop/restart. Backend availability,
selection and owner publication remain host responsibilities. No GUI dependency is
required to load or capture the document.

New-project staging writes a validated ordinary initial template and publishes it
with the existing atomic project promotion. Existing projects with no collision
file get a typed missing-document diagnostic; restore or explicitly author their
portable document instead of silently installing a runtime default. Editor project
open validates the document in preparation and rechecks its exact file identity
before publishing registry state and entering the workspace.

## Public contract and migration

`HoroEngine::ProjectCollisionSettings` owns
`Horo/Application/ProjectCollisionSettings.h`. `LoadProjectCollisionSettings(root,
expectedProjectId)` returns the immutable settings plus exact source identity.
`settings.Capture(nonzeroGeneration)` returns the existing
`PhysicsWorldSimulationBinding`; it does not discover a backend, install a world,
resolve an ambient profile or claim activation. Hosts retain that capture until
the actual owner retires it. Physics material/cook provenance remains the separate
published trigger capture contract; loading this document does not fabricate it.
The Editor store/session headers are owned by `HoroEditorServices` and the existing
modal draft/presentation headers remain owned by `HoroGui`.

`SceneSourceDocument` gains trailing transient `unsupportedComponentOwners`, sorted
and deduplicated by the existing owning decoder after object admission. Callers
which perform destructive reference work must inspect this evidence. The writer
and all valid schema-1 canonical bytes are unchanged; opaque evidence is not
serialized. Scene Cook consumes the same decoder recognition result and rejects every unhandled
component key, including null or disabled future payloads, rather than dropping it
from source-free runtime data. Its existing authoring-only trigger rejection remains.
The same canonical decoder now rejects duplicate object keys and
nesting at the existing Foundation guard's 64-level boundary, including ignored
fields. Ambiguous last-wins input must be repaired before use; no second permissive
decoder exists. Completed-DOM child cleanup avoids flattening scratch growth in
supported allocation-qualification compositions; checked-iterator MSVC Debug can
still allocate standard-container proxies inside `noexcept` constructors. Persistent
OOM cleanup inside the upstream partial parser is outside this guarantee.

Source closure: the game-module SDK exports Gameplay/Foundation/Assets/Runtime/
Physics/UI/Navigation boundaries, not `HoroSceneSource`, `HoroEditorServices`,
`HoroGui` or the new Project settings target. `SceneSourceModel`, GameRuntimeContext,
module vtables, registrations, descriptors and extension ABI headers remain
unchanged. SDK21 is retained; this is an engine host/editor rebuild migration,
not a new module layout. Public-header consumers and actual app targets remain
required validation; this source audit alone is not executable ABI proof.

## Qualification scope

The headless collision suite covers strict codec/load, stable identity, captured
schema lifetime, real native source CAS, exact reference and active-generation
fences, cached projection access, history rollback at every observed allocation
failure and real post-replacement uncertainty. Canonical Scene tests cover valid
wire preservation, decoder-owned opaque owners, active-snapshot override, malformed/
future/duplicate/deep input and one-shot allocation failures. GUI tests exercise
the actual modal draft commands and weak lifecycle plus workspace Save Copy,
rejected Save As, accepted Save As and destruction. Existing project creation tests
load the actual staged collision document. GUI model tests do not substitute for
manual narrow/localized visual interaction checks or native Physics activation.

## Allocation qualification

The three global single-fault sweeps run every measured allocation position in
supported production STL compositions. Windows checked Debug iterator proxies
allocate in noexcept container operations, so these exact probes use the existing
Scene allocation admission guard and run mandatorily in a consistently configured
Windows Release engine/test composition. Ordinary Debug functional, lifecycle and
cached-projection zero-allocation cases remain enabled. Release discovery and
JUnit validation require all three exact cases, zero skips, and nonzero equal
`measured`/`completed` counters. No positions are sampled and case timeouts remain
90 seconds. This does not promise persistent-OOM safety for an upstream partially
constructed JSON parser DOM.

Path containment admission also remains fallible: allocation failures propagate
to the caller's capacity boundary before any settings/history publication.
Ordinary filesystem equivalence lookup failures retain the existing canonical
and lexical containment fallback. The direct containment and complete commit
sweeps qualify every measured allocation position; neither exception swallowing
nor a reduced sweep substitutes for rollback evidence.
