# Project Model

## Project-open ownership

`ProjectLoading` is a presentation route over the host-owned, GUI-neutral
`ProjectOpenService`; it does not simulate progress or mutate project metadata.
The service runs cleanup/recovery before compatibility inspection, resolves an
atomic same-contract patch marker or the journaled migration transaction,
prepares registered derived state on its operation job, installs those candidates
on the application owner thread, and performs renderer preflight. A project-open
job holds the shared mutation lease while it acts as the dedicated serialized
project-mutation executor; the GUI thread only pumps progress and owner-thread
publication. The current route remains active until a fully authoritative,
generation-safe `ProjectSessionCandidate` is ready or a renderer restart
continuation is returned. Failure exposes Retry/Back without constructing a
workspace against staged state.

After compatibility and any required migration complete, project-open resolves
`settings.defaultScene` to an absolute project-contained `.horo` path and performs
bounded read, schema, object, and document-invariant validation on the operation
worker. Missing files, parent traversal, symlink escape, unreadable or oversized
content, unsupported schema, and invalid document topology fail the loading
operation before derived-state publication or workspace construction. Workspace
entry repeats the authoritative load boundary and must propagate a typed
initialization error instead of silently constructing a fallback scene.

`EditorWorkspace` is not a root-path entry point. Its route carries a non-zero
`ProjectSessionCandidateId`. `OnEnter()` reserves the candidate, constructs the
controller and attaches panels, then consumes the candidate exactly once. A
failed entry releases the reservation and rolls partial workspace state back.
Welcome recent projects, startup projects and newly created projects all enter
through `ProjectLoading`; no UI route may bypass compatibility, recovery,
derived-state or renderer preflight.

Welcome uses the same read-only `ProjectOpenPreflightService` as project-open,
but only through a display projection. At most 128 recent roots are refreshed
per generation with four concurrent inspection jobs. Cached version/status data
exists solely for first paint; it cannot disable a card, publish metadata, or
authorize migration. Clicking any card creates a `ProjectLoading` request, where
fresh preflight is repeated before mutation authority can be acquired.

## Purpose

This document defines the durable project and workspace model for Horo Engine.
It covers the on-disk project structure, editor workspace persistence, project
settings, and the data model shared by project-browser screens, the editor, CLI,
and MCP.

## Project Directory

A Horo project is a directory with a `.horo/` metadata folder:

```text
MyGame/
    .horo/
        project.json           # project identity and settings
        packages.json          # portable package and extension requests
        packages.lock          # exact resolved package graph
        input.json             # portable project input defaults
        collision.json         # portable Physics layer/profile/query schema
        editor_workspace.json  # last editor layout and UI state
        asset_index.json       # derived asset lookup registry
        local/                 # optional machine-local overrides, ignored
    Assets/                    # source assets in newly created projects
        Models/
        Textures/
        Materials/
        Shaders/
        Scenes/
    src/                       # optional game code
    CMakeLists.txt             # optional project build file
    build/                     # generated build outputs and asset caches
```

The project root is the directory containing `.horo/`. All project-relative
paths are resolved from this root.

New project templates create `Assets/` with title-cased category folders and
store default scenes under `Assets/Scenes/`. Existing projects with a lowercase
`assets/` root retain their on-disk spelling and paths; opening them does not
rename or duplicate their directories. Asset and scene paths preserve the
project's actual root spelling, including on case-sensitive filesystems. The
`assets` JSON property in the derived index is a schema key, not a directory
name.

Durable portable metadata, machine-local state, and derived output remain
separate:

- `.horo/project.json`, `.horo/packages.json`, `.horo/packages.lock`,
  `.horo/input.json`, `.horo/collision.json`,
  `.horo/platform_services.ids.json`, and asset sidecars are portable
  source-controlled inputs
- `.horo/editor_workspace.json`, `.horo/local/`, and `.horo/asset_index.json`
  are local or derived state
- `build/` contains generated build outputs and content-addressed asset caches

Legacy `.horo/plugins.json` is read only by the transactional ADR-054 migration
into `.horo/packages.json`; it is not a second package request authority.

## Project Format And Identity

`project.json` stores stable project metadata:

```json
{
  "horoVersion": "0.1.0",
  "persistentContract": "sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03",
  "projectId": "proj_2a4f...",
  "platformServicesIdSalt": "psid1:0123456789abcdef0123456789abcdef",
  "name": "MyGame",
  "projectVersion": "0.1.0",
  "createdAt": "2026-01-15T09:30:00Z",
  "settings": {
    "renderBackend": "opengl",
    "physicsEnabled": true,
    "targetFrameRate": 60,
    "defaultScene": "Assets/Scenes/main.horo",
    "assetCompression": "lz4",
    "textureCompression": "bc7",
    "buildProfile": "desktop-debug",
    "requiredToolchain": {
      "targetPlatform": "host",
      "compilerFamily": "default",
      "minimumCxxStandard": 20
    }
  }
}
```

`horoVersion` is the single engine/project compatibility marker.
`persistentContract` binds that exact release to the durable project descriptor.
`projectVersion` remains the independent game/product version and never selects
engine migrations. MIG-001A performs bounded, read-only classification through
`HoroEngine::Application`; unknown future releases are fail-closed unless an
injected verifier validates an exact compatibility proof.

`projectId` is generated once at project creation and never changes. It is used
as a non-secret observability, crash-reporting, and workspace-correlation
identifier.

`platformServicesIdSalt` is the single non-secret 128-bit namespace seed for
[ADR-132](../../adr/132-platform-services-project-salt-stable-id-tombstone-and-provider-mapping.md)
achievement, leaderboard, stat and presence-status IDs. Project creation obtains it
from the platform cryptographic random source and commits it with the project identity
and empty `.horo/platform_services.ids.json` ledger. Ordinary VCS/filesystem clones
preserve it. Only an explicit project-identity fork or reviewed namespace migration
may replace it; that transaction must remap every durable reference and provider
mapping rather than editing this field alone.

User activity such as `lastOpenedAt` does not belong in portable
`project.json`; it is stored in the user-level recent-projects model.

## Project Settings

Settings are typed and validated. They include:

| Setting              | Type              | Default            |
| -------------------- | ----------------- | ------------------ |
| `renderBackend`      | string            | `"opengl"`         |
| `physicsEnabled`     | bool              | `true`             |
| `targetFrameRate`    | int               | `60`               |
| `defaultScene`       | `ProjectPath`     | empty              |
| `assetCompression`   | string            | `"lz4"`            |
| `textureCompression` | string            | `"bc7"`            |
| `buildProfile`       | string            | `"desktop-debug"`  |
| `requiredToolchain`  | typed requirement | host/default/C++20 |

Settings are exposed through:

- editor settings panel
- CLI: `horo-engine project settings set --project <root> <key> <value>`
- MCP: `set_project_setting` tool

Invalid settings are rejected at load time with a typed error; the engine does
not silently fall back.

Settings persistence uses deterministic serialization, a sibling temporary
file, and atomic replacement. Concurrent writers use project identity and file
revision checks so one GUI, CLI, or MCP operation cannot silently overwrite a
newer settings commit.

## Project Collision Schema

[ADR-086](../../adr/086-collision-layer-profile-and-query-channel-policy.md)
assigns the Project domain one versioned `.horo/collision.json` authority. It stores
opaque stable layer, profile and query-channel IDs, one complete symmetric
simulation response matrix, complete reusable profiles and an explicit default
profile used only when an authoring command creates a collider.

Display names, editor colors and list order are presentation. Scene/collider data
references stable profile IDs and authored queries reference stable channel IDs;
neither stores array positions, names, native object layers or masks. Renames and
reorders preserve identity. Semantic edits produce a new immutable fingerprinted
generation, and deletion is blocked until durable references migrate.

Collision-schema saves use the same revision-checked project mutation lease,
deterministic serialization, sibling temporary file and atomic replacement as
other portable settings. Project-open validates the complete ID graph, matrix and
profile/channel tables before scene or Physics candidate publication. Runtime does
not fill missing entries, migrate legacy numeric/name data or fall back to the
authoring default.

## Toolchain Profiles

Portable project settings describe build intent and minimum toolchain
requirements. They do not store compiler paths, SDK installation paths, signing
identities, or machine-specific profile names.

User- or host-level toolchain profiles resolve those requirements to installed
tools:

```json
{
  "toolchainProfiles": {
    "linux-clang": {
      "targetPlatform": "linux",
      "compilerFamily": "clang",
      "generator": "Ninja",
      "compilerPath": "/usr/bin/clang++",
      "cmakePath": "/usr/bin/cmake"
    },
    "windows-msvc": {
      "targetPlatform": "windows",
      "compilerFamily": "msvc",
      "generator": "Visual Studio 17 2022",
      "compilerPath": "C:/Program Files/Microsoft Visual Studio/..."
    }
  }
}
```

Profiles are stored through the platform user-configuration directory. CI
provides equivalent profiles through validated invocation or CI configuration.
Resolution selects a compatible local profile from the portable project
requirements; a developer does not need a profile with an identical arbitrary
name.

Optional project-specific machine overrides live under
`.horo/local/toolchains.json` and are ignored by version control. Absolute paths
are legal only in user/CI/local-machine configuration, never in portable project
metadata. Signing identities and credentials remain credential references
resolved through the OS or CI credential provider.

## Editor Workspace Persistence

Editor workspace state is stored in `.horo/editor_workspace.json`. It includes:

- active scene path
- open tabs and active tab per tab stack
- layout-tree split ratios and collapsed state
- viewport camera state
- selected objects
- per-tab persisted UI state

### Content Browser projection

The Content Browser consumes an immutable, current-directory projection built
from the absolute project asset root and the authoritative Asset Registry
snapshot. Disk enumeration, registry identity, importer provenance, and preview
resolution belong to that projection; search text, the selected asset-type
filter, and name/type sort preferences are panel-owned presentation state.
Filtering and sorting return indices into the immutable directory snapshot and
never mutate registry or filesystem order.

Directory projection state is explicit (`Loading`, `Ready`, or `Error`). A
manual synchronous refresh first publishes `Loading`, yields a presentation
frame, and performs the bounded scan on the following workspace update. Registry
revision and authored mutation refreshes rebuild the projection and reconcile
back/forward history against existing absolute directories. Missing current
directories safely fall back to the absolute asset root. Selection is retained
only while its absolute entry remains visible in the current directory query.

Asset-card drag/drop carries only an absolute source path and a typed copy/move
intent into the workspace controller. Folder cards contribute absolute
destination directories; the panel never mutates the filesystem directly.
Normal drop uses the identity-preserving move operation, while `Cmd/Ctrl` drop
uses the new-identity copy operation. Context-menu, clipboard, drag/drop, `F2`,
and `Delete` entry points converge on the same typed mutation and confirmation
paths.

Authored Content Browser mutations validate portable entry names,
case-folded destination collisions, non-symlink companion sets, and canonical
asset-root containment before acquiring the shared project mutation lease.
Copy uses durable filesystem primitives; move, rename, and delete explicitly
roll back completed renames when I/O durability or a complete Asset Registry
publication fails. Delete publishes a unique project-local trash entry with a
durable `trash.json` manifest that records every original absolute path.

```json
{
  "schemaVersion": 1,
  "scenePath": "Assets/Scenes/main.horo",
  "selection": ["obj_wall_north"],
  "panelLayout": {
    "schemaVersion": 1,
    "closedTabs": [],
    "root": {
      "type": "split",
      "id": "workspace_root",
      "axis": "vertical",
      "ratio": 0.78,
      "first": {
        "type": "split",
        "id": "main_row",
        "axis": "horizontal",
        "ratio": 0.18,
        "first": {
          "type": "split",
          "id": "left_column",
          "axis": "vertical",
          "ratio": 0.62,
          "first": {
            "type": "tabStack",
            "id": "hierarchy_stack",
            "tabs": ["hierarchy"],
            "activeTab": "hierarchy"
          },
          "second": {
            "type": "tabStack",
            "id": "project_stack",
            "tabs": ["project"],
            "activeTab": "project"
          }
        },
        "second": {
          "type": "split",
          "id": "content_row",
          "axis": "horizontal",
          "ratio": 0.78,
          "first": {
            "type": "panel",
            "id": "viewport_panel",
            "panel": "viewport"
          },
          "second": {
            "type": "tabStack",
            "id": "properties_stack",
            "tabs": ["properties"],
            "activeTab": "properties"
          }
        }
      },
      "second": {
        "type": "tabStack",
        "id": "bottom_tools",
        "tabs": ["assets", "console", "mcp", "performance"],
        "activeTab": "console"
      }
    }
  },
  "viewport": {
    "eye": [10, 10, 10],
    "target": [0, 0, 0]
  },
  "tabs": {
    "hierarchy": {
      "schemaVersion": 1,
      "payload": {
        "searchFilter": "",
        "expandedNodes": ["Room"]
      }
    }
  }
}
```

The complete workspace document is owned by `EditorWorkspaceController` and
persisted on editor close or explicit save. Ownership of its runtime slices is:

- `EditorPanelHost`: layout tree, tab placement, active tabs, and collapsed state
- `EditorSelectionModel`: selected objects and selected asset
- `EditorViewportModel`: editor camera and navigation state
- individual tabs: project-scoped presentation state under their stable tab ID

Sequence-document tabs follow
[ADR-121](../../adr/121-cinematic-editor-document-and-authoring-context.md).
Workspace state may retain their open route, active tab, timeline selection,
zoom/scroll/playhead and detachable authoring-scene route by stable asset/tab
identity. It never stores sequence content, dirty/history/save state, live scene
handles or preview authority leases. A restored missing sequence/scene asset produces
a typed diagnostic and omits/detaches that route without modifying either document.

`EditorViewportModel` owns a backend-neutral `EditorViewportCamera`, advances a
monotonic viewport revision after committed navigation or explicit preview
invalidation, and publishes `ViewportChangedEvent`. Camera navigation and gizmo
preview state are workspace state; neither is serialized into the scene document
or added to scene undo history.

The controller gathers these slices into one versioned document and restores
them in dependency order. Workspace state is not part of the scene document and
is not versioned with scene changes.

Workspace restore is best-effort and must not prevent project opening:

- unsupported or corrupt workspace documents fall back to the versioned default
  layout
- deleted scenes are not opened and produce a typed diagnostic
- missing tabs and panels are reported and skipped while bounded opaque plugin
  state is preserved where supported
- missing selected objects or assets are removed when selection is reconciled
- invalid per-tab state is ignored without discarding otherwise valid layout
  state

The restored state is normalized before the next workspace save so stale
references do not persist indefinitely.

Open editor modals, their navigation step, focus, draft form values, and child
stack are transient GUI state and are not stored in the workspace document.

## Asset Index

`asset_index.json` is a derived lookup registry for imported assets:

```json
{
  "schemaVersion": 1,
  "assets": {
    "a1b2c3d4-e5f6-4890-abcd-ef1234567890": {
      "assetId": "a1b2c3d4-e5f6-4890-abcd-ef1234567890",
      "assetType": "core.mesh",
      "sourcePath": "Assets/Models/cube.fbx",
      "metadataPath": "Assets/Models/cube.fbx.horo"
    }
  }
}
```

The committed sidecar next to each source asset is the source of truth for:

- the single canonical UUID `assetId`
- metadata schema and importer version
- import and cook settings
- dependency identities

Scene documents reference the stable logical asset ID. `asset_index.json` may be
ignored because rebuilding it scans source assets and their committed sidecars;
rebuild never generates replacement IDs for valid sidecars.

Runtime conversion emits canonical typed scene dependencies, not source paths.
At activation, `RuntimeSceneService` pins the current immutable registry
snapshot, validates expected asset types, and loads cooked bytes through the
runtime provider. Registry revision changes reject the candidate at the
owner-thread safe point and preserve the previous active scene. The editor
document and project model never retain process-local payload leases.

The index is loaded as one bounded all-or-nothing candidate. Malformed,
version-skewed, or globally ambiguous index content is never partially applied;
the project attempts a sidecar rebuild and retains the last valid in-memory
snapshot if publication fails. Read-only project access does not rewrite the
derived index. Edit-mode rebuild uses deterministic serialization and atomic
replacement.

Rebuild is deterministic and transactional:

- deleted source assets remove derived path entries but do not silently rewrite
  scene references
- moved assets retain identity when the sidecar moves with the source
- a source without a sidecar remains untracked and does not receive an ID
- missing/invalid identity, malformed/future sidecar, and orphan-sidecar cases
  remain distinguishable typed diagnostics; valid records may publish degraded
- duplicate ID, duplicate canonical path, or portable case collision rejects
  the entire candidate rather than selecting one entry
- changed import settings invalidate the affected imported/cooked cache entries
- stale generated outputs are excluded until their source and sidecar validate

`AssetImportService` writes or updates the sidecar and commits the derived index
only after a successful import transaction. Developers do not edit the index
manually.

## Scene Documents

Scene documents live in `Assets/Scenes/` for new projects, or the existing
`assets/scenes/` directory for legacy projects, and are editor-authorable files. They
store:

- object hierarchy
- components and their properties
- asset references by logical ID
- editor-only metadata (gizmo state, camera bookmarks)

Scene documents are converted to runtime definitions before entering the
runtime. See [System Design](../foundation/system-design.md) for the scene boundary.

## Project Operations

Application use cases own project-level operations:

- `CreateProject`
- `OpenProject`
- `CloseProject`
- `SaveProjectSettings`
- `MigrateProject`
- `ValidateProject`

GUI, CLI, and MCP adapters invoke these same use cases where the operation is
exposed by that host. Host availability and presentation routing are explicit;
an adapter does not reimplement project business rules.

`OpenProject` opens or validates an application-level project session. GUI route
navigation remains owned by `GuiScreenHost`; an application use case never
silently replaces the active screen.

## Project Validation

Validation is operation-specific:

```cpp
enum class ProjectValidationMode {
    ReadOnly,
    Edit,
    Build
};
```

All modes require:

- `.horo/project.json` exists and parses correctly
- `.horo/collision.json` exists and its typed ID graph, symmetric matrix and
  complete profile/channel tables validate, or a migration planner can produce it
- `horoVersion` and `persistentContract` form a known compatible decision, or a
  migration planner can produce a valid path
- `projectId` is present
- referenced default scene exists or is empty
- asset sidecars can be scanned and the index can be loaded or reconstructed

`ReadOnly` requires only readable project inputs and supports project browsing,
information queries, validation, and read-only CI checks. A missing index is
reconstructed in memory without writing project state.

`Edit` validates writability per requested mutation: saving settings requires a
writable `project.json`, saving the collision schema requires writable
`collision.json`, saving a scene requires that scene destination, and importing
requires the source sidecar and derived-index destination. Failure to persist
optional workspace state is a warning and does not make an otherwise editable
project invalid.

`Build` requires a compatible toolchain resolution and writable build/output
directories. A read-only source checkout remains valid when build outputs and
caches are redirected to writable locations.

Validation produces typed diagnostics, not silent failures.
Portable validation also detects project paths that collide under supported
case-folding rules even when the current filesystem would allow both names.

## Migration

When the project's contract baseline is older than the current supported
baseline, migration use cases transform the project:

```text
project v1  ->  project v2  ->  project v3
```

Migrations are:

- deterministic
- logged
- reversible when possible
- tested with fixture projects for each supported version

Migration writes use staged files and atomic replacement. A failure preserves
the pre-migration portable metadata and reports the failing transformation.
Unknown future versions are never migrated downward or opened as writable
projects.

## Recent Projects

The graphical host maintains a recent projects list in the platform user-state
directory:

```text
<user-state>/horo/recent_projects.json
```

Entries include:

- project root path
- project name
- last opened timestamp
- a project thumbnail cache key when available

The list is bounded (default 20 entries) and prunes missing or invalid paths
lazily.

Project thumbnails live under the platform user-cache directory and are looked
up by cache key, normally derived from `projectId`, a canonical-root hash, and
thumbnail revision. The recent-projects file does not persist arbitrary
absolute thumbnail paths.

Renderer component health is not copied into `recent_projects.json`. The project
model reads the project's stable renderer ID and joins it with a current
machine-local renderer component snapshot to produce transient card preflight
state. Removing or repairing a renderer therefore updates card state without
rewriting the recent-project list or the project.

## Project Browser Screen

The project-browser screen inside `HoroEditor` uses the project model to:

- list recent projects
- create new projects from templates
- open existing projects
- validate project before opening
- resolve missing, unhealthy, or incompatible requested renderer components
  before workspace navigation

It does not load scene data or editor workspace state until the editor workspace
is activated. It is a screen in the graphical host, not a separate launcher
module or lifecycle.

If the requested renderer is unavailable, opening the card remains in the
Welcome/Project Browser route and starts the renderer-resolution dialog. The
normal GUI offers installation of the requested renderer or one explicit
persistent change to a compatible available renderer. It does not offer a second
session-only renderer action. Persistent changes use the same revision-checked,
transactional project settings operation as other adapters.

See
[Renderer Distribution And Availability](../runtime/renderer-distribution-and-availability.md#recent-project-renderer-preflight)
for the state and capability-validation contract.

## CLI Commands

```bash
# Create a new project
horo-engine create-project MyGame --template empty

# Open a project (validates and prints info)
horo-engine project info /path/to/MyGame

# Set a project setting
horo-engine project settings set --project /path/to/MyGame renderBackend vulkan

# Validate a project
horo-engine project validate /path/to/MyGame
```

## MCP Tools

- `create_project`
- `set_project_setting`
- `get_project_info`
- `validate_project`
- `open_project_session` (headless host)
- `open_project_workspace` (GUI host)

`get_project_info` and `validate_project` are read-only and do not change the
active project or GUI route. `open_project_session` opens an application-level
session in the headless host.

`set_project_setting` identifies an explicit project root or validated active
session and uses the same revision-checked `SaveProjectSettings` operation as
the GUI and CLI.

`open_project_workspace` is a GUI-only adapter over `GuiScreenHost::Navigate`.
It passes through route validation, dirty-document leave guards, running-work
policy, native-dialog deferral, and modal interaction policy. MCP transport code
never mutates GUI route state directly. Stateful open requests return typed
`Busy`, leave-denied, cancellation, or entry-failure results.

## Security And Portability

### Project source-file policy and open routing

All Asset Browser activations, build diagnostics, and editor commands submit one
`SourceOpenRequest` to `SourceFileOpenService`. The request carries its origin,
optional line/column, and whether an external fallback is permitted; callers do
not validate or launch a path themselves.

`SourceFilePolicy` classifies case-insensitive extensions into native source,
Horo Script, project text, or unsupported. The policy also owns project-specific
file-name/extension additions and the availability of the embedded and external
editor routes. A supported file opens through the embedded workspace and uses the
existing `DocumentIdentityRegistry`, so relative, absolute, and project-contained
symlink paths focus one canonical source document. An unsupported file may only
use the explicit external fallback when the request allows it; `EmbeddedOnly`
requests return a typed unsupported result.

Before classification or fallback, the service normalizes the project root and
input path, rejects lexical traversal outside the root, resolves existing path
components and symlinks, and requires the final regular file to remain inside
the canonical project root. Internal symlinks are allowed by policy and resolve
to the target identity; escaping symlinks, missing files, unavailable editor
capabilities, and unsupported routes return distinct typed source-open errors.

- Portable project files store normalized forward-slash `ProjectPath` values.
  Absolute paths, drive roots, UNC roots, and `..` traversal are rejected.
- Lexical normalization must not escape the project root. For an existing path,
  security-sensitive operations resolve symlinks and reject a target outside the
  canonical project root. For a new write target, the nearest existing parent is
  resolved and checked before creation.
- Symlinks that resolve inside the project root may be read only when the owning
  operation permits them. Import, delete, migration, and build-output operations
  reject symlink ambiguity by default.
- External files require an explicit user-approved external-reference type;
  ordinary project paths never gain external access implicitly.
- Portable project files must not contain secrets, credentials, signing key
  paths, compiler installation paths, or API tokens.
- Signing identities and credentials use opaque references resolved by the OS or
  CI credential provider.
- Workspace state is user-specific and should not be committed to version
  control.
- `.horo/project.json`, `.horo/packages.json`, `.horo/packages.lock`,
  `.horo/input.json`, `.horo/collision.json`, and asset metadata sidecars are
  portable and may be committed. `.horo/editor_workspace.json`,
  `.horo/asset_index.json`, `.horo/local/`, and generated build/cache output
  should be ignored.

Recommended `.gitignore`:

```gitignore
.horo/editor_workspace.json
.horo/asset_index.json
.horo/local/
build/
```

## Testing

Required coverage:

- project format and product version remain independent
- unknown future `horoVersion` values fail without mutation unless an exact,
  trusted same-release-line compatibility proof is verified
- each supported migration fixture upgrades deterministically and atomically
- failed migration preserves the original portable metadata
- `defaultScene` resolves from the project root using the stored spelling, such as `Assets/Scenes/...`
- settings exist in one canonical location and concurrent saves detect revision
  conflicts
- read-only validation succeeds without writing an index or workspace state
- edit and build validation check only the writable targets required by the
  requested operation
- missing index rebuild preserves sidecar asset IDs and import settings
- duplicate, missing, corrupt, and moved sidecars produce the documented results
- scene logical asset references survive deterministic index rebuild
- corrupt or stale workspace state cannot prevent project opening
- missing scenes, selections, tabs, and panels are skipped with diagnostics
- local toolchain resolution works with different profile names on two machines
- portable project files reject absolute paths, traversal, and case collisions
- symlink containment is enforced for reads, writes, imports, deletes, and build
  outputs
- recent-project thumbnails resolve through user cache keys
- GUI MCP project open passes through navigation leave guards
- headless project open does not mutate GUI route state

## Related Documents

- [Project Settings UI Reference](./project-settings.html)

- [New Project Wizard](./new-project-wizard.html): HTML reference design for
  project creation, template selection, path validation, and initial settings.
- [System Design](../foundation/system-design.md): host and module boundaries.
- [Asset Pipeline](../runtime/asset-pipeline.md): source to cooked asset flow.
- [Configuration System](../foundation/configuration-system.md): project setting schema,
  precedence, validation, and immutable snapshots.
- [Editor Document Model](./editor-document-model.md): scene save, autosave,
  recovery, and external-file conflict behavior.
- [Editor Panel Host](./editor-panel-host.md): workspace layout model.
- [GUI Screen Host](./gui-screen-host.md): project-browser and editor-workspace
  route lifecycle.
- [Platform Abstraction](../foundation/platform-abstraction.md): structured paths,
  user directories, atomic replacement, and symlink policy.
- [MCP Architecture](../interfaces/mcp-architecture.md): host-specific tool
  availability and main-thread dispatch.
- [Release Architecture](../release/release.md): packaging project artifacts.
- [Horo Package System](../packages/package-system.md): project package dependencies and lockfile
- [Observability Architecture](../observability/observability.md): safe project correlation and
  game-specific log storage.
