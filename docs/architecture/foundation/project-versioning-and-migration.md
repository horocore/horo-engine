# Project Versioning And Migration Architecture

## Status

MIG-001A–E, the project-settings adoption slice MIG-001F-01, and prefab
migration adoption PFB-001.8 are implemented
and locally verified on macOS/AppleClang. Version/compatibility inspection,
structured migration definitions, catalog generation, deterministic planning,
constrained pipeline execution, verified dry-run, shared project mutation
locking, durable journaled publication, content-addressed recovery contracts,
canonical history, bounded committed cleanup, automatic recovery, real
project-open orchestration, typed Welcome-card compatibility inspection, and
structured migration lifecycle logging exist. Future subsystem/package adoption
remains under MIG-001F; cross-platform hardening remains under TST-001.

## Purpose

This document defines how a Horo Engine release recognizes, upgrades, validates,
and opens a project created by an older Horo Engine release.

The migration experience is automatic for the project user. Opening an older
project does not require choosing migration steps, locating intermediate engine
versions, editing files, or approving individual transformations. HoroEditor
builds and executes the required migration chain inside the ordinary
`ProjectLoading` route.

Automatic does not mean unsafe or silent. The editor visibly reports that an
upgrade is running, preserves the original project until the replacement is
fully validated, records diagnostics, and can recover from cancellation,
failure, or process termination.

## Core Decisions

- One semantic `HoroProjectVersion` governs all Horo-owned durable authoring
  data inside one project.
- `HoroProjectVersion` advances with the Horo Engine release version. Core scene,
  prefab, project settings, input, material, graph, and asset-sidecar formats do
  not maintain unrelated independent schema-version timelines.
- The authoritative project version is stored once in `.horo/project.json`.
  Project-owned documents are interpreted under that version.
- `projectVersion`, chosen by the game developer for their product, remains
  independent from `horoVersion`.
- In-memory models such as `RuntimeSceneDefinition` carry no persistent-format
  version.
- Derived state is rebuilt, not migrated. This includes asset indexes, caches,
  cooked artifacts, shader caches, thumbnails, and build outputs.
- Engine installation and update activation never mutate user projects.
  Migration begins only when a project is opened or an explicit headless project
  migration operation is invoked.
- Project opening always performs a bounded read-only compatibility inspection
  before acquiring mutation authority.
- An older project with a complete migration chain is upgraded automatically in
  `ProjectLoading`; the user is not asked to design, select, or confirm steps.
- A newer minor/major project, or a project with a different or unverifiable
  persistent contract, is never downgraded. An older engine rejects it without
  mutation and identifies the required Horo version. A higher patch in the same
  release line is the sole exception: it may open only when its embedded signed
  compatibility proof resolves to the engine's exact persistent contract. The
  older engine never lowers the project marker.
- The project root version is published last. A project is never declared
  current before every durable transformation and cross-file validation succeeds.
- Every minor or major Horo release that changes the persistent contract ships a
  migration definition chain from its supported predecessor versions or a verified
  checkpoint migration that covers them.
- Patch releases within one `major.minor` release line are mutually project-file
  compatible and may not change the persistent contract.
- Every release manifest declares a bounded `minimumMigratableVersion`; support
  does not silently grow to include all historical project formats forever.
- Persistent-model changes without a generated or explicit migration decision
  fail the release pipeline.

## Current Production Baseline

Horo `0.1.0` is the current development release. The frozen `0.0.1` descriptor
remains unchanged and is the first supported predecessor. The automatically
discovered sequential definition
`core.project_settings.compression_defaults` upgrades `0.0.1` projects to
`0.1.0`: missing `settings.assetCompression` and
`settings.textureCompression` values become `lz4` and `bc7`. Existing values
are retained only when they belong to the frozen whitelists
`lz4 | none | zstd` and `bc7 | bc5 | astc | none` respectively.

The definition validator owns these transformation postconditions. The generated
`0.1.0` target validator separately checks complete root metadata, target
version/contract binding, project settings, and migration history at the
publication boundary. Transaction finalization starts from the pipeline's
migrated `project.json` tree and overlays only transaction-owned root fields;
it never reconstructs the root from the pre-migration document.

The same definition discovers canonical `.prefab` sources and their
`.prefab.horo` identity sidecars as distinct document families. It validates the
sidecar-owned `core.prefab` `AssetId`, replaces legacy prefab-local `prefabId`
metadata with that identity, advances the prefab's unified project version, and
then converts legacy scene `sourcePath` or `prefabId` references to
`sourceAsset`. Prefab sources are transformed before scenes, unknown JSON-owned
component payloads remain present, and a missing, duplicate, future-version, or
conflicting identity fails the unpublished candidate rather than mutating the
project.

## Version Model

### Canonical semantic version

```cpp
struct HoroVersion
{
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
};

using HoroProjectVersion = HoroVersion;

struct EngineReleaseVersion
{
    HoroVersion value;
};

struct ContractBaselineVersion
{
    HoroVersion value;
};
```

The engine release manifest and project metadata use the same canonical
`HoroVersion` value. For a public release `2.3.1`, the migration target written
to a successfully opened project is exactly `2.3.1`; a second independently
advanced "file schema version" does not exist. The project-domain alias exists
to make API intent readable, not to create another counter.

The persistent representation is canonical SemVer text without build metadata:

```json
{
  "horoVersion": "1.7.0",
  "persistentContract": "sha256:4c8d...e291",
  "compatibilityProof": {
    "release": "1.7.0",
    "contractBaseline": "1.7.0",
    "decisionHash": "sha256:b729...18fc",
    "signature": "base64:MEUCIQ..."
  },
  "migrationHistoryHead": "sha256:78a1...0b42",
  "projectVersion": "0.4.0"
}
```

`horoVersion` selects the project migration chain. `projectVersion` is authored
game/product metadata and never selects an engine migration.

`persistentContract` identifies the release-generated persistent descriptor set.
It is not another version and cannot independently select a migration. Because
the exact release marker is provenance rather than a descriptor, the generator
validates `project-contract.json.horoVersion` against its release directory but
excludes that marker from canonical contract hashing. Therefore byte-equivalent
descriptors in `1.0.0` and `1.0.1` produce the same contract identity, while any
real patch-line descriptor drift fails catalog generation. Because
project metadata is user-editable, a bare hash is not authenticity proof. The
embedded `compatibilityProof` is the signed release decision for the exact
`horoVersion`; it binds that release, contract baseline, contract hash, and
migration-definition-set hash. An older patch can verify this envelope with its
trusted Horo release-signing roots without having the future patch in its frozen
registry.

`EngineReleaseVersion` and `ContractBaselineVersion` are strong domain types,
not aliases. The compiler must reject accidental assignment or comparison
between an exact engine release and the engine release that established a
persistent-contract baseline. `migrationHistoryHead` anchors audit provenance;
it does not participate in compatibility or chain selection.

The release pipeline freezes one `HoroProjectVersion` for the release. Local
commit hashes, compiler versions, build numbers, and machine identifiers do not
change project compatibility. Development builds use the compatibility target
declared by their release line and must not invent a new project version per
build.

### One version, one project state

The project root is the compatibility authority:

```text
MyGame/
  .horo/project.json   horoVersion = 1.7.0
  assets/scenes/       interpreted as Horo project 1.7.0
  assets/prefabs/      interpreted as Horo project 1.7.0
  assets/materials/    interpreted as Horo project 1.7.0
  assets/**/*.horo     interpreted as Horo project 1.7.0
```

Core project documents do not advance independently. This avoids a matrix in
which a project is simultaneously project-format 3, scene-format 8,
asset-sidecar-format 2, and input-format 5.

The signed compatibility proof, recovery records, and transaction journals may
repeat source or target Horo versions so they can bind evidence or validate
without trusting a partially migrated root. Those copies are verification and
recovery evidence, not independent schema authorities.

### Version semantics

- A major release may contain incompatible authoring-model changes.
- A minor release may add compatible authoring capabilities or transformations.
- A patch release may not change persistent descriptors, authored semantics, or
  migration requirements. If a proposed patch needs such a change, it is
  released as the next minor version instead.
- Every patch publishes a signed compatibility decision proving that its
  persistent descriptor and migration-definition-set hashes match the release
  line.
- Editors in the same `major.minor` release line accept projects marked by any
  patch from that line, including a higher patch. This is safe because the
  persistent contract is identical across the line.
- A newer patch may advance the root marker to record the highest Horo release
  applied to the project. An older compatible patch may edit the project but
  never lowers that marker.
- Opening a project from an older minor or major version runs the registered
  migration chain before creating a writable session.
- Build metadata does not participate in ordering or migration identity.
- Pre-release compatibility is isolated from stable projects. A stable editor
  does not treat a project last migrated by an incompatible pre-release as a
  stable equivalent merely because the numeric core matches.
- Two builds advertising the same public `HoroVersion` must carry identical
  persistent descriptors and migration-definition-set hashes. A durable format
  change requires a new engine release version; it cannot be slipped into a
  rebuild of an existing version.
- Patch compatibility does not create a second schema counter. The exact
  `horoVersion` remains useful provenance, while `major.minor` equality is the
  release-line compatibility rule enforced by immutable descriptor hashes.

### Exact release compatibility decisions

Patch compatibility and persistent migration are separate registries.
`ReleaseCompatibilityRegistry` contains one exact, signed decision for every
published engine version:

```cpp
struct ReleaseCompatibilityDecision
{
    EngineReleaseVersion release;
    ContractBaselineVersion contractBaseline;
    PersistentContractHash contractHash;
    MigrationDefinitionSetHash migrationDefinitions;
    CompatibilityDecisionKind decision;
};
```

Example:

```text
Engine 1.0.0 -> baseline 1.0.0, contract C1
Engine 1.0.1 -> baseline 1.0.0, contract C1
Engine 1.0.7 -> baseline 1.0.0, contract C1
Engine 1.1.0 -> baseline 1.1.0, contract C2
```

Each decision is generated as a release artifact such as
`releases/v1_0_7/project-compatibility.json`, signed, packaged with the engine,
included in the generated release-compatibility catalog, and passed to registry
construction by application composition. The release pipeline
rejects a patch decision whose descriptor or migration-definition-set hash
differs from the frozen release-line contract.

When an editor creates a project or advances its marker, it copies the signed
decision envelope into `project.json` as `compatibilityProof`. The exact registry
resolves releases known when the engine was built. For a higher unknown patch in
the same `major.minor` line, the inspector instead verifies the embedded envelope
against a trusted Horo signing root, requires its bound release to equal
`horoVersion`, and requires its contract hash to equal the engine's frozen
contract hash. It does not add the future release to the frozen registry or infer
compatibility from numeric proximity.

A missing, invalid, untrusted, mismatched, or differently contracted proof makes
the project `FutureVersion`. After proof verification, bounded parsers and
document validators still protect against corrupt or manually altered contents;
the proof identifies an official format contract, not the integrity of every
project file.

The registry resolves a known exact project `horoVersion` and verified contract
hash to one exact `ContractBaselineVersion`. It never resolves by overlapping
version ranges. A compatibility decision is release provenance, not a project
migration, and therefore is not written into project migration history.

## Scope

### Governed project-owned data

The single project version governs:

- `.horo/project.json`
- project settings and portable input configuration
- authored scenes and prefabs
- project-owned component and behavior payloads
- material and shader-graph sources
- animation, VFX, UI, sequencer, terrain, and other authored graph documents
- asset identity sidecars and import/cook settings
- package/plugin contribution state stored in the project
- future Horo-owned durable authoring documents

Prefab instance/variant override sets are governed authored data. Per
[ADR-093](../../adr/093-prefab-override-property-identity-and-delta-operations.md),
their stable component/property/collection-element IDs, typed operation records,
expected source digests, conflicts and opaque orphans migrate with the project and
must not be discarded as derived cache data.

### Rebuilt derived data

The following is invalidated and rebuilt after migration:

- `.horo/asset_index.json`
- import, cook, shader, thumbnail, and editor preview caches
- cooked assets and cooked scenes
- generated code and generated descriptor indexes
- build-system output
- renderer pipeline caches

A migration must not spend time preserving data whose authority is a durable
source file plus a deterministic rebuild operation.

[ADR-105](../../adr/105-navigation-asset-and-scene-ownership-boundary.md)
applies this rule to navigation. `NavigationDefinition` assets, stable grounded
profile/area/link IDs and typed Scene navigation components are governed authored
data and migrate with the project. Bake-input snapshots, generated polygons/tiles,
cooked/cell NavigationMesh payloads, provider-private sections, caches and runtime
topology are invalidated and rebuilt. A legacy format containing embedded generated
or provider-native data may migrate only recoverable semantic intent through an
explicit fidelity-reporting recipe; generated bytes never become the new source of
truth.

### Independently versioned external/runtime contracts

The following are not governed by the editor project version because they may
outlive the authoring workspace or communicate with independently deployed
products:

- shipped save-game formats
- network protocols
- package/archive container formats
- extension ABI and plugin package versions
- release manifests and update protocols
- external interchange formats

Those systems may reuse the semantic-version and migration primitives, but their
compatibility decisions remain independent.

## Compatibility Inspection

Project compatibility is classified before the editor constructs a writable
project session:

```cpp
enum class ProjectCompatibilityStatus : std::uint8_t
{
    Current,
    CompatibleReleaseLine,
    AutomaticMigrationRequired,
    RecoveryRequired,
    FutureVersion,
    MigrationPathMissing,
    RequiredProviderUnavailable,
    Corrupt,
    Inaccessible
};
```

Inspection is read-only and bounded. It reads only the minimum project metadata,
migration journal, package lock, and provider descriptors required to classify
the project. It does not parse all scenes, load gameplay code, initialize a
renderer, rebuild caches, or modify recent-project metadata.

The result carries:

```cpp
struct ProjectCompatibilitySnapshot
{
    ProjectId project;
    HoroProjectVersion projectVersion;
    HoroProjectVersion targetVersion;
    ProjectCompatibilityStatus status;
    std::optional<MigrationPlanSummary> migration;
    BoundedDiagnosticList diagnostics;
};
```

Unknown future minor/major versions are not guessed compatible. A future patch
is accepted only through the signed same-contract proof defined above, never by
assuming that equal `major.minor` numbers are sufficient. Corrupt version text
is not treated as an old version. A missing migration provider is different from
a missing migration step and produces a separate typed diagnostic.

`CompatibleReleaseLine` is intentionally narrow: only patch differences within
the same `major.minor` line qualify. A known patch uses its registered decision;
a higher unknown patch requires a valid embedded proof whose contract equals the
engine's frozen hash. A future minor or major remains `FutureVersion`; numeric
closeness and a self-asserted bare hash are not compatibility.

## Module Ownership And Dependency Direction

Migration is one product workflow, not one monolithic library. Its contracts are
split along the existing dependency direction:

```text
Gui / apps
  -> EditorServices / application use cases

EditorServices
  -> Foundation
  -> Platform filesystem abstractions
  -> Packages migration-provider resolver
  -> Runtime/editor document migration providers

Platform -> Foundation
Packages -> Foundation
Runtime/editor document providers -> Foundation provider contracts
```

`A -> B` means `A` depends on `B`. Lower-level contracts never depend on editor
routes, ImGui, SDL, renderer backends, package UI, or concrete application
composition.

Responsibilities are normative:

- Foundation owns value types, stable IDs, errors, immutable migration
  descriptors, registry validation, and deterministic chain planning.
- Platform owns cross-platform filesystem capabilities. It does not know Horo
  document semantics or select migration steps.
- EditorServices owns the project-open use case, operation lifetime,
  cancellation, work budgets, migration lock, staging transaction, validation,
  derived-state invalidation, and project-session candidate.
- Runtime/editor document modules contribute convention-discovered typed stages
  and validators through narrow provider contracts. They do not navigate screens
  or publish files.
- Packages resolves verified provider implementations declared by the project
  lockfile. It does not bypass the core transaction.
- GUI observes immutable operation snapshots and emits cancel/retry/navigation
  intents. It does not mutate project files or advance migration phases.
- `apps/` composes concrete services and supplies the release's frozen target
  version and bundled migrations.

No subsystem may create a second project-version authority as a local shortcut.
If a subsystem needs independent runtime compatibility, it must fall under the
external/runtime-contract exception described above, not silently add another
project schema field.

## Welcome And Project Browser Presentation

Every recent-project card displays the last inspected Horo project version in a
right-aligned metadata area. The version is visible without opening the project.

Example:

```text
┌────────────────────────────────────────────────────────────┐
│  MyGame                                      Horo 1.7.0    │
│  /Projects/MyGame                         2 days ago       │
└────────────────────────────────────────────────────────────┘
```

The card also communicates compatibility without relying on color alone:

```text
Horo 2.1.0                 current
Horo 2.1.4 · Compatible    same release line; no migration required
Horo 1.7.0 · Will upgrade  automatic migration available
Horo 3.0.0 · Newer engine  cannot open with this editor
Version unavailable        missing/unreadable metadata
Recovery required          interrupted migration journal found
```

Requirements:

- `RecentProjectEntry` carries a last-known typed compatibility projection, not
  an arbitrary presentation string.
- The user-state recent-project cache may retain the last successful inspection
  result for fast first paint.
- The cache is not authoritative. A bounded background preflight refreshes the
  card from `.horo/project.json` and any migration journal.
- Clicking an older migratable project navigates directly to `ProjectLoading`.
  No migration confirmation modal is inserted.
- Clicking a future, corrupt, or provider-blocked project does not mutate it and
  exposes an actionable diagnostic or repair route.
- All labels are localized and verified at narrow widths and with long locale
  text.

## Automatic Project-Open Flow

```text
Welcome / Project Browser
  -> read-only compatibility inspection
  -> ProjectLoading route
  -> recover an interrupted operation when present
  -> acquire project migration lock
  -> resolve engine and trusted package migration providers
  -> construct complete migration chain
  -> inventory durable project files
  -> create same-filesystem staging and journal
  -> execute generated and explicit transformations
  -> validate every staged document and cross-file reference
  -> publish durable files
  -> publish project.json with target horoVersion last
  -> invalidate and rebuild derived state
  -> construct project/editor session
  -> enter EditorWorkspace
```

The exact current-version open path uses the same operation but skips
transformation and publication. A compatible newer patch may take the lightweight
marker-publication path defined below. Renderer selection/restart occurs only
after the minimum project compatibility preflight can safely read the requested
backend. Workspace construction never begins against partially migrated source
data.

## ProjectLoading Integration

The current simulated project-open progress path must be replaced by a real
`ProjectOpenOperation`. Migration is not a separate modal or screen; it is one
bounded segment of project loading.

```cpp
enum class ProjectOpenPhase : std::uint8_t
{
    Inspecting,
    ValidatingCompatibility,
    PublishingCompatibilityMarker,
    Recovering,
    ResolvingMigrationProviders,
    PlanningMigration,
    Inventorying,
    Staging,
    Migrating,
    ValidatingMigration,
    PublishingMigration,
    RebuildingDerivedState,
    InitializingSession,
    Completed,
    Failed,
    Cancelling
};
```

### Patch-only project open

An open from `1.0.3` to `1.0.7` with the same verified baseline, contract, and
signed release decision does not construct a migration plan or inventory all
project documents. It follows a lightweight metadata path:

```text
Inspecting
  -> ValidatingCompatibility
  -> acquire project mutation lock when the marker must advance
  -> PublishingCompatibilityMarker
  -> InitializingSession
```

`PublishingCompatibilityMarker` writes the new marker, contract identifier, and
signed decision proof to a sibling temporary `project.json`, flushes it,
atomically replaces the root metadata, and synchronizes the parent directory
according to platform policy. A single-file atomic replacement does not create
the multi-file migration journal. Failure before replacement leaves the old
marker; success leaves the new marker. A lower compatible patch verifies a
higher marker's embedded proof but never writes it backwards. Patch-only opening
neither alters `migration_history.json` nor changes `migrationHistoryHead`.

The loading UI reports compatibility verification and metadata update, not scene
migration. Failure injection immediately before and after atomic replacement is
required. The marker write still uses the project mutation lock and the owned
`ProjectOpenOperation` cancellation/shutdown policy.

The loading screen shows real progress and the automatic version transition:

```text
Opening 'MyGame'
Upgrading Horo project 1.7.0 → 2.0.0
Migrating scenes 12 / 48                         43%
```

After migration:

```text
Validating upgraded project...
Rebuilding asset index...
Initializing workspace session...
```

Progress is weighted from discovered bounded work units. It is monotonic and
does not use a timer simulation. The status model contains stable phase and
progress data; localization happens in the view.

### Cancellation

Cancellation is user control, not a required migration decision.

- Before publish, cancellation stops admission, joins owned work, deletes
  staging, and leaves the original project unchanged.
- During the small publish critical section, cancellation becomes pending and
  the UI reports `Finishing safely...`; it does not interrupt between journaled
  file replacements.
- After publish, cancellation may stop derived rebuild/session initialization,
  but the project remains a valid target-version project and can be reopened.
- Application shutdown follows the same policy and must not abandon worker
  callbacks after owning services are destroyed.

## Versioned Migration Definitions And Chain Planning

Persistent-contract transitions are immutable, Flyway-like definitions authored
in code and automatically cataloged at build time. Definitions use exact
contract baselines, never release ranges:

```cpp
struct ProjectMigrationDefinition
{
    StableMigrationId id;
    ContractBaselineVersion from;
    ContractBaselineVersion to;
    PersistentContractHash sourceContract;
    PersistentContractHash targetContract;
    std::span<const ProjectMigrationStep> steps;
    MigrationDefinitionHash hash;
};
```

Example:

```text
Definition baseline 1.0.0 / C1 -> baseline 1.1.0 / C2
  scene.camera.fov_to_vertical_fov
  project.renderer_id_canonicalization

Definition baseline 1.1.0 / C2 -> baseline 2.0.0 / C3
  component.stable_property_ids
  prefab.reference_identity_upgrade
  asset.sidecar_import_settings_split
```

Patch `1.0.1` through `1.0.7` resolve to baseline `1.0.0 / C1` through exact
release decisions. They do not create overlapping migration edges. A hotfix that
needs a persistent transformation must become a minor release and establish a
new baseline.

Definitions are grouped by the exact target contract baseline. Directory names
use canonical SemVer text and do not repeat `from`, `to`, or implementation
details:

```text
src/application/project_migrations/
├── definitions/
│   └── 1.1.0/
│       ├── migration.horo.json
│       ├── ProjectMigration.h
│       ├── ProjectMigration.cpp
│       └── stages/
│           ├── CameraContractStage.cpp
│           ├── LightContractStage.cpp
│           └── PrefabReferenceStage.cpp
└── checkpoints/
    └── 2.0.0/
        └── 1.0.0/
            ├── checkpoint.horo.json
            ├── ProjectMigration.h
            ├── ProjectMigration.cpp
            └── stages/
```

The directory grammar is exactly canonical stable SemVer
`<major>.<minor>.<patch>`; prerelease migrations use the separately defined
prerelease fixture tree and cannot collide with stable release definitions. The
manifest declares stable ID, exact source/target baselines, contract hashes, and
providers. The target baseline in the manifest must equal the directory name;
the generator derives the standardized namespace/factory symbol from that
version rather than accepting an arbitrary symbol string.

```json
{
  "id": "horo.project.contract.1.1.0",
  "fromBaseline": "1.0.0",
  "toBaseline": "1.1.0",
  "sourceContract": "sha256:C1...",
  "targetContract": "sha256:C2..."
}
```

At configure/build time, `ProjectMigrationCatalogGenerator` scans only immediate
children matching the directory grammar, parses every bounded manifest, and
generates one deterministic `GeneratedProjectMigrationCatalog.cpp`. The generated
catalog includes the standardized `ProjectMigration.h` entrypoint for each
version, orders definitions by typed target baseline, and exposes an immutable
span to composition. It also emits the build-tree source manifest containing the
standard `ProjectMigration.cpp` plus recursively sorted `*Stage.cpp` files under
that version directory. Configure dependencies cause a bounded reconfigure when
a matching directory/source is added or removed; CI independently regenerates
and verifies the catalog from a clean tree. Adding a correctly named directory,
valid manifest, standardized entrypoint, and convention-matching stage sources is
sufficient; developers do not edit a central registration/source-list file or
write per-migration `registry.Register(...)` calls.

Runtime filesystem discovery, linker-section tricks, static constructors, and
hidden global registration are forbidden. They make availability depend on
packaging, link order, or process initialization. The build fails on malformed
names/manifests, duplicate IDs/baselines, missing standardized entrypoints, gaps,
hash drift, or generated-catalog differences. `apps/` passes the generated
catalogs to registry construction; successful construction returns immutable
registries before project inspection.

Published definitions and generated catalog inputs are append-only and
checksum-immutable. Directory names provide discovery and ownership; manifest
metadata and typed registry edges remain the semantic authority.

The planner:

- resolves the exact source release through `ReleaseCompatibilityRegistry`
- accepts only forward transitions between exact contract baselines
- validates definition identity, contract hashes, and definition hash
- rejects cycles, duplicate edges, multiple canonical forward paths, gaps, and
  target overshoot
- produces a deterministic ordered chain
- records every step and affected format family before mutation
- resolves required package/plugin capsules before staging
- never chooses an arbitrary shortest path when multiple paths disagree

Users may skip engine releases. The installed editor must retain a complete
chain from its declared `minimumMigratableVersion` or ship a tested checkpoint
migration that replaces an older prefix. A release must not require the user to
install intermediate editors manually.

### Support horizon and checkpoints

Every release manifest contains:

```cpp
struct ProjectMigrationSupportDescriptor
{
    HoroProjectVersion targetVersion;
    HoroProjectVersion minimumMigratableVersion;
    PersistentContractHash releaseLineContract;
    std::span<const MigrationCheckpointId> checkpoints;
};
```

The support policy is explicit and bounded:

- The first development baseline is the oldest migratable version until a later
  release deliberately advances the boundary.
- A major release review must either retain the existing boundary or publish a
  checkpoint and declare the new boundary. It may not drop history accidentally
  because an old step disappeared from packaging.
- A checkpoint converts one declared older baseline directly to a later
  validated baseline and replaces the covered chain prefix. It must preserve the
  same externally visible result as sequential migration for retained fixtures.
- Releases older than `minimumMigratableVersion` are classified as
  `MigrationPathMissing` without mutation and report the oldest supported source
  version.
- Historical fixtures, definition hashes, and migration capsules inside the support
  window are release artifacts. They are not pruned as ordinary build cache.

Before the first stable release, the baseline may be reset deliberately because
there are no external projects to preserve. After a stable public baseline, any
boundary advance is a documented product support decision with release notes,
fixture coverage, and archived tooling—not an incidental implementation detail.

## Automatic And Explicit Transformations

### Generated transformations

Horo's persistent descriptor toolchain stores stable document, component,
property, enum-value, and asset-reference identities. Release tooling compares
the previous published descriptor snapshot with the current snapshot.

The following changes may generate migration steps automatically:

- adding an optional field with a deterministic default
- renaming a field while preserving its stable property identity
- moving a field between typed containers with an explicit stable alias
- adding an enum value
- remapping enum values through a declared table
- widening a numeric type when the conversion is lossless and bounded
- changing a canonical textual spelling while preserving typed identity
- adding an optional component or document section
- invalidating derived data associated with changed source descriptors

Generated steps are reviewed artifacts committed with the release migration
definition. They are not synthesized differently on each user's machine.

### Developer-authored recipes

The following require an explicit engine or trusted package migration recipe:

- semantic unit changes
- lossy numeric conversions
- splitting or merging components
- hierarchy rewrites
- changing stable IDs
- replacing asset reference models
- transforming behavior/plugin-owned payload semantics
- any change requiring domain knowledge not represented by descriptors

[ADR-132](../../adr/132-platform-services-project-salt-stable-id-tombstone-and-provider-mapping.md)
is one such boundary: replacing `platformServicesIdSalt` changes every Platform
Services numeric identity. A project fork/regeneration therefore requires a dry-run
old-to-new map for active and tombstoned entries, complete durable-reference and
provider-mapping coverage, transactional publication and derived/offline invalidation.
Editing the salt field, regenerating during cook or treating text aliases as numeric
namespace compatibility is forbidden.

PLS-003.2 supplies the bounded deterministic registry validator used on both sides of
such a migration. A migration stages a complete candidate and provider-mapping set,
validates every active and tombstoned identity plus the captured fingerprint, and only
then allows the Project transaction owner to publish it. A failed candidate leaves the
previous immutable registry usable; the registry API itself performs no file repair,
salt generation, ambient registration or partial publication.

Example:

```cpp
struct MigrationRecipeDescriptor
{
    StableMigrationId id;
    MigrationDomain domain;
    MigrationOperation operation;
    ValidationPolicy validation;
};
```

Project migrations do not execute arbitrary source text or project gameplay code.
Core recipes are compiled into trusted engine tooling. Package-owned recipes are
resolved from installed, verified packages and run through the package migration
capability boundary. Their output is always revalidated by core document
validators.

Prefab override migration is ID- and schema-driven. A retained identity may survive
a representation rename; a tombstoned identity may move only through a declared
one-way mapping and total typed value conversion. Unkeyed index paths, missing
package schemas, ambiguous split/merge or failed conversion remain preserved
conflicts/orphans. Migration never guesses from localized labels, C++ offsets,
reflection order or current collection indexes. Cooked prefabs remain derived and
are recooked from the successfully migrated effective authoring state.

### Release gate

The release pipeline captures a canonical persistent-descriptor snapshot and
compares it with the preceding release.

```text
Persistent contract unchanged
  -> require exact compatibility decision mapped to the existing baseline

Compatible descriptor change
  -> establish a new baseline and generate/test migration steps

Incompatible or semantic change
  -> require explicit migration recipe

Changed contract with no decision
  -> fail release build
```

This gate makes automatic user migration possible. A single project version can
detect when migration is needed, but it cannot infer the meaning of an
unrecorded semantic change after release.

## Migration Registry

The backend-neutral registries own release decisions, migration definitions, and
planning metadata. They do not own GUI, filesystem mutation, JSON, project
routes, or package installation.

Suggested public contracts:

```cpp
class ReleaseCompatibilityRegistry
{
  public:
    static Result<ReleaseCompatibilityRegistry> Create(
        std::span<const ReleaseCompatibilityDecision> generatedCatalog);
    Result<ContractBaselineVersion> Resolve(
        EngineReleaseVersion release,
        PersistentContractHash contract) const;
};

class ProjectMigrationRegistry
{
  public:
    static Result<ProjectMigrationRegistry> Create(
        std::span<const ProjectMigrationDefinition> generatedCatalog);
    Result<ProjectMigrationPlan> Plan(ContractBaselineVersion from,
                                      ContractBaselineVersion to) const;
};

struct ProjectMigrationPlan
{
    ContractBaselineVersion source;
    ContractBaselineVersion target;
    std::vector<StableMigrationId> definitions;
    std::vector<ProjectMigrationStep> steps;
    MigrationWorkEstimate estimate;
    BoundedDiagnosticList diagnostics;
};
```

Catalog validation and planning are load-time operations. Successful `Create`
returns an already immutable registry safe for concurrent inspection; there is
no partially registered public state. Tests may inject a small generated-style
span directly. Duplicate, cyclic, or ambiguous definition sets fail before a
project can be mutated.

### Typed inventory and execution context

Migration steps do not enumerate the filesystem or receive an unrestricted root
path. The executor builds one normalized, bounded, deterministically sorted
inventory before step execution:

```cpp
struct MigrationDocumentQuery
{
    MigrationDocumentKind kind;
    std::optional<MigrationDomain> domain;
    std::optional<StableTypeId> type;
};

class ProjectMigrationContext
{
  public:
    std::span<const MigrationDocumentEntry> ListDocuments(
        const MigrationDocumentQuery& query) const;
    Result<ProjectDocumentView> ReadDocument(
        MigrationDocumentHandle document) const;
    Result<void> ReplaceDocument(MigrationDocumentHandle document,
                                 MigratedDocument replacement);
    Result<void> AddDocument(ProjectRelativePath path,
                             MigratedDocument document);
    Result<void> RemoveDocument(MigrationDocumentHandle document);
    Result<void> InvalidateDerivedState(DerivedStateKind kind);
};
```

Handles belong to the operation generation and become stale on cancellation or
replacement. Queries return stable inventory order and cannot discover paths
created by another concurrent writer because the project mutation lock and input
hash validation protect the source snapshot. Add/remove/replace operations
target staging only and are recorded in the candidate change set.

### Migration stage interface and job pipeline

A definition builds one typed `MigrationPipeline`; it does not expose a bag of
unrelated callbacks. The pipeline is a dependency stream compiled by the
migration executor onto Foundation `TaskGroup`/`JobSystem`. It is not a second
scheduler and does not add a public general-purpose `JobStream` abstraction.

Two stage shapes cover deterministic parallel and cross-document work:

```cpp
class IProjectMigrationDocumentStage
{
  public:
    virtual ~IProjectMigrationDocumentStage() = default;
    virtual MigrationStageDescriptor Describe() const noexcept = 0;
    virtual Result<MigrationDocumentChange> Execute(
        const ProjectDocumentView& source,
        const MigrationStageContext& context,
        const CancellationToken& cancellation) const = 0;
};

class IProjectMigrationStage
{
  public:
    virtual ~IProjectMigrationStage() = default;
    virtual MigrationStageDescriptor Describe() const noexcept = 0;
    virtual Result<void> Execute(
        ProjectMigrationContext& context,
        const CancellationToken& cancellation) const = 0;
};
```

Document stages receive one immutable document and return an owned change; they
cannot mutate shared staging from worker threads. The executor may fan them out
through an operation-owned `TaskGroup`, then merges successful changes in stable
inventory order at the node barrier. Cross-document/add/remove stages receive
the constrained project context and execute on the serialized migration
executor. Stage descriptors declare stable ID, reads, writes, required providers,
estimated weight, and execution shape. Conflicting parallel write sets fail
pipeline validation.

Each standardized `ProjectMigration.cpp` exposes one factory in a namespace
derived by the catalog generator from the target version:

```cpp
namespace Horo::ProjectMigrations::R1_1_0
{
    Result<ProjectMigrationPipeline> BuildProjectMigration()
    {
        return ProjectMigrationPipelineBuilder::Begin(
                   StableMigrationId{"horo.project.contract.1.1.0"})
            .ForEach<CameraContractStage>(
                MigrationDocumentQuery::Kind(MigrationDocumentKind::Scene))
            .Then<LightContractStage>()
            .ForEach<PrefabReferenceStage>(
                MigrationDocumentQuery::Kind(MigrationDocumentKind::Prefab))
            .Then<CrossDocumentReferenceStage>()
            .Validate<ContractC2Validator>()
            .Build();
    }
}
```

Pipeline semantics are normative:

- `ForEach<T>` creates bounded document jobs from the deterministic query.
- `Then<T>` depends on completion and deterministic merge of the preceding node.
- `Validate<T>` is a terminal validation node and cannot mutate candidate data.
- A node failure cancels not-yet-started dependent nodes and joins all accepted
  jobs before returning.
- Cancellation is checked before scheduling, during bounded work, before merge,
  and before publication.
- Stage IDs and node order participate in `MigrationDefinitionHash`; renaming,
  reordering, adding, or removing a published stage is release hash drift.
- Pipeline output is a candidate change set. Only the transaction executor owns
  staging publication, journal updates, history receipt, and rollback.

Simple sequential changes may use only `Then<T>`. Large scene/prefab sets gain
bounded parallelism through `ForEach<T>` without letting migration authors create
threads, submit unowned jobs, or control publish ordering.

### Plan preview and verified dry-run

Plan preview and dry-run are different operations:

- Plan preview resolves definitions, providers, estimated work, affected domains,
  and conservative capacity without executing transformation code. It is fast
  and read-only but does not prove that migration will succeed.
- Verified dry-run constructs the real inventory, executes the same steps into
  disposable staging, and runs all document and cross-file validators. It never
  obtains publish authority, never replaces an authoritative file, and deletes
  staging after producing its bounded result.

```cpp
enum class MigrationExecutionMode : std::uint8_t
{
    Publish,
    VerifiedDryRun
};
```

Both modes use the same `ProjectMigrationContext` and transformation code.
Dry-run is not a mock context with different mutation semantics. Reverse or
`undo` migrations are forbidden; rollback restores verified original files.
Repeatable migrations are also forbidden—repeatable work belongs to derived
state rebuilding.

## File Inventory And Dependency Order

Migration operates on a deterministic, normalized project-relative inventory.
It rejects root escape, unsafe symlinks, portable case collisions, duplicate
stable identities, unbounded document counts, oversized inputs, and files that
change after inspection.

Default transformation order:

1. project and package identity metadata needed by later steps
2. asset sidecars and stable asset-reference metadata
3. shared typed descriptors and project settings
4. prefabs and reusable authored graphs
5. scenes and scene-local behavior payloads
6. input and build/release authoring configuration
7. cross-document references and default-scene resolution
8. project root version marker

The actual plan is a validated dependency graph. The numbered order is a
default, not permission to ignore declared edges.

## Crash-Safe Transaction

Migration never edits the authoritative files in place.

```text
authoritative project (read-only during transform)
  -> .horo/local/migration/<operation-id>/staging
  -> transform staged copies
  -> validate staged project
  -> create and verify durable rollback copies of every replaced file
  -> write durable journal
  -> publish file replacements
  -> publish migration_history.json
  -> publish project.json last
  -> mark journal committed
  -> rebuild derived state
  -> cleanup staging according to retention policy
```

Staging and rollback storage are on the same filesystem as the project so
individual replacement renames can be atomic and recovery never depends on a
temporary directory from another volume. Before transformation begins, preflight
accounts for the bounded worst-case staging, rollback, journal, and filesystem
overhead. Insufficient capacity fails before source data is changed. A project
mutation lock prevents editor, CLI, MCP, import, save, autosave, and package
operations from writing concurrently.

Capacity is calculated over the affected durable authored-file set, not the
entire project directory:

```text
required temporary bytes =
    estimated staged outputs
  + rollback originals for replaced/deleted files
  + journal and filesystem safety overhead
```

Imported source blobs that are not transformed are not duplicated. Derived
indexes, caches, cooked artifacts, thumbnails, build output, and renderer caches
are invalidated rather than staged. If every authored file changes, conservative
temporary capacity may approach twice the affected authored bytes in addition
to the originals already present. Platform-supported reflink/copy-on-write may
reduce physical usage, but correctness and admission never assume it is
available; ordinary verified copies remain the fallback.

The journal records:

- operation ID
- project identity
- source and target Horo versions
- migration definition hashes
- normalized affected paths
- original and staged content hashes
- verified rollback-copy paths and hashes for every file that may be replaced
- original and staged migration-history hashes and resulting history-head hash
- publish order and completed replacements
- cancellation and recovery state
- engine build identity for diagnostics

Before publish, cancellation or failure removes staging and rollback copies and
changes nothing. During multi-file publish, the journal and verified rollback
copies make interruption recoverable even though general filesystems do not
offer one atomic transaction across many paths. Recovery never attempts to
reconstruct an original file from its hash. `project.json` is replaced last; its
target version means every preceding replacement and validation has completed.

Rollback copies live under ignored machine-local project state and remain pinned
until the journal is durably committed. An optional configured external recovery
root may receive a longer-lived post-commit backup, but it cannot replace the
same-filesystem transactional rollback set. Version control is recommended but
is not the transaction or rollback mechanism.

## Migration History And Receipts

The recovery journal and permanent audit history are separate artifacts:

```text
.horo/local/migration/<operation-id>/journal.json  temporary recovery authority
.horo/migration_history.json                      project-owned audit evidence
```

One successful project-open migration chain appends one receipt containing its
source exact release, resolved source baseline, target release, ordered
definition IDs and hashes, operation ID, engine build identity, and completion
time. Patch-only compatibility-marker updates do not append receipts because no
persistent transformation ran. Their exact compatibility decisions remain
signed engine release artifacts in `ReleaseCompatibilityRegistry`.

History is an ordinary affected file in the migration transaction:

1. The receipt is appended to staged history.
2. Staged history is validated and receives a rollback copy/hash like every
   replaced file.
3. Authored migrated files are published.
4. `migration_history.json` is published.
5. `project.json`, containing the new version, contract hash, and
   `migrationHistoryHead`, is published last.

If failure occurs after history publication but before root publication,
recovery either resumes the remaining verified publish or restores the original
history together with every other published file. A restored project cannot
retain a receipt for a migration that was rolled back. History never selects a
migration or overrides root compatibility; its head hash only detects audit/root
inconsistency.

## Recovery

Project open checks for an incomplete journal before ordinary compatibility
inspection.

```cpp
enum class MigrationRecoveryAction : std::uint8_t
{
    DiscardUnpublishedStaging,
    ResumePublish,
    RestoreOriginals,
    FinalizeCommittedMigration
};
```

Preparation interrupted before `PublishReady` is discarded and deterministically
prepared again. Journal readability is selected by the content-addressed
`MigrationRecoveryContractId` frozen into each release, not by requiring the
writer release to equal the running editor release. Committed operation directories
move to `.horo/local/migration-cleanup/<operation-id>` and a bounded janitor removes
them without treating them as unfinished journals.

Recovery action selection is automatic from journal state and verified hashes.
It is not guessed from timestamps. If no action can be proven safe, the project
remains closed and a diagnostic bundle is produced; the editor does not attempt
a destructive best effort.

## Package And Plugin Participation

Project migration may require descriptors owned by packages or plugins.

Migration execution must not depend on the currently installable plugin binary
remaining available forever. Each package release that changes durable payloads
publishes an immutable migration capsule:

```cpp
struct MigrationCapsuleDescriptor
{
    MigrationCapsuleId id; // content-addressed
    PackageId package;
    PackageVersion packageVersion;
    HoroProjectVersion from;
    HoroProjectVersion to;
    MigrationExecutorContract executorContract;
    ContentHash payloadHash;
    SignatureEnvelope signature;
};
```

A capsule contains declarative descriptor transforms and, only where necessary,
a trusted bounded transform targeting a retained migration-executor contract. It
does not contain or launch the package's ordinary editor/runtime plugin entry
point. The project lockfile records required capsule identities. Capsules are
content-addressed, signed, and retained in the project-local package store,
configured package registry, or Horo migration archive for at least the declared
project migration support window.

- Package restore and signature/trust verification occur before provider
  activation.
- Migration providers expose data descriptors or trusted bounded transforms,
  not UI callbacks.
- Resolution prefers the exact capsule recorded by the lockfile and may restore
  it automatically without installing or activating an abandoned plugin.
- A missing required capsule blocks mutation and identifies its package,
  content hash, and the archives that were queried.
- Unknown plugin/component payloads are preserved opaquely when their owning
  document format permits it.
- A step that must interpret unknown payload semantics cannot run without the
  provider.
- Plugin migration cannot navigate screens, run arbitrary project processes,
  access credentials, or mutate files outside the staged project boundary.
- Provider completion is validated against the operation generation so late
  results cannot commit into a cancelled or replacement operation.

For the normal user path, required compatible packages are restored
automatically from the lockfile before migration. Trust escalation or an
unavailable package remains an explicit blocking diagnostic rather than a silent
data-loss fallback.

There is no automatic "skip and delete unknown data" path. If a payload can be
preserved opaquely under the target contract, it is carried forward without
interpretation. If semantic transformation is mandatory and the capsule cannot
be obtained or verified, the authoritative project remains untouched. A future
destructive quarantine/export repair tool is a separate, explicitly authorized
data-recovery workflow and is not part of ordinary automatic project opening.

## Error And Diagnostic Contract

Stable error categories include:

```text
project.version.missing
project.version.invalid
project.version.future
project.version.compatibility_proof_missing
project.version.compatibility_proof_invalid
project.version.compatibility_proof_untrusted
project.version.contract_mismatch
project.migration.path_missing
project.migration.path_ambiguous
project.migration.definition_invalid
project.migration.provider_missing
project.migration.capsule_missing
project.migration.capsule_invalid
project.migration.locked
project.migration.input_changed
project.migration.transform_failed
project.migration.validation_failed
project.migration.publish_failed
project.migration.recovery_failed
project.migration.cancelled
```

Errors preserve the failing definition, step, project-relative path, source/target
version, and underlying subsystem error. User-facing views localize summaries;
logs and diagnostic bundles retain stable IDs and technical context.

One invalid document fails the project transaction. Partial successful output is
not opened as a degraded writable project.

## Concurrency And Ownership

- `ProjectOpenService` owns the operation, cancellation source, staging lease,
  migration lock, journal, and final project-session candidate.
- Workers may inspect and transform immutable source/staged snapshots.
- Workers never publish authoritative files or navigate GUI routes.
- Filesystem publication, project-version commit, and session activation occur on
  the application owner thread or its dedicated serialized project-mutation
  executor.
- Work is bounded by file count, input bytes, output bytes, diagnostic count,
  concurrent jobs, and wall-clock policy.
- Cancellation and shutdown stop admission and join service-owned work before
  destroying providers or the job system.
- Completion checks operation ID, project identity, source hashes, and target
  generation before accepting worker output.

## Host Behavior

### GUI

Welcome shows version and compatibility. `ProjectLoading` executes migration and
real project open work. Success enters `EditorWorkspace`; failure leaves the
project unchanged and provides retry/diagnostic navigation.

### CLI

The same application service supports:

```bash
horo-engine project inspect <root> --json
horo-engine project migrate <root> --dry-run --json
horo-engine project open-check <root> --json
```

Ordinary GUI opening migrates automatically. CLI mutation requires an explicit
command because invoking a headless inspection command must remain read-only.
CI may enable migration in a disposable checkout and fail when it produces an
unexpected diff.

### MCP

MCP may inspect compatibility and start the same bounded migration use case only
with project-write capability. It does not expose arbitrary migration-step
execution or raw filesystem replacement.

## Testing And Release Gates

Required coverage includes:

- canonical SemVer parse, formatting, ordering, overflow, prerelease, and build
  metadata policy
- same-release-line patch compatibility, future-patch contract-hash verification,
  signed future-decision verification, and refusal of a patch whose persistent
  descriptor hash drifted
- missing, malformed, mismatched, and untrusted embedded compatibility proofs;
  a bare self-asserted contract hash must not admit an unknown future patch
- exact-release-to-baseline resolution with strong-type compile-time checks and
  no overlapping/range-based migration edges
- automatic canonical-SemVer directory discovery, manifest/directory mismatch,
  generated source catalog determinism, clean-tree verification, and absence of
  static/manual registration
- patch-only marker atomic replacement, no-history behavior, downgrade-marker
  prevention, and failure injection before/after replacement
- current, old, future, missing, invalid, and interrupted project classification
- recent-project version/status cache refresh and stale-cache replacement
- Welcome card current/old/future/unavailable/recovery presentation
- direct Welcome-to-ProjectLoading navigation for an old migratable project
- deterministic multi-release chain and skipped-release migration
- `minimumMigratableVersion` boundary and checkpoint equivalence to the retained
  sequential chain
- no-persistent-change release mapped explicitly to the existing baseline
- missing, duplicate, cyclic, ambiguous, overshooting, and corrupted definitions
- generated additive/default/rename/enum/numeric migration fixtures
- explicit semantic recipe fixtures
- deterministic typed inventory enumeration plus add/remove/replace and stale
  operation-handle rejection
- pipeline `Then` ordering, bounded `ForEach` fan-out, stable merge order,
  conflicting write-set rejection, failure propagation, cancellation, and owned
  task-group join
- package capsule restore, abandoned-plugin migration, missing/invalid capsule,
  stale provider completion, and unknown payload preservation
- plan preview without execution and verified dry-run through real disposable
  staging with zero authoritative project mutation
- cancellation before publish and during publish
- injected failure/crash after every journal and replacement boundary
- recovery resume, rollback, finalize, and unrecoverable hash mismatch
- crash after history publish/before root publish, including resume and rollback
  with no phantom receipt
- original project preservation after every failed path
- project root version published last
- affected-authored-byte capacity estimation, insufficient-space rejection before
  mutation, reflink optimization, and verified ordinary-copy fallback
- deterministic output and second-run no-op behavior
- non-ASCII, spaces, permissions, root escape, symlink, case collision, file
  replacement, and cross-platform path behavior
- derived state invalidation/rebuild without source migration
- bounded file count, byte count, concurrency, diagnostics, and cancellation
- GUI, CLI, and MCP consuming the same compatibility/plan/result model
- fixture projects for every source Horo version at or above the release's
  `minimumMigratableVersion`
- Linux/GCC, macOS/Clang, and Windows/MSVC filesystem/recovery coverage

Release gates:

- persistent descriptor snapshot diff is clean or covered by a migration decision
- patch release persistent descriptors and migration-definition-set hashes
  exactly match the frozen `major.minor` release-line contract
- every migration definition passes fixture migration and current validators
- a project created by each supported public release reaches the target release
  without an intermediate editor installation
- migration output is deterministic across supported host platforms
- release packaging contains every required core definition, support descriptor,
  checkpoint, signed compatibility decision, and trusted migration-capsule
  manifest
- migration code and descriptor hashes are included in release provenance

## Implementation Plan

Implementation status on 19 July 2026: MIG-001A through the local MIG-001E slice are implemented by the
backend-neutral `HoroEngine::Application` target. It includes canonical SemVer,
strong release/baseline identities, frozen `0.0.1` and current `0.1.0` contract decisions,
bounded read-only metadata inspection, fail-closed proof verification, and the
editor metadata/preflight consumers. MIG-001B adds `HoroEngine::ProjectMigrations`,
convention-based sequential/checkpoint discovery, frozen definition-set hashes,
immutable exact-baseline planning, typed pipeline barriers, deterministic
inventory and non-publishing verified dry-run. PRJ-001A adds native durable
filesystem operations and one shared project-mutation lease. MIG-001C adds
sparse prepared candidates, conservative storage admission, rollback copies,
durable journals, ordered root-last publication, permanent history receipts and
hash-driven resume/rollback/finalization, recovery-contract selection, canonical
compact history and committed-cleanup isolation. MIG-001D replaces timer-driven
loading with an operation-owned `ProjectOpenService` job, automatic recovery,
compatibility/migration resolution, patch-marker publication, two-stage
derived-state contributors, renderer preflight and a generation-safe
project-session activation lease. All project entry routes now pass through
ProjectLoading. The first production migration exercises that complete path and
MIG-001E adds display-only cached compatibility projections plus bounded
background refresh. MIG-001F remains proposed.

### MIG-001A — Version and compatibility foundation

- Add canonical `HoroVersion`, parsing, comparison, prerelease policy,
  `ProjectCompatibilityStatus`, typed errors, and standalone public-header tests.
- Add release-line persistent-contract hashes, patch compatibility, and
  `minimumMigratableVersion` support descriptors.
- Add strong `EngineReleaseVersion`/`ContractBaselineVersion` types, exact signed
  `ReleaseCompatibilityDecision` artifacts, and immutable compatibility registry.
- Add embedded compatibility-proof parsing and trust verification so an older
  patch can verify—but never guess—a higher same-line patch contract.
- Replace integer `formatVersion` in newly created projects with canonical
  `horoVersion` while the project has no external compatibility obligation.
- Keep game-authored `projectVersion` separate.
- Add read-only `InspectProjectCompatibility` with strict bounded parsing.

### MIG-001B — Definitions, registries, context, and planner

- Add canonical target-version directory discovery, bounded migration manifests,
  generated source/catalog composition, code-authored definitions, immutable
  `ProjectMigrationRegistry`, and clean-tree catalog verification.
- Add typed stage interfaces and `ProjectMigrationPipelineBuilder`, compiling
  sequential/fan-out dependencies onto operation-owned `TaskGroup` jobs with
  deterministic merge and conflict validation.
- Add checksum validation, deterministic exact-baseline chain planning, provider
  requirements, and estimates.
- Add typed deterministic document inventory, constrained staging context,
  plan-preview, and verified dry-run execution modes.
- Add checkpoint planning, one no-persistent-change release decision, and one
  real fixture migration definition to prove the pipeline.
- Add release descriptor snapshots and the persistent-change release gate.

### MIG-001C — Transaction and recovery

- Implemented locally: add shared project mutation lock, deterministic inventory, same-filesystem staging,
  durable journal, validation, ordered publish, root-version-last commit,
  cancellation, cleanup, and automatic recovery.
- Add project-owned migration history, staged receipts, history-head anchoring,
  and history-aware resume/rollback.
- Add failure injection at every durable boundary and cross-platform filesystem
  tests.

### MIG-001D — Project-open integration

- Implemented locally: replace simulated project loading with `ProjectOpenService` phases and real
  progress snapshots.
- Add lightweight atomic patch-marker publication without a migration definition,
  multi-file journal, or history receipt.
- Run inspection, recovery, migration and derived preparation on one owned job;
  install derived candidates, perform renderer preflight and reserve/consume the
  workspace candidate on their explicit owner boundaries.
- Preserve the current route until the operation succeeds; do not construct a
  workspace against staged state.

### MIG-001E — Welcome and Project Browser visibility

- Implemented locally: `RecentProjectEntry` carries typed Horo version,
  compatibility status, target version and cache freshness.
- A host-owned inspection service reuses the project-open preflight authority,
  caps one refresh at 128 projects and four concurrent jobs, rejects stale
  generations, and joins accepted work during shutdown.
- Welcome cards render right-aligned textual version/status metadata; cached
  state is presentation-only and every click still enters `ProjectLoading` for
  fresh authoritative inspection.
- English/Turkish catalog parity and service/controller regressions cover the
  current, migratable, corrupt, cached, refreshing and fresh paths.

### MIG-001F — Subsystem and provider adoption

- Register core scene, prefab, project settings, input, asset-sidecar, material,
  graph, and future authored-document transformations under the single Horo
  version chain.
- Integrate content-addressed migration capsules, trusted package/plugin
  providers, archive retention, and automatic lockfile restore.
- Replace subsystem-local project schema decisions with references to this
  architecture while retaining independent save/network/archive/ABI versions.

## Documentation Adoption

After review, the following documents must reference this contract and remove
conflicting project-owned schema timelines:

- `foundation/system-design.md`
- `editor/project-model.md`
- `editor/editor-document-model.md`
- `editor/gui-screen-host.md`
- `runtime/asset-pipeline.md`
- `runtime/built-in-scene-primitives.md`
- `packages/package-lifecycle.md`
- `release/distribution-and-update.md`
- `delivery/quality-and-ci.md`
- `desired-project-tree.md`
- [GitHub Project 8 — Horo Engine Kanban](https://github.com/users/abdullahbodur/projects/8/views/1)

## Related Documents

- [System Design](./system-design.md)
- [Error And Diagnostics](./error-and-diagnostics.md)
- [Concurrency And Job System](./concurrency-and-jobs.md)
- [Platform Abstraction](./platform-abstraction.md)
- [Project Model](../editor/project-model.md)
- [Editor Document Model](../editor/editor-document-model.md)
- [GUI Screen Host](../editor/gui-screen-host.md)
- [Asset Pipeline](../runtime/asset-pipeline.md)
- [Package Lifecycle](../packages/package-lifecycle.md)
- [Distribution And Update](../release/distribution-and-update.md)
