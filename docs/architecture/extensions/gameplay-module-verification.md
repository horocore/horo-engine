# Gameplay Module Verification

## Purpose

This document collects regression and contract coverage required for project-owned gameplay module behavior across module loading, authoring, runtime integration, hot reload, and editor workflows.

## Module Boundary Tests

Required tests cover:

- SDK-only game module build
- one primary module enforcement
- module factory boundary-version/fingerprint validation
- `CreateGameModule()` and `DestroyGameModule()` ownership across the dynamic
  library boundary
- descriptor data is copied or has an explicitly declared module-owned lifetime
- failed startup unwinds only acquired startup scopes before destruction
- module startup failure unwind, including `Stop()` after failed `Start()`
- game-owned ID namespace validation
- registration uniqueness and dependency validation
- registration context denying scene, editor, renderer, and global service
  access
- service descriptor registration through `GameRegistrationContext::services`
- service lifetime ordering for project-scoped and scene-scoped services
- capability thread-affinity validation for jobs and scene access
- module-owned job cancellation
- absence of editor/backend dependencies
- game-namespaced error code stability and host-adapter mappings

## Generated Descriptor Bundle Tests

Required tests cover:

- generated descriptor bundle fingerprint and descriptor-revision mismatch
  rejection
- generated behavior bundles are complete snapshots; stale or partial bundles do
  not leave removed behavior descriptors active
- descriptor metadata is copied into host registries rather than retaining spans
  into generated/module-owned storage
- native factory bindings are invalidated before module stop, destroy, or dynamic
  library unload
- removed behavior descriptors preserve existing scene payloads as unknown
  behaviors until a descriptor returns or migration runs
- behavior field migration descriptors apply stable property-ID migrations
  across `schemaVersion` changes and reject missing migration paths
- duplicate or renamed native behavior IDs produce typed diagnostics rather than
  silently replacing existing descriptors
- public game module registration does not expose a manual object-attached
  behavior registration path

## Behavior Authoring Tests

Required tests cover:

- editor project open starts or connects to the project build session without
  mutating runtime scenes
- background build diagnostics preserve the previous valid module on failure
- object-attached behavior registration, serialization, inspector metadata, and
  attachment to scene entities
- native, scripted, and visual-authored behaviors all attach through the same
  `BehaviorComponent` and descriptor model
- `HORO_BEHAVIOR` native annotation scanning generates behavior descriptors
  without one manual `Register()` entry per native behavior
- project-panel Create Script Behavior scaffolds source and sidecar files,
  refreshes descriptors, and exposes the behavior in Add Behavior/Add Component
  after successful validation
- script asset scanning discovers behavior declarations and generates
  descriptors without a parallel manual registry entry
- script display-name changes preserve stable `BehaviorTypeId`; identity changes
  require an explicit migration that updates or validates all references
- script source `id` mismatch against the sidecar `BehaviorTypeId` produces a
  typed diagnostic and does not activate a descriptor
- Attach to Selected Object creates an undoable document command and the object
  starts using the script behavior at the next play/live-preview safe point
- editor property changes to behavior fields go through undoable document
  commands
- authoring metadata remains declarative and editor-neutral

## Script Runtime Conformance Tests

Required tests cover:

- script reload preserves compatible state and keeps the previous valid script
  active on incompatible reload
- compatible reload tests cover function-body changes, editor-only metadata
  changes, field addition with defaults, and schedule revalidation
- incompatible reload tests cover field type changes, missing migrations,
  `BehaviorTypeId` edits, new required dependencies, and write-access changes
- scripting runtime conformance when scripting is introduced
- scripts use the same registration, runtime capability, scene mutation,
  scheduling, diagnostics, and hot-reload safe-point rules as native gameplay
  code

## Visual Scripting Tests

Required tests cover:

- invalid visual scripting graphs produce diagnostics and cannot activate
  runtime behavior
- visual graph discovery validates node schemas, pin types, dependency cycles,
  missing assets, and declared component access before descriptor activation

## Scene Lifecycle And Scheduling Tests

Required tests cover:

- behavior lifecycle ordering for create, enable, start, fixed update,
  presentation update, disable, destroy, scene unload, and module stop
- created-disabled behavior runs `OnCreate()` but delays `OnEnable()` and
  `OnStart()` until first enable
- `OnStart()` runs once per instance lifetime while `OnEnable()` may run multiple
  times
- spawned prefab and additive scene activation preserve the documented
  `OnCreate()` before first-enable/`OnStart()` ordering
- behavior/system ordering through `after`/`before` dependencies and cycle
  rejection
- schedule node namespace validation rejects duplicate engine/game/generated
  node IDs
- generated behavior runners schedule one batch per `BehaviorTypeId` per phase
- stable scene/entity iteration is used inside a behavior batch unless the
  descriptor declares order irrelevant
- system phase/access validation, including incompatible writes and ordering
  cycles
- phase-specific behavior access rejects presentation-phase simulation writes
- behavior declared access enforcement and structural command buffering
- input action registration, binding conflict diagnostics, and fixed-tick
  consumption from behavior context
- fixed-step behavior under different render rates
- cross-scene reference resolution returns typed stale/unloaded errors instead
  of exposing dangling entity handles

## Runtime Integration Tests

Required tests cover:

- game-owned asset type import, cook, authoring metadata, async load, and hot
  reload handle invalidation
- game-owned asset type descriptors and extension-package importer/cooker
  contributions reject duplicate type IDs
- file-extension importer conflicts require explicit priority, user selection, or
  project policy rather than load order
- component serialization and conversion
- scene-scoped system instance destruction before module stop and unload
- behavior dependency injection rejects missing services or capabilities before
  `OnCreate()`
- dependency lifetime tests verify required services outlive dependent behavior
  instances during normal scene unload and module stop
- behavior-held service handles are invalidated before scene unload completes
  and cannot be used after dependent service teardown
- play-session stop and service destruction

## Hot Reload And Degraded Mode Tests

Required tests cover:

- native reload precondition and restart fallback
- the default module reload contract returns restart-required
- external behavior and system runtimes pin their exact native generation and
  block retirement until those leases are released
- cancellation before module quiescence and bounded module snapshot restore
- throwing behavior activation and cleanup callbacks remain contained while every
  partial factory instance is released
- behavior snapshot matching by stable instance/type identity without replaying
  `OnStart` for an established instance
- preservation, unload, and shadow-load restoration of the previous native
  artifact
- rollback artifact cleanup on success, failure, early exit, and ownership transfer
- native candidate and rollback activation reuse the exact captured in-memory Lua
  generation even when source files change during the transaction
- candidate rejection followed by rollback against the unchanged runtime scene
- failed retirement quarantines source watching, refresh, reload, and new Play
  admission until workspace teardown
- iteration tier tests verify fast/medium/slow reload behavior and fallback
  restart diagnostics
- live preview reduced-scope activation rejects behaviors or systems whose
  required capabilities are unavailable
- play-in-editor clones authoring state, isolates runtime mutation, and restores
  authoring state on stop
- live preview uses runtime-safe isolation and never writes directly to the
  authoring document
- scene open in degraded mode preserves unknown game-owned component and
  behavior payloads when the module is missing or incompatible

## Future Extension Tests

Required tests cover:

- future `behavior.provider` packages remain disabled until the full generated
  descriptor bundle, trust, `runtime.participate`, fingerprint, lifetime, and
  safe-reload contract is implemented
- future networking/save/memory extension tests preserve stable descriptor and
  module-boundary invariants when those systems are introduced

## Related Documents

- [Gameplay Module Overview](./gameplay-module.md)
- [Gameplay Module Boundary](./gameplay-module-boundary.md)
- [Gameplay Behavior Authoring](./gameplay-behavior-authoring.md)
- [Gameplay Runtime Integration](./gameplay-runtime-integration.md)
- [Game Project Testing](../delivery/game-project-testing.md)

## Replication Declaration Qualification

`HORO-1129` / [NET-003.10] qualifies the descriptor/declaration contract through
`HoroNetworkApiTests`, `HoroGameplayRuntimeTests`, `HoroLuaBehaviorTests`,
`HoroGameModuleHostTests`, and `HoroGameplayReplicationPublicHeaderConsumer`.
The native and Lua declarations use the same stable schema, field and codec IDs.
Source paths, sidecar paths, component categories and display names may change
without changing their canonical schema fingerprint or encoded field values.

The v1 fingerprint input uses big-endian fixed-width integers, 64-bit lengths
for text/collections, identity-sorted schemas/fields/tombstones and the domain
`horo.network.replication-schema-set.v1`. The fixed reference vectors below were
encoded independently with Python `struct` and hashed with `hashlib.sha256`;
the C++ tests compare against literal digests, never compute the expected result
by calling the production fingerprint implementation.

| Declaration | Canonical bytes | SHA-256 (without the display prefix) |
|---|---:|---|
| Schema 42, owner `game.tests`, version/window 1.0, required owner-only number field 7 (type 1, codec 2, 8 bytes/1 element) | 137 | `4baf64ed25af9cc8f64b622de9c6eb3f089244456252553b637a5787a15f72f7` |
| Same schema at 1.1, window 1.0–1.1, plus optional always boolean field 9 (type 3, codec 4, introduced 1.1, 1 byte/1 element, canonical false default) | 175 | `c80416267e305c91974697b75e721236450d060e07b2b2fb038cd7f6908cced7` |

Both vectors use authority-server-only writes, no custom conditions and no
tombstones. Cross-platform CI runs the same literal assertions on Linux/GCC,
macOS/Clang and Windows/MSVC. The compatibility qualification checks admission
at both ends of an explicit minor-version window and rejection outside it.
Malformed metadata, duplicate identities and failed script reloads retain their
existing focused regression cases. Missing native codec code and conflicting
codec contributions fail freeze without publishing a generation.

The native module restart test first proves codec usability while an external
generation lease pins the code after its host owner is released. It then releases
all old module/replication leases before loading a replacement, comparing only
copied digest, payload and owner evidence. This qualifies the explicit restart
boundary; it does not assert operating-system loader residency or hot reload
while external code leases remain admitted.

The public gameplay consumer uses only the declared GameplayLua dependency
closure. Compile assertions pin the Horo owner/schema/schedule types; local
poisoned-token probes reject transport/backend interfaces in the declaration
include closure, and concrete transport/Lua headers must be unavailable. The
existing GameplayApi dependency on NetworkApi still makes other backend-neutral
NetworkApi headers reachable: this qualification does not claim that the entire
transitive transport header surface is inaccessible.
