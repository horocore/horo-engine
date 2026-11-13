# Character movement events and project surfaces

HORO-963 / #963 [CHR-004.2] adds bounded post-commit movement facts and a separate
terminal-state projection. The source contract below has focused macOS arm64 Debug
qualification; it is not evidence of a passing cross-platform matrix.

## Author semantic surfaces

Horo project contract `0.3.0` owns `.horo/surfaces.json`. Its `schemaVersion: 1`
shape is frozen under `.horo/project.json`'s `horoVersion` and
`persistentContract`; it has no independent migration timeline. The catalog's
`projectId` must exactly equal the existing opaque project identity, including
legacy `proj_...` values. Do not replace that identity with a surface or asset UUID.

This example is valid for a project whose actual ID is `proj_example`:

```json
{
  "schemaVersion": 1,
  "projectId": "proj_example",
  "revision": 1,
  "defaultSurfaceId": "11111111-1111-4111-8111-111111111111",
  "surfaces": [
    {"id": "11111111-1111-4111-8111-111111111111", "displayName": "Walkway"}
  ]
}
```

A `SurfaceMaterialId` is a distinct nonzero canonical lowercase UUID. It does not
resolve an AssetId sidecar, renderer material, physical coefficient or media cue.
Names are display metadata and never infer bindings. The document allows at most
256 entries and 128 UTF-8 bytes per display name, within a 256 KiB file and eight
JSON nesting levels. Duplicate IDs/keys, unknown v1 fields, invalid text, zero
revisions and defaults outside the catalog fail validation. Unsupported versions
produce a migration diagnostic before v1 fields are interpreted. Optional gameplay
tags are absent from this wire version.

The append-only `0.2.0` to `0.3.0` migration adopts a missing catalog with the exact
existing project ID, revision 1, an empty `surfaces` array and
`defaultSurfaceId: null`. It infers no surface meaning. Existing valid catalog
bytes are retained; malformed or foreign catalogs reject the candidate. Ordinary
loading never creates or repairs a missing file. New-project staging authors a
real surface UUID and explicitly selects its template entry as the default.

An empty catalog/null default is valid authoring, not proof that semantic movement
is available. Author a surface and select a catalog-valid default before admitting
Character movement that requires it. `LoadProjectSurfaceSettings` retains the
exact source-byte identity and an immutable document. Its canonical fingerprint
includes the selected default; `Capture(mappingGeneration)` returns the retained
catalog and explicit default with a host-assigned nonzero mapping generation.
Project-open retains this capture and rejects source changes before session
readiness, reservation and activation commit.

## Bind authored components

Collider component schema 2 adds optional `surfaceBindings` rows with `slot` and
`surface` fields. The slot names an existing physical-material binding, and the
surface names the captured project catalog. The semantic list is bounded to 64
rows, independently from the legacy physical-material list. Omission means no semantic binding;
schema-1 colliders remain valid only without semantic rows. The existing physical
`materials` list remains authoritative for physical asset identity. Native cooked
triangle-mesh realization remains unsupported. Analytic colliders use one physical
slot each; compound bodies retain their distinct authored collider provenance.
Unsupported multi-slot collider realization fails explicitly.

Character component schema 1 retains its version and adds optional
`collision.defaultSurface`. An explicit value must belong to the captured
catalog. Absence inherits the actual captured project default. Neither missing
project policy nor missing support is replaced by a material-name heuristic.
An absent catalog/default remains explicit
`CharacterSurfaceAvailability::Unavailable` while physical movement continues.
An invalid present default rejects preparation. Neither path fabricates a
semantic ground surface.

This change copies physical material identity/revision/slot evidence. It does not
implement native physical-coefficient realization: friction, restitution, density
and combine-policy application must not be inferred from semantic surface IDs.

## Capture committed facts

The public API is `Horo::Character::CharacterWorld::CaptureEvents`. Call it on the
world's owner thread, outside movement/placement callbacks, using the exact scene
generation and Character world identity. Wrong-thread, busy, stale and closed
requests return their corresponding `CharacterEventCaptureStatus` without granting
access to candidate state. The following fragment uses already-owned host values
`world`, `sceneGeneration`, `expectedWorld` and `lastPublication`:

```cpp
#include "Horo/Physics/CharacterWorld.h"
#include <array>

std::array<Horo::Character::CharacterMovementEvent, 64> events{};
std::array<Horo::Character::CharacterEventTerminalState, 32> terminal{};
const Horo::Character::CharacterEventCaptureRequest request{
    .sceneGeneration = sceneGeneration,
    .world = expectedWorld,
    .afterPublicationRevision = lastPublication};
const auto capture = world.CaptureEvents(request, events, terminal);
```

The array sizes are caller quotas, not world configuration. Read only the first
`copiedEvents` and `copiedTerminalStates` elements when the receipt permits copied
output. No vector, callback registration, consumption operation or VM object is
required. Calls never allocate event storage or mutate simulation.

Facts are `Landed`, `LeftGround`, `HitWall`, `HitCeiling`, `SurfaceChanged`,
`SlideStart` and `SlideEnd`. They compare prior committed state with final movement
once per controller/tick. Sustained wall/ceiling contact and sustained sliding do
not repeat entry events. `SurfaceChanged` requires two grounded semantic IDs with
comparable catalog/mapping provenance; a physical material revision alone does not
create it. Contact impact velocity is optional actual inbound sweep evidence;
a support-only probe does not invent an impact velocity.

For deduplication retain the capture's scene/world identity together with the
event's controller, tick, sequence, state revision, transform publication revision,
kind/ordinal and stream publication revision. `streamPublicationRevision` is the
world event watermark; the event's `publicationRevision` is its transform
publication revision. Advance `lastPublication` to the receipt's
`publicationRevision` only after handling or explicitly accepting loss for that
publication. Repeating the same read does not consume facts. A request whose watermark
equals the current publication returns `NoNewPublication`; a future watermark is stale.

## Handle retention and output limits

`maximumQueuedEvents` bounds the retained journal. Newer ticks take retention
priority. Within a tick, Landed/LeftGround/SlideEnd take precedence over ordinary
hints, followed by canonical controller/kind/ordinal ordering. Capture presents
the retained events in canonical tick/controller/kind/ordinal order.

`droppedEvents` and saturating `cumulativeDroppedEvents` report journal retention
pressure, including eviction of facts a consumer may already have read. They are
not an exact per-consumer missed-event count. `readDroppedEvents` counts unseen
retained facts omitted by this call's event-output quota. An undersized event span
returns a deterministic prefix with `CapacityLimited`; retry with a larger span
before advancing the watermark if that loss is unacceptable.

Every resident controller has a terminal-state copy reserved independently from
the journal, preserving final ground/slide/collision/semantic state. If the caller's
terminal span cannot hold the complete projection, capture rejects before changing
either output span. Terminal snapshots retain their real source tick and revisions:
a no-command tick advances the stream watermark without inventing new contacts.
A failed tick leaves the previously committed journal and terminal states visible.

Copied events and terminal states are historical values. They remain readable
after consumer retirement or world shutdown, but do not authorize new queries,
commands or current-world surface lookup. Live admission still requires the exact
world, controller and captured catalog/mapping generations. Do not reinterpret an
old event through the latest catalog.

No consumer is required for movement to commit. Consumer absence, reload,
backpressure and shutdown cannot change simulation. These events do not own
animation footstep timing or choose Audio/VFX assets; application-owned post-commit
consumers retain their own binding revisions and cosmetic policy.

## Build and qualification boundary

The public gameplay contract advances to SDK ABI/fingerprint 25; rebuild native
gameplay modules and actual generated descriptor fixtures against the matching SDK.
Do not load an older module by reinterpreting its layout.

Local macOS arm64 Debug qualification ran the owning Physics and Scene test
binaries, the catalog/migration/project-open/project-creation binaries and four
executable public consumers. The six test binaries executed 39,050 assertions
with zero failures, errors or skips in the recorded runs. Allocation and ordering
oracles include actual native queries, event publication and bounded retention.
The affected editor and headless composition archives also compiled; an archive
build does not prove an observed editor or packaged-product launch.

This guide claims no Windows/Linux, GPU, performance or observed editor
qualification. Local Sonar is waived and Codacy was not rerun. The feature adds no
avoidance, dynamic-body pushing, footstep scheduler or physical-coefficient
realization; native cooked triangle-mesh activation remains unsupported.
