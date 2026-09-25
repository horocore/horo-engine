# Asset Pipeline

## Purpose

This document defines the asset pipeline for Horo Engine: how source assets
flow from raw files on disk into runtime-ready artifacts inside a packaged
release.

The pipeline is host-agnostic. The same importers, cookers, and packagers are
used by the GUI, CLI, and MCP.

Animation assets follow the generic identity, orchestration, cache, publication,
and byte-provider authority here. Animation-specific source selection, schemas,
dependencies, compression, cooked payloads, and diagnostics are owned by the
[Animation Asset Pipeline Contract](./animation-asset-pipeline-contract.md).
Assets must not interpret joints, tracks, root motion, or compression profiles.

Virtual-texture sources follow this same authority under
[ADR-164](../../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md):
the pipeline publishes immutable target/capability-keyed artifacts and bounded byte
leases. The VTX runtime neither constructs cache/package paths nor discovers loose
files, and the asset pipeline does not own runtime page demand or GPU residency.

## Asset Lifecycle

```text
Source Asset
    |
    v
Importer + Metadata
    |
    v
Editor Asset (authoring representation)
    |
    v
Cooker (per platform target)
    |
    v
Cooked Asset (runtime-ready)
    |
    v
Packager (chunks/bundles)
    |
    v
assets.horo (release archive)
```

## Asset Identification

Every imported asset has a **stable logical asset ID**. The ID must be:

- unique within the project
- independent of the source file path
- independent of the source file name
- stable across renames and moves
- encoded as one canonical lowercase UUID in the sidecar `assetId` field

The importer assigns the random 128-bit ID once and commits it to the sidecar:

```cpp
AssetId GenerateAssetId(const std::filesystem::path& projectRelativePath,
                        const std::array<uint8_t, 16>& randomGuid) {
    // randomGuid is generated once and stored in the sidecar metadata.
    // It survives renames and moves as long as the sidecar travels with the asset.
    return AssetId::FromBytes(randomGuid);
}
```

There is no parallel `assetGuid` identity. `assetId` is the single authority.
The derived `.horo/asset_index.json` stores both ID and normalized project path
lookups, but it never invents an ID and may always be rebuilt from valid
sidecars.

## Implemented AST-001A Baseline

`HoroEngine::Assets` is a backend-neutral target depending only on Foundation.
The implemented baseline contains canonical `AssetId`/`AssetTypeId` parsing,
immutable registry snapshots, sidecar rebuild, deterministic derived-index
storage, synchronous providers, and a bounded asynchronous load service over
the process `JobSystem`.

Registry candidates are mutable only on their owner thread while being built.
Publish first validates the complete candidate and then atomically replaces one
immutable shared state. Snapshots pin that state and are safe for concurrent,
allocation-free ID/path lookup while later revisions publish. A global duplicate
ID, duplicate canonical path, or portable case collision rejects the candidate
and preserves the last valid snapshot.

Project open uses the explicit two-stage form: the operation worker calls
`PrepareAssetRegistryCandidate()` to scan committed sidecars into an unpublished
candidate; the application owner thread first writes the deterministic derived
index and only then publishes the validated immutable registry revision. The
worker never replaces the registry consumed by runtime/editor readers.

An asynchronous request captures an `AssetRecord` and registry revision at
submission; workers never access mutable registry state. Registry replacement
therefore cannot invalidate an in-flight read. Completion carries its source
revision so a later authoritative installation boundary can reject stale work.
Service shutdown stops admission, cancels and joins owned requests, and prevents
callbacks after service destruction.

AST-001A does not implement cooking, archives, cache, or hot reload. The editor
Content Browser combines the immutable registry snapshot with a bounded
directory enumeration used strictly for presentation. Files without canonical
identity metadata remain visibly marked as unregistered and never become
runtime-resolvable assets by appearing in the browser. A successful editor
import writes the canonical `<asset>.horo` identity sidecar, rebuilds and
publishes the registry, and the active workspace replaces its Content Browser
projection when that revision changes. Runtime-scene consumption is the
separate AST-001B slice described below.

Content Browser navigation state uses canonical absolute native paths. It
enumerates direct child directories and visible non-sidecar files beneath the
absolute project `assets` root, decorates matching files from the pinned
registry snapshot, and exposes absolute breadcrumb targets. Relative navigation
and `..` segments are never presented to the UI. Rename and recoverable-delete
operations hold the shared project mutation lease, move payload and sidecars as
one logical entry, and rebuild the registry before publishing the refreshed
view. Portable sidecars and the derived asset index retain normalized
project-relative `ProjectPath` values for cross-machine portability.

Importer contributions may attach an `IAssetPreviewProvider`. The host passes
the bounded imported editor payload, absolute diagnostic path, stable asset
type, requested dimensions, and cancellation token. A provider returns only an
owned tightly packed RGBA8 image; ImGui callbacks, renderer handles, backend
types, texture ownership, and arbitrary card UI do not cross the module
boundary. The Content Browser validates the image, uploads and caches it through
the selected GUI renderer, and destroys the texture when the directory or panel
lifetime ends. Missing or failed providers select host-owned mesh, image, audio,
or generic fallbacks by declared asset type.

`AssetPreviewService` is the host-owned execution boundary. It admits a bounded
number of requests to the process `JobSystem`, reads regular files in cancellable
chunks under an explicit byte limit, and rejects invalid dimensions or RGBA8
output before publication. Every accepted request retains a shared provider
lease until its job reaches one terminal state, so package/catalog replacement
cannot unload code underneath an invocation. Shutdown stops admission, requests
cancellation, joins all accepted work, and only then releases provider leases.

Successful images are cached in a bounded in-memory LRU using a host-computed
key over the owning module identity/version, contribution identity/version,
asset type, requested dimensions, and exact payload digest. Absolute paths are
diagnostic only and do not affect reuse. Editor navigation cancels obsolete handles; a late completion is applied
only when the current card still has the same absolute path, contribution, and
provider version. GPU texture caching remains a separate renderer-owned layer.

Identity sidecars retain the importer contribution ID so asset types shared by
multiple importers resolve the provider that authored the payload. This field is
advisory for editor presentation; runtime identity and dependency resolution
continue to use `AssetId` and `AssetTypeId`.

The built-in FBX importer uses a pinned ufbx scene decoder with bounded parser
allocations. It combines every mesh instance, applies each node's geometry-to-world
transform, triangulates polygon faces, and emits the same versioned mesh editor
payload consumed by the built-in preview provider. The provider depth-rasterizes
shaded triangle surfaces; topology-free legacy payloads use the typed mesh fallback
instead of fabricating a point-cloud preview. Malformed scenes fail import with a
typed diagnostic instead of committing an opaque payload.

## Implemented AST-001B Runtime Scene Consumption

`HoroEngine::RuntimeScene` depends one-way on `HoroEngine::Assets` and resolves
versioned scene dependencies through the AST-001A snapshot/provider contracts.
The scene definition stores only stable AssetId and expected AssetTypeId values;
it never stores a source path, process-local provider handle, or mutable registry
record.

`QueuePreparation` captures an immutable registry snapshot and validates the
complete dependency set before starting I/O. Each accepted async request carries
the captured registry revision. Workers read provider-owned cooked bytes without
re-entering the registry. Registry replacement therefore cannot invalidate
in-flight memory, while the owner-thread activation boundary can still reject a
logically stale candidate by comparing revisions.

Loads are admitted in configurable bounded waves. The default candidate limits
are 1,024 dependencies, eight concurrent loads, and one GiB of logical resident
payload. Budget checks are incremental and fail fast. Reused payload leases and
new payloads both count once toward the candidate budget; physical storage
shared with the active scene is not copied. Matching AssetId/type payloads may
be reused independently when the registry revision is unchanged.

Every declared dependency is required in AST-001B. Missing records, type
mismatch, empty payload, provider failure, cancellation, queue pressure, budget
overflow, or authoritative revision change prevents activation and preserves
the previous active scene. No fallback asset or partial scene activation is
invented. `RuntimeSceneView` exposes allocation-free lookup of the opaque cooked
bytes; decoding and typed resource installation belong to later asset/material/
renderer slices.

## Localized Catalog And Variant Assets

[ADR-081](../../adr/081-runtime-ui-and-localization-ownership-boundary.md) keeps
Assets responsible for stable identity, dependency graphs, cook/package
publication and immutable byte transport. Localization owns catalog parsing,
semantic validation, message-format/CLDR compatibility fingerprints and locale
fallback; Assets never selects or formats a translated message.

Localized visual/audio variants are finite authored mappings from normalized
locale profiles to stable `AssetId` values. The owning feature declares Required,
UseNeutral or Omit behavior, Localization supplies the fallback chain, and Assets
resolves only declared variants. Cook closure includes required catalogs, source/
product fallbacks, locale-specific font policy and localized asset variants; no
runtime path scan, source import, download or undeclared substitute is allowed.

### Collision Rules

- Two source files with the same name in different directories are independent
  assets and receive independent IDs.
- A duplicate ID or canonical path is a global ambiguity: no candidate is
  published and the previous registry remains active.
- Paths are compared both canonically and with a portable case fold so a project
  cannot become ambiguous on a case-insensitive filesystem.
- A source without a sidecar is untracked and excluded with
  `asset.registry.sidecar_missing`; AST-001A never generates an ID.
- Missing/empty identity, invalid UUID, malformed JSON, unsupported schema, and
  an orphan sidecar are distinct typed diagnostics. These record-local failures
  may publish one degraded snapshot containing the remaining valid records.

## Source Assets

Source assets are files created by external tools:

| Type          | Extensions                             |
|---------------|----------------------------------------|
| meshes        | `.fbx`, `.obj`, `.gltf`, `.glb`        |
| textures      | `.png`, `.jpg`, `.tga`, `.exr`, `.hdr` |
| materials     | `.horomat`                             |
| shaders       | `.vert`, `.frag`, `.comp`, `.hlsl`     |
| shader graphs | `.horoshadergraph`                     |
| scenes        | `.horo`                                |
| prefabs       | `.prefab`                              |
| audio         | `.wav`, `.ogg`                         |

Built-in scene primitives (cube, sphere, capsule, etc.) are not source assets.
They are described by `PrimitiveMeshDescriptor` and resolved by the engine
without import. See [Built-In Scene Primitives](./built-in-scene-primitives.md).

Source assets live in the project directory under `assets/`. They are not
shipped in a release unless explicitly requested by a developer package
profile.

## Import

Importing reads a source asset and produces an editor-side representation.
Import is triggered by:

- GUI drag-and-drop into the asset browser
- CLI: `horo-engine asset import <path>`
- MCP: `import_asset` tool
- project open / asset discovery scan

### Importer Interface

```cpp
class IAssetImporter {
public:
    virtual ~IAssetImporter() = default;

    virtual std::vector<std::string> SupportedExtensions() const = 0;
    virtual ImportResult Import(const std::filesystem::path& sourcePath,
                                const ImportOptions& options) = 0;
};
```

### Import Result

```cpp
struct ImportResult {
    std::string assetId;          // stable logical identifier
    std::string assetType;        // mesh, texture, material, ...
    std::filesystem::path importedPath;
    AssetMetadata metadata;
    std::vector<std::string> generatedDependencies;
    std::vector<ImportDiagnostic> diagnostics;
};
```

Each importer registers itself in `AssetImportService`:

```cpp
AssetImportService::RegisterImporter(std::make_unique<FbxImporter>());
AssetImportService::RegisterImporter(std::make_unique<ObjImporter>());
AssetImportService::RegisterImporter(std::make_unique<PngImporter>());
```

### Import Concurrency

Importers are stateless and thread-safe. The pipeline executes independent
imports concurrently using the engine job system. The default concurrency is
capped by the number of available hardware threads, with a lower priority than
runtime frame work.

```cpp
class AssetImportService {
public:
    std::vector<ImportResult> ImportMany(const std::vector<std::filesystem::path>& paths,
                                         const ImportOptions& options);
};
```

`ImportMany` schedules one job per asset, gathers results, and reports a single
aggregate diagnostic list. Ordering is preserved only when the caller requests
it.

## Future Parallel Import And Cook

Large projects may contain thousands of assets. Sequential import and cook is
unacceptable. The pipeline schedules independent import and cook jobs on the
engine job system.

This job-graph orchestration and the aggregate `CookReport` are future
architecture outside AST-001C. AST-001C retains bounded structured concurrency
for its supported operation but does not implement dependency-graph scheduling.

Rules:

- Import jobs for different source files run concurrently.
- Cook jobs run concurrently once their dependencies are cooked.
- Dependency edges enforce ordering: a material cannot cook before its textures.
- The job graph is rebuilt from the asset dependency graph on every cook
  command.

```cpp
class AssetCookService {
public:
    // Cook all assets for a platform target using the job system.
    CookReport CookAll(const CookProfile& profile,
                       const std::vector<CookTarget>& targets);
};
```

The cook report contains per-asset success/failure, timing, cache hits, and
invalidated dependencies.

## Metadata

Every imported asset has a sidecar metadata file:

```text
assets/
    models/
        cube.fbx
        cube.fbx.horo          // metadata
```

Metadata includes:

- logical asset ID
- source file hash
- stable importer contribution identity and version
- owning package/module identity and module version
- absolute original source path used by the editor reimport workflow
- metadata schema version
- import timestamp
- serialized importer settings required to reproduce the import
- dependencies
- typed reasons for the last import transaction

```json
{
  "schemaVersion": 1,
  "assetId": "a1b2c3d4-e5f6-4890-abcd-ef1234567890",
  "assetType": "core.mesh",
  "importerContributionId": "com.vendor.fbx-importer.importer",
  "importerVersion": "1.2.0",
  "importerPackageId": "com.vendor.fbx-importer",
  "importerModuleId": "com.vendor.fbx-importer.native",
  "importerModuleVersion": "2.0.0",
  "absoluteSourcePath": "/Users/developer/project-source/cube.fbx",
  "sourceExtension": "fbx",
  "sourceHash": "sha256:abc123...",
  "sourceByteSize": 421337,
  "importSettings": {
    "settings.coordinateSystem": "0",
    "settings.unitScale": "1.0"
  },
  "dependencies": [
    "8c643d5e-708d-4b00-b326-7a621a42ee54"
  ],
  "lastImportReasons": ["source_changed", "module_changed"],
  "importedAtUtc": "2026-07-27T00:00:00.000+00:00"
}
```

The canonical `assetId` is the stable identity. The sidecar file name follows
the source asset name; moving source and sidecar together preserves identity.

Rebuild is all-or-nothing at the snapshot boundary. A malformed or
version-skewed derived index is never partially consumed; it is discarded and a
sidecar rebuild is attempted. Read-only rebuild publishes only memory state.
Edit-mode rebuild may atomically replace `.horo/asset_index.json`. Filesystem
scans reject root escape and symlink ambiguity before opening source metadata.

## Schema Migration

Metadata sidecar files carry a `schemaVersion` field. When the importer or the
metadata schema changes, the pipeline compares the stored version with the
current version:

```cpp
enum class MigrationAction {
    None,       // schema is current
    Reimport,   // importer changed, re-import from source
    Upgrade,    // metadata format changed, rewrite sidecar in-place
    Error       // unsupported old version, manual intervention required
};
```

Rules:

- Explicit reimport resolves the exact recorded `importerContributionId`; a
  different contribution is never selected merely because it claims the same
  extension.
- Source bytes are hashed before import. A hash change records
  `source_changed`; a contribution-version change records `importer_changed`;
  and an owning module identity/version change records `module_changed`.
- When none of those values changed, an explicit user action records
  `manual_reimport`.
- Reimport preserves `assetId`, restores the serialized importer settings, and
  atomically replaces payload plus sidecar. Publication failure restores the
  previous pair.
- If only the sidecar `schemaVersion` changed and the importer is the same, the
  metadata is upgraded in-place without touching the source asset.
- If the old schema is no longer supported, a diagnostic error is emitted and
  the asset is marked for manual re-import.

```cpp
MigrationAction AssetMigrationService::Evaluate(const AssetMetadata& metadata);
```

Automatic migration policy remains future work. The implemented editor action
is explicit and never silent: the Asset Info surface presents stored/current
provenance and the structured log reports every completed reimport.

## Asset Configuration

Every imported asset carries two distinct configuration blocks in its sidecar
metadata:

- **Import settings**: how the source file is interpreted.
- **Cook settings**: how the editor asset is transformed into runtime artifacts.

This separation keeps interpretation concerns separate from runtime optimization
concerns. It also matches how artists and technical artists think about assets:
"What is this file?" is different from "How do we ship it?"

```json
{
  "schemaVersion": 1,
  "assetId": "a1b2c3d4-e5f6-4890-abcd-ef1234567890",
  "assetType": "core.texture",
  "importSettings": {
    "colorSpace": "sRGB",
    "wrapMode": "Repeat"
  },
  "cookSettings": {
    "generateMipmaps": true,
    "mipmapFilter": "Box"
  }
}
```

### Texture Configuration

| Setting           | Block  | Description                                                                     |
|-------------------|--------|---------------------------------------------------------------------------------|
| `colorSpace`      | import | `sRGB`, `Linear`, `HDR`. Defines how shaders sample the texture.                |
| `wrapMode`        | import | `Repeat`, `Clamp`, `MirrorRepeat`. Written into the sampler descriptor.         |
| `generateMipmaps` | cook   | Whether to build a mipmap chain.                                                |
| `mipmapFilter`    | cook   | `Box`, `Lanczos`, `Kaiser`.                                                     |
| `compression`     | cook   | `BC7`, `ASTC4x4`, `ETC2`, `None`. Defaults come from the active `CookProfile`.  |
| `maxResolution`   | cook   | Per-asset resolution cap. Final size is `min(source, profile.max, target.max)`. |

### Mesh Configuration

| Setting            | Block  | Description                                                  |
|--------------------|--------|--------------------------------------------------------------|
| `coordinateSystem` | import | `YUp`, `ZUp`. Normalized during import.                      |
| `unitScale`        | import | Scale factor from source units to engine units.              |
| `importNormals`    | import | `Imported`, `Calculate`, `CalculateSmoothed`.                |
| `meshCompression`  | cook   | `None`, `Draco`, `MeshOpt`. Platform profile may override.   |
| `generateTangents` | cook   | Required for normal-mapped meshes.                           |
| `lodSettings`      | cook   | Optional automatic LOD generation. Stored as derived assets. |

### Material Configuration

| Setting             | Block | Description                                                    |
|---------------------|-------|----------------------------------------------------------------|
| `blendMode`         | cook  | `Opaque`, `Masked`, `Translucent`, `Additive`.                 |
| `parameterDefaults` | cook  | Default color, float, vector, and texture slot values.         |
| `shadingModel`      | cook  | `Lit`, `Unlit`. Expanded later as more models are implemented. |

### Shader Configuration

| Setting        | Block | Description                                                                    |
|----------------|-------|--------------------------------------------------------------------------------|
| `entryPoints`  | cook  | Map of stage to function name: `{ "vertex": "vsMain", "fragment": "fsMain" }`. |
| `targetStages` | cook  | Declared stages to compile for each admitted target; `Compute` requires explicit capability support. |
| `defines`      | cook  | Preprocessor macros included in the variant key.                               |

### Audio Configuration

| Setting       | Block  | Description                                                     |
|---------------|--------|-----------------------------------------------------------------|
| `channels`    | import | `Mono`, `Stereo`. Spatialized sounds must be mono.              |
| `compression` | cook   | `PCM`, `Vorbis`, `Opus`. Profile and target aware.              |
| `sampleRate`  | cook   | `44100`, `22050`. Final rate is `min(source, profile, target)`. |

### Scene Configuration

| Setting           | Block  | Description                                                            |
|-------------------|--------|------------------------------------------------------------------------|
| `referenceMode`   | import | `Embed` or `Reference`. Large scenes should reference external assets. |
| `chunkAssignment` | cook   | Which release chunk this scene belongs to.                             |

### Cook Profiles

A `CookProfile` defines platform-specific defaults. It lives in the project
settings and is selected by `--profile` on the command line.

Target-profile selection is future architecture outside AST-001C. The examples
below preserve that contract without claiming that these profiles or their
desktop targets are currently accepted.

```json
{
  "id": "pc-low",
  "textureDefaults": {
    "maxResolution": 1024,
    "compression": "BC7",
    "generateMipmaps": true
  },
  "meshDefaults": {
    "maxLOD": 2,
    "meshCompression": "MeshOpt"
  },
  "audioDefaults": {
    "compression": "Opus",
    "sampleRate": 22050
  }
}
```

```json
{
  "id": "pc-high",
  "textureDefaults": {
    "maxResolution": 4096,
    "compression": "BC7",
    "generateMipmaps": true
  },
  "meshDefaults": {
    "maxLOD": 4,
    "meshCompression": "None"
  },
  "audioDefaults": {
    "compression": "Vorbis",
    "sampleRate": 44100
  }
}
```

### Configuration Precedence

Settings are resolved in the following order, from lowest to highest priority:

1. **Cook profile** — platform-specific defaults.
2. **Folder rules** — per-folder overrides defined in project settings.
3. **Asset sidecar** — per-asset overrides in the `.horo` metadata file.

```cpp
nlohmann::json ResolveConfig(AssetType type,
                              const CookProfile& profile,
                              const std::vector<FolderRule>& folderRules,
                              const AssetMetadata& sidecar);
```

A higher-priority override replaces a lower-priority value. Missing values fall
back to the next level.

### Folder Rules

Folder rules apply overrides to every asset of a given type under a project
path.

```json
{
  "folderRules": [
    {
      "path": "assets/ui",
      "types": [
        "texture"
      ],
      "overrides": {
        "cookSettings": {
          "maxResolution": 512,
          "compression": "BC7"
        }
      }
    },
    {
      "path": "assets/audio/sfx",
      "types": [
        "audio"
      ],
      "overrides": {
        "cookSettings": {
          "compression": "Opus",
          "sampleRate": 22050
        }
      }
    }
  ]
}
```

Folder rules are evaluated after the cook profile and before the asset sidecar.
They are useful for bulk policy changes without editing hundreds of sidecar
files.

## Editor Asset

Editor assets are the authoring representation. They are stored in a canonical
intermediate format that preserves editable properties. For example:

- a mesh editor asset stores raw vertex data and material slots
- a texture editor asset stores the uncompressed image and import settings
- a material editor asset stores parameter values and texture references

Editor assets are not loaded by the runtime directly. They must be cooked.

## Cooking

Cooking transforms tracked editor assets into deterministic runtime artifacts.
AST-001C is deliberately scoped as a backend-free proof of this boundary. Once
implemented, this slice will admit only the `headless-null` **cook-target ID**.
That ID is not the Null renderer and must not make the generic cooker or cook
operation renderer-dependent. Its artifacts will contain validation-only data
usable by headless applications and by runtime compositions that use the Null
renderer. This slice must not parse importer formats or create GPU resources.

OpenGL, Metal, Vulkan, and future interactive backends are equal future cook-
target peers. A target becomes available only when its owning module
provides a typed renderer capability contract that the host validates and
activates. No renderer is the base, default, or fallback for another, and an
unavailable target is a typed error rather than an implicit substitution.

### Modular Cooker Catalog

The canonical host-owned authority invariant is: the host owns bounded immutable
source storage, the typed cooker registry, cache keys, output placement, operation
state, and publication. Cooker contributions claim a stable contribution ID plus
an `AssetTypeId`/target pair. Built-in cookers and trusted extension cookers enter
the same descriptor validation, immutable snapshot, and lookup path; trust and
binary discovery remain Extension Manager concerns as defined by the
[Extension System](../extensions/plugin-system.md).

Registration is transactional. The host validates a complete candidate set and
publishes one immutable catalog snapshot only if every contribution is valid.
Conflicting claims are resolved only by project policy or explicit user choice
naming one exact contribution ID; registration or module load order is never a
tie breaker. Each cook operation pins one accepted snapshot for all of its jobs.
Disable and update remain restart-required in AST-001C, and live module unload is
not supported by this slice.

The internal C++ strategy interface is not a stable third-party binary ABI. Both
internal strategies and C-ABI cookers receive immutable borrowed views into the
host's bounded source storage; those views are valid only for the invocation and
must not be retained. External cooker binaries must use a versioned C ABI with
fixed-width values, opaque contexts, size/version-checked structures, borrowed
bounded byte spans, and host-owned output callbacks. No source-byte ownership is
transferred to a plugin. STL types, C++ exceptions, RTTI assumptions, allocator
ownership, and C++ object ownership do not cross that ABI. The adapter must
validate all output before it can enter the cache or a candidate cooked
generation, so external and built-in contributions cannot bypass host invariants.

### Terrain Domain Import And Cook Boundary

[ADR-138](../../adr/138-terrain-source-cooked-tile-cache-and-streaming-ownership.md)
applies the generic-Assets/domain-semantics split to Terrain and Foliage. Assets owns
tracked source bytes and `AssetId`, immutable input borrows, cooker registration and
scheduling, dependency-aware keys, cache storage, staging, verified generation
manifests, atomic `current.json` publication and provider byte leases. It does not
interpret height, layer, hole, seam, foliage placement, coordinate or LOD semantics.

Terrain Import/Model owns external format decoding and canonical authored Terrain
source validation. Terrain Cook owns deterministic tiling, seam/mip/LOD derivation,
layer/hole/spline semantics, foliage clustering and Terrain manifest/tile schemas. Both
are bounded catalog contributions; they create no second asset identity, dependency
graph, scheduler, cache directory or publication authority.

The dependency-aware key closes over the exact canonical Terrain source revision and
digest, every accepted dependency artifact, tiling/seam/LOD/compression policy,
effective tier/limit profile, Terrain source/cooked/manifest/algorithm versions and the
generic target/toolchain envelope. The resulting immutable dataset manifest binds a
canonically ordered set of independently verifiable tile/cluster artifacts with exact
bounds, digests, seam/dependency signatures and peak costs. Native Render, Physics and
Navigation artifacts are excluded and remain separately keyed by their owners.

Runtime pins one verified published generation through the Assets provider. It never
scans an output directory, follows `current.json` independently for each tile, imports
source or cooks a missing variant. Package validation applies the same manifest, digest,
limit and dependency checks used by fresh cook and cache reuse.

### Virtual Texturing Domain Import And Cook Boundary

[ADR-165](../../adr/165-virtual-texture-source-cooked-artifact-page-store-and-cache-ownership.md)
applies the generic-Assets/domain-semantics split to virtual textures. Assets owns
tracked source identity/bytes, immutable input borrows, generic dependency scheduling,
content-addressed cache storage, staging, atomic generation publication, package
membership and runtime artifact/range leases. It does not interpret virtual page,
mip-tail, border, channel, color, addressing, encoding or pack-grouping semantics.

VTX Import/Model owns canonical authored source/settings validation. VTX Cook owns
deterministic page derivation and a versioned root manifest plus bounded immutable page
packs. A logical page is independently requested and verified but is not an independent
authored `AssetId`, sidecar, mutable file or current-generation pointer. The manifest is
the only page/pack membership and canonical-order authority.

Runtime pins one exact generation and uses an Assets-owned bounded page-store/range-read
port. It cannot pass paths, request an ambient "current", enumerate storage or mix pack
bytes from different generations. Generic envelopes/requested keys, root/pack/page
digests, checked ranges and VTX semantics are all validated before publication.

Cook cache entries, published generations, provider byte caches, VTX decoded-page
caches and Renderer physical caches retain separate ownership and eviction effects.
Removing one never silently unloads, unmaps or republishes another.

### VFX Domain Import And Cook Boundary

[ADR-126](../../adr/126-vfx-graph-compilation-and-runtime-representation-convergence.md)
applies the same generic-Assets/domain-semantics split to stack and graph VFX assets.
Assets owns stable `AssetId`, immutable source/metadata snapshots, generic cooker
catalog/scheduling, `CacheKeyV1`, target selection, output bounds, staging, atomic
generation publication, rollback and immutable runtime byte delivery. It does not
interpret particle modules/nodes, select simulation domains, compile VFX kernels or
define runtime parameter/render behavior.

VFX Model/Cook owns both authoring schemas and one deterministic lowering pipeline to
the backend-neutral `CompiledVfxEffectDescriptor` plus target CPU/GPU kernel packages.
`ParticleSystemDescriptor` is the nested compiled emitter-unit foundation, not a
parallel stack/graph runtime. VFX validation owns semantic IDs/order, stages, payloads,
domain edges, readback/fallback, render/sort compatibility, resource dependencies,
provider/kernel versions and peak costs. A compiler IR is invocation-local and never
becomes a published executable representation.

The VFX cache extension includes source/semantic/compiler/provider/kernel/plan schema
versions, the effective target capability fingerprint and every accepted dependency
digest. Stack/graph sources with equivalent semantics and identical locked inputs emit
identical descriptor/kernel fingerprints. Editor layout, comments, timestamps, paths
and worker completion are not semantic inputs.

Runtime receives the published immutable envelope through the Assets provider, then
VFX validates payload/kernel/target/provider versions and digests before admission.
It never parses authoring nodes, invokes cookers/plugins, runs arbitrary particle
scripts or compiles missing variants. Too-old artifacts recook in authoring hosts;
newer/incompatible packaged artifacts fail typed loading. Hot reload publishes a new
generation atomically while active instances retain old artifact/provider/resource
leases until safe finish/restart and final retirement.

### PCG Domain Graph And Cook Boundary

[ADR-150](../../adr/150-pcg-graph-source-cooked-plan-cache-and-runtime-ownership.md)
applies the generic-Assets/domain-semantics split to procedural generation graphs.
Assets owns stable `AssetId`, accepted source/dependency snapshots, generic cook
orchestration, cache storage, staged-generation manifests, atomic publication and
immutable provider leases. It does not interpret nodes, lower graphs, evaluate plans
or commit generated scene state.

PCG Model owns graph/node/pin/edge semantics and PCG Cook deterministically lowers one
closed input snapshot to the backend-neutral immutable `CookedPCGPlan`. The PCG cache
extension includes normalized source semantics, every locked dependency, node-library
catalog and referenced node-schema generations, seed/numeric policy, effective target
capabilities/limits and all byte-affecting schema/compiler versions. Editor layout,
selection, timestamps, paths and worker order are excluded.

Fresh output and cache reuse pass the same generic envelope and PCG semantic validation
before aggregate generation publication. Runtime consumes only an exact published
plan through an Assets provider lease; it never interprets source or compiles a missing
variant. Evaluation intermediates are operation-local, and generated outputs publish
only through their target owners after a separate aggregate transaction.

### Audio Domain Import And Cook Boundary

[ADR-064](../../adr/064-audio-asset-and-cook-boundary.md) is the single
normative owner of the Audio/AST import, cook, cache, publication, and runtime
media boundary. AST owns stable asset identity, source and sidecar access,
generic orchestration, resource limits, dependency-aware cache keys, staging,
atomic publication, rollback, and runtime artifact delivery. It treats Audio
domain values as validated typed contribution data and does not parse codecs,
choose channel layouts, interpret loop or gapless metadata, or select runtime
decode policy.

AudioModel and AudioCook own the audio source extractors, container and codec
registry, authoring and cooked schemas, typed cook profiles, resampling and
layout conversion, analysis, the deterministic audio fingerprint, logical
outputs, and runtime compatibility requirements. The host composition root
registers those contributions through the generic catalog. The Assets targets
never depend on Audio targets, and Audio does not create a parallel scheduler,
cache, output tree, publication record, or asset identity.

[ADR-071](../../adr/071-procedural-audio-graph-ownership.md) applies the same
authority split to `ProceduralAudioGraph` assets. AudioModel owns the typed graph
schema and AudioCook emits an immutable `CompiledSoundGenerator`; AST retains the
stable `AssetId`, dependencies, deterministic cache inputs, staging, publication,
rollback and delivery. Editor layout/session state is excluded from the semantic
cook fingerprint, and source graphs never execute directly at runtime.

AST validates the contribution envelope and declared outputs, then publishes a
complete generation atomically. AudioRuntime consumes immutable published bytes
through the Assets provider API and validates the Audio-owned cooked header
before preparing resident or streamed state. Source decoding, file access, and
asset-provider calls never occur on the audio callback.

### Runtime UI Font Domain Import And Cook Boundary

[ADR-075](../../adr/075-runtime-ui-font-asset-family-and-fallback.md) applies the
same generic-Assets/domain-semantics split to Runtime UI fonts. AST owns stable
`AssetId`, tracked source/sidecar access, importer/cooker scheduling, canonical
cache inputs, dependency graph, staging, atomic publication, rollback and runtime
artifact delivery. It treats font contributions as bounded typed data and does not
parse font tables, select a face/fallback, infer platform fonts or build glyph
atlases.

The Runtime UI Font domain owns source-container validation, face/family/variation
schemas, deterministic matching/fallback expansion, coverage/metric indexes,
subsetting closure, semantic cook fingerprints and cooked compatibility headers.
The host registers those strategies through the generic catalogs; neither target
creates a parallel asset identity, scheduler, cache, output tree or publication
record.

Cook dependencies include every source face, fallback family, locale/script
fallback profile and required feature table. `FullFace`, declared-range and closed-
static-corpus subsetting are explicit semantic inputs. Dynamic text must have a
declared full/range coverage contract; a missing runtime glyph cannot open source
files, recook, download bytes or scan installed fonts.

Platform font discovery is an optional capability outside generic AST. A portable
artifact imports authorized bytes into a tracked font source. An explicitly
`SystemAugmented` manifest records exact discovered-face fingerprints and runtime
requirements; successful same-name discovery is not a cache dependency or silent
substitute. Font cook output contains family/face manifests, retained shaping/
raster tables, coverage/metrics and integrity metadata, never GPU atlases, native
font handles, absolute source paths or editor preview state.

### Runtime UI Style Domain Import And Cook Boundary

[ADR-076](../../adr/076-runtime-ui-style-asset-token-and-inheritance.md) gives AST
stable runtime-style asset identity, source/sidecar access, generic cooker/cache/
dependency scheduling, staging, atomic publication, rollback and runtime bytes.
The Runtime UI Style domain owns token/property/class/state schemas, typed
normalization, inheritance/reference graph validation, deterministic flattening,
semantic fingerprints and cooked payload compatibility.

Style cook dependencies include base style assets, referenced font families,
imagery/material assets and registered property/provider schema revisions. AST does
not interpret a token, resolve a visual state, import an editor theme or create a
computed style. The Style domain does not create a parallel scheduler, cache,
output tree, publication record or asset identity.

Cook output is a bounded `CookedRuntimeStyle` with stable IDs, flattened token/
class provenance, typed property/state tables, dependency digests, effect masks,
limits and integrity/version data. Editor inspector layout, selected token, preview
overrides, `ImGuiStyle`, HoroEditor `Theme`, paths, callbacks and renderer/native
handles are excluded from semantic inputs and output. Runtime consumes only the
published cooked graph and cannot scan style source files or guess missing tokens.

### Runtime UI Template Domain Import And Cook Boundary

[ADR-083](../../adr/083-ui-template-identity-schema-and-expansion.md) gives Assets
stable template asset identity, source/sidecar access, generic dependency/cook/
cache/package scheduling, atomic artifact publication and immutable byte delivery.
The Runtime UI Template domain owns local/parameter/slot/instance schemas, typed
validation, nested-DAG expansion, semantic/interface fingerprints and compatibility.

UI document cook resolves each exact accepted template revision from the locked
asset/package graph and deterministically flattens linked instances into ordinary
elements. Dependencies include nested templates and every referenced style, font,
localization, binding schema and slot resource. Cache identity includes canonical
arguments/slots, expansion algorithm/property/element schemas and all dependency
digests.

AST never interprets template parameters, reconciles updates or detaches instances;
the Template domain never creates another asset ID, scheduler, cache, package lock
or publication authority. Cooked output excludes source paths, editor state,
runtime handles and source-template update behavior.

### Physics Shape Domain Import And Cook Boundary

[ADR-085](../../adr/085-physics-shape-authoring-cook-and-runtime-boundary.md)
gives Assets stable collider asset identity, source/sidecar access, generic
dependency/cook/cache/package scheduling, atomic artifact publication and immutable
byte delivery. The Physics Shape domain owns typed primitive, convex hull,
triangle-mesh, height-field and compound schemas; geometry normalization; motion
compatibility; material/subshape mapping; and solver-private payload construction.

The cooker deterministically validates and canonicalizes geometry, bakes accepted
local scale, resolves exact dependency revisions and publishes a bounded Horo
envelope keyed by semantic digests, Physics schemas, tolerance/cooker profiles,
target platform and the exact private solver build. Paths, editor state, runtime
handles and raw authoring meshes are excluded from the artifact.

Runtime Physics accepts only validated published artifacts and immutable shape
leases. It cannot invoke import/cook work or substitute a fallback shape. The Shape
domain does not create a parallel asset ID, scheduler, cache, package lock or
publication authority.

### Destruction Domain Source And Cook Boundary

[ADR-145](../../adr/145-destruction-source-chunk-geometry-collision-and-cook-ownership.md)
gives Assets tracked mesh/recipe identity, immutable input delivery, dependency-aware
scheduling, physical content-addressed cache/package storage, operation staging and
atomic publication. Destruction Cook owns fracture semantics: normalized input
validation, deterministic chunk generation/import mapping, canonical exterior/interior
geometry, material slots, hierarchy/support/connectivity and solver-neutral convex input.

One canonical DFR artifact is the portable chunk-membership/topology authority. It has a
complete source/recipe/algorithm/seed/policy/schema/toolchain fingerprint and contains
no native Physics shape, renderer resource, path, editor state or runtime handle.
Physics consumes its exact chunk collision inputs to publish separately keyed solver-
private shape artifacts under ADR-085. Mesh/Render consumes its exact geometry to publish
separately keyed render products. Solver/backend changes invalidate only their derived
products unless DFR semantics changed.

Destruction Cook creates no private cache, current-generation pointer or package root.
Workers produce immutable candidates in the Asset operation namespace; only Assets may
verify and atomically publish. Runtime loads exact validated DFR plus required dependent
artifacts and cannot import, repair, fracture or cook a missing product.

[ADR-148](../../adr/148-fracture-document-generator-undo-and-preview-ownership.md)
keeps editor generation and publication distinct. `FractureAssetDocument` owns only an
unsaved working source candidate plus semantic history. Generator workers return bounded
detached candidates to that document; completion cannot write source, cache entries or a
current generation. Source save asks Assets to durably publish one exact accepted
document revision. Production cook then consumes that Assets revision through the
ordinary ADR-145 operation. Preview cook uses a transient non-published namespace and
cannot satisfy production readiness, enter a package or clear document dirty state.

### Navigation Domain Capture And Cook Boundary

[ADR-105](../../adr/105-navigation-asset-and-scene-ownership-boundary.md)
gives Assets the tracked `NavigationDefinition` identity/sidecar, immutable input
storage, dependency-aware scheduling/cache keys, staging, atomic generation
publication, packaging and runtime byte delivery. The Navigation domain owns typed
grounded profiles, area/link/source semantics, Scene contribution capture,
canonicalization, NavMesh build rules, neutral payload validation and provider-
private conversion.

The application bake operation captures one bounded immutable
`NavigationBakeInputSnapshot` from exact migrated project/Scene revisions, the
definition AssetId/source digest, stable contributor IDs and accepted canonical
geometry-source digests. A host adapter may project an ADR-085 normalized Physics
source model without exposing a runtime world or solver-native shape. The snapshot
is ephemeral derived state: it is neither registered as an asset nor edited,
migrated, packaged or loaded by runtime. Cookers receive it through host-owned
bounded views and cannot retain live Scene, registry, ECS or asset-record pointers.

Cook output is an immutable `GroundedNavMeshArtifact`/neutral `NavMeshData` keyed by
the full source/dependency/settings/schema/coordinate/cooker/provider fingerprint.
Generated scope/profile/tile partitions retain definition, grounded-profile and
stable contributor provenance but receive no authoring AssetIds or sidecars.
Provider-native sections are optional fingerprinted derived data. Only Assets may
publish the complete candidate generation; Navigation creates no parallel cache,
output tree, current-generation pointer or package authority.

Runtime accepts only validated published artifacts and immutable leases. Missing,
stale, corrupt or unsupported required data fails Scene/cell preparation. Runtime
cannot parse source, invoke bake, migrate data, choose an inactive generation or
substitute straight-line navigation. Dynamic carved topology remains transient
runtime state and cannot be published as a new cooked base.

[ADR-106](../../adr/106-navigation-bake-ownership-transaction-and-cache.md)
binds this contribution to the application `NavigationBakeService` and generic
AssetCook transaction. AssetCook remains sole owner of immutable cache storage,
unique same-filesystem staging, manifest construction, the cross-process
publication lock, generation directories and final `current.json` replacement.
The service owns request coalescing, immutable navigation capture, tile/profile
task groups, latest-wins generation checks and operation progress. The provider
builder owns only bounded computation over borrowed immutable input.

The navigation cache key is an explicitly versioned dependency-aware extension,
not the dependency-free AST-001C `CacheKeyV1`. Fresh and cached tiles pass equal
envelope/key/digest/bounds/semantic checks before complete generation assembly.
Cache presence cannot select a generation. Cancellation, failure, supersession,
lock timeout or stale source preserves the last valid pointer; interrupted staging
and inactive generations are cleaned/quarantined but never inferred as current.

### Prefab Domain Resolution And Cook Boundary

[ADR-095](../../adr/095-prefab-cook-boundary-and-artifact-model.md) gives Assets
stable prefab identity, immutable snapshot capture, bounded cook scheduling,
dependency-aware cache keys, staging, manifest verification, atomic generation
publication, rollback, packaging and runtime byte delivery. Assets does not parse
prefab hierarchy semantics, apply overrides or create a second expansion path.

The Prefab domain owns source validation and one versioned ADR-094 resolver that
produces an immutable `EffectivePrefabCandidateV1`. Scene Cook consumes it to embed
static placements as ordinary `RuntimeSceneDefinition` content. Prefab Cook consumes
it to emit flat `CookedPrefab` artifacts for declared dynamic-spawn roots. The
effective candidate is not an asset, cache publication or runtime input.

Keys include exact source/transitive identity, revision and digest; project,
package, schema and resolver revisions; semantic edge/placement and override
digests; cooker/output/envelope versions; target/profile; and the role-specific
scene placement/conversion or dynamic-spawn policy. Fresh and cached outputs pass
the same envelope, requested-key, payload, bounds and compatibility validation.

Standard packaging excludes raw prefab source, sidecars and effective candidates.
Static-only prefab content ships inside cooked scenes; `CookedPrefab` ships only in
the declared dynamic-spawn closure. Candidate cancellation, corruption, staleness
or publication failure preserves the prior active generation. Reloaded templates
affect future spawns only; static scene replacement remains a Scene Runtime
transaction.

### Determinism And Failure Invariants

A cooker receives an invocation-bounded immutable source input, immutable
host-canonical cooker-input metadata, a typed target, and cancellation. A
contribution may receive a contiguous borrowed view only when the admitted source
fits that contract; oversized or seek-oriented formats receive a bounded seekable
reader backed by host-owned storage. The reader uses caller-provided buffers,
enforces byte/read/seek limits, and cannot be retained beyond the invocation.
Before invocation, the host serializes exactly the fields that the current metadata
schema declares cooker-visible into `canonicalCookerMetadataBytes`: fields use
schema-defined order, scalar encodings are fixed, strings are UTF-8, and repeated
or map values are sorted by their canonical encoded key. The serialization is
versioned and length-delimited. It is produced only after supported schema
migration and default resolution, then frozen for the invocation. Unknown sidecar
fields and nondeterministic bookkeeping such as import timestamps, source paths,
diagnostics, and editor/UI state are not cooker input. Its cryptographic digest
and the metadata `schemaVersion` are immutable canonical inputs; arbitrary mutable
metadata is not.

The host retains ownership of the bounded source/reader and metadata storage for
the entire invocation. Given the same complete `CacheKeyV1` inputs defined in
[Incremental Cook And Cache Reuse](#incremental-cook-and-cache-reuse), the cooker
must deterministically emit byte-identical payload, dependency, and diagnostic
outputs through the host-owned writers. The host operation validates those
outputs, computes the complete requested `CacheKeyV1` digest, and only then
deterministically constructs the complete artifact-envelope bytes. The versioned
envelope records its format version, the full `CacheKeyV1` digest, asset ID/type,
target, source digest, bounded payload, and payload digest; malformed, oversized,
or integrity-mismatched artifacts are rejected as typed errors. The host applies
the same envelope and requested-key validation to freshly cooked output and cache
entries before either can enter a candidate generation.

The operation uses bounded structured concurrency. Missing cooker, malformed or
oversized input/output, cache corruption, cancellation, and cooker failure abort
the candidate generation. Accepted jobs are cancelled and joined before the
operation returns. No failed or cancelled operation changes the active
generation.

### Generation Publication

Cook output is immutable and content-addressed by the exact manifest bytes:

```text
<cooked-root>/<target>/
    generations/<manifest-sha256>/
        <asset-id>.cooked
        manifest.json
    current.json
```

`manifest.json` deterministically orders artifacts by canonical `AssetId` and
binds each artifact path, artifact digest, and full `CacheKeyV1` digest. While
validating fresh or cache-reused output, the host decodes each envelope and
requires its cache-key digest to byte-match the manifest entry's expected key in
addition to verifying its normal identity, target, size, and digest fields. The
host verifies all staged artifacts, publishes the complete generation directory,
and atomically replaces `current.json` last. `current.json` contains the active
manifest digest and its relative generation path; it is the sole authority
selecting a generation. Orphaned staging or inactive generations are never
inferred as current.

Runtime composition resolves `current.json` once, verifies the manifest and
relative path, and constructs the existing `FilesystemAssetProvider` with the
immutable generation directory. The provider continues to load canonical
`<AssetId>.cooked` files and needs no cooker/cache knowledge.

### Future Platform-Target Cooking Contracts

The contracts in this subsection are retained future architecture and are
outside AST-001C. They do not make desktop cooking, prefab expansion, profile
selection, or the listed triggers currently available.

Operating-system release targets will select one or more supported cook targets.
OpenGL, Metal, and Vulkan remain equal peers (`desktop-opengl`, `desktop-metal`,
and `desktop-vulkan`); none is a base, default, or fallback. A future native
mobile or platform-specific target must likewise publish and validate its typed
capability before selection. The same source asset may produce distinct bytes
for every selected target—for example, renderer-specific shader packages and
texture compression for desktop targets versus validation metadata for
`headless-null`.

Future cooking may be triggered by:

- an explicit **Cook Assets** command
- play-in-editor for the explicitly selected current editor target
- a release build or package operation
- CLI and MCP adapters described in [Future Host Commands](#future-host-commands)

The future `CookTarget` carries a stable target ID, feature level, and
compression profile. Profiles are explicit selection data, not inheritance from
another renderer target:

```cpp
struct CookTarget {
    std::string targetId;          // desktop-opengl, desktop-metal, desktop-vulkan, ...
    std::string featureLevel;      // target-owned validated capability level
    std::string compressionProfile;
};
```

Under the canonical host-owned authority invariant above, cookers never receive
source or destination filesystem authority. An internal strategy or C-ABI adapter
receives the invocation-bounded immutable borrowed input view plus the selected
target and writes payloads, dependencies, and diagnostics through bounded host-
owned callbacks. Callback-result validation may admit output only to a candidate
cooked generation; the host then stages and transactionally publishes that
generation. Catalog snapshot publication belongs exclusively to the separate
registration transaction described in [Modular Cooker Catalog](#modular-cooker-catalog).
A cooker cannot choose an output path or publish a generation.

Future cooked artifacts retain a versioned header, asset-type and target tags,
a bounded payload, and a cryptographic digest. The compatibility policy will be:

- the runtime reads the current cooked format and at least the two previous
  versions
- an artifact older than that window is re-cooked on editor startup
- an artifact newer than the runtime is rejected with a request for a compatible
  engine version

Prefab cooking is also future work. One Prefab-domain resolver will supply both
scene cook, which inlines static placements, and runtime-template cook, which emits
`CookedPrefab` only for declared dynamic-spawn roots. Standard release packages do
not ship raw `.prefab` files as runtime data. See [Prefab Architecture](./prefab-architecture.md)
and [ADR-095](../../adr/095-prefab-cook-boundary-and-artifact-model.md).

The future development output contract uses
`build/<preset>/cooked_assets/<target>/` as each target's `<cooked-root>/<target>`
location; packaging uses the release staging directory. Every target keeps a
distinct immutable generation and its own `current.json`, rather than writing
path-authoritative cooker output directly:

```text
build/debug/cooked_assets/desktop-opengl/generations/<manifest-sha256>/...
build/debug/cooked_assets/desktop-metal/generations/<manifest-sha256>/...
build/debug/cooked_assets/desktop-vulkan/generations/<manifest-sha256>/...
build/debug/cooked_assets/headless-null/generations/<manifest-sha256>/...
```

### Phase A Boundary

AST-001C must prove a real tracked source/sidecar to `headless-null` artifact flow,
verified cache reuse, atomic generation activation, and existing filesystem
provider loading. `headless-null` will be the only target admitted by this slice
once implemented. Importer-sidecar mutation, archives, hot reload, shader
compilation, GPU upload, and editor UI are separate later slices. Desktop target
formats must not be claimed until their typed renderer capabilities and consumers
exist.

## Shader Pipeline

The following shader pipeline is a future target and is not implemented by
AST-001C. Shader compilation/transpilation and every renderer-specific payload
remain outside Phase A; the list below does not activate a desktop cook target.

Shaders require a dedicated pipeline because they are source code that must be
transpiled and optimized for each target graphics API.

[ADR-035](../../adr/035-shader-source-and-intermediate-representation.md) owns the
HLSL source contract, compiler/IR routes, normalized Horo reflection and diagnostics.
It is the sole normative owner of those choices; this section projects them onto
Asset Pipeline's cook, cache, staging, and publication authority without defining
a second shader language or artifact route.
SPIR-V is a shared derived intermediate, not a universal D3D12 interchange format.
The following routes are target architecture; they add no current cook capability.

Pipeline stages:

1. **Resolve inputs**: validate source manifests and bounded include/dependency
   snapshots; graph lowering emits the same HLSL source contract and source maps.
2. **Enumerate variants**: validate the finite declared keyword/permutation sets
   before scheduling compilers. Fold each set into the versioned effective settings
   digest. The complete dependency-aware cook key remains authoritative; a keyword
   hash is only an index or display aid.
3. **Compile per target and variant**: locked DXC options produce validated SPIR-V
   for the Vulkan/GL/Metal routes or validated DXIL directly for D3D12.
4. **Realize target artifacts**: normalize actual target reflection and validate
   logical interfaces, binding maps and requirements:
   - `desktop-vulkan` → SPIR-V for the ADR-031 Vulkan environment
   - `desktop-opengl` → SPIRV-Cross GLSL 4.10 plus binding map
   - `desktop-metal` → SPIRV-Cross MSL 2.4, then offline cooked Metal library
   - `desktop-d3d12` → direct DXIL for the ADR-032 Shader Model baseline
   - `headless-null` → declared source/interface validation without native GPU proof
5. **Pack**: validated target payloads, Horo reflection/maps, dependencies and
   variant tables form the host-published artifact generation. A required target
   or variant failure cannot be silently omitted to publish a successful cook.

The shader artifact identity includes the complete target descriptor and compiler,
translator, validator, source-language, layout and generator versions/options.
Native pipeline acceleration blobs have separate device/driver compatibility keys;
neither they nor live GPU handles replace the canonical cooked artifact. Required
missing variants are errors. Packaged games do not install authoring compilers,
although a backend may realize a cooked payload (for example, GL compile/link of
cooked GLSL) at its controlled resource preparation boundary.

```cpp
struct ShaderCookResult {
    LogicalOutputId backendPayload;
    std::vector<LogicalOutputId> variants; // host-approved keyword permutations
};
```

The shader cooker writes payload bytes, reflection, dependencies, and diagnostics
through bounded host-owned writers; the logical output IDs above refer only to
host-owned result slots and grant no filesystem or publication authority. Only
the host chooses staging paths, cache locations, manifest entries, generation
IDs, and publication paths.

Changing a variant's canonical keyword/permutation set changes its effective
settings digest and therefore its `CacheKeyV1`. In the future dependency-aware
pipeline, changed included-file dependency digests will change the versioned
dependency-aware key described below and invalidate affected variants and their
transitive dependents.

See the rendering subsystem documentation for shader binding, keyword systems,
and runtime variant selection.

## Dependency Tracking

The dependency graph below is a future incremental-cook contract. AST-001C does
not schedule dependency graphs and rejects unsupported non-empty dependency sets.
When added, graph target IDs remain typed capabilities rather than a fixed
renderer preference.

The pipeline maintains an explicit **asset dependency graph**. Nodes are asset
IDs plus version metadata; edges represent references such as texture usage,
shader inclusion, material parameter, or scene object.

```cpp
struct AssetDependencyNode {
    AssetId id;
    CacheKeyDigest artifactIdentity; // CacheKeyV1 now; dependency-aware key later
};

struct AssetDependencyEdge {
    AssetId from;
    AssetId to;
    std::string kind; // texture_ref, shader_include, material_param, scene_child
};
```

The cooker reports edges and generated outputs through host-owned bounded result
writers. The completed host-owned result contains logical identities only:

```cpp
struct CookResult {
    CookTarget target;
    HostOwnedCookOutputs outputs; // bounded, host-owned result
    bool success;
};
```

During execution, bounded host-owned payload, dependency, diagnostic, and derived-
asset writers build `HostOwnedCookOutputs`; cookers cannot return paths or publish
those results themselves. Derived IDs are logical asset identities approved by
the host, not filesystem names. Only the host chooses staging paths, cache
locations, manifest entries, generation IDs, and publication paths.

The build system uses this graph for incremental cooking:

- Any `CacheKeyV1` component change invalidates the node's cached artifact.
- Importer changes invalidate a node only when re-import produces a changed
  canonical source, cooker-input metadata, or effective settings digest; importer
  identity or version is not an unbound hidden cache-key input.
- Under the future dependency-aware key, a changed dependency artifact identity
  or digest invalidates the node and every transitive dependent.
- Cycles are rejected at graph build time with a diagnostic error.

### Derived Assets

Some cookers generate additional assets:

- mesh cooker generates LOD meshes
- texture cooker generates mipmaps
- shader cooker generates variant binaries

Derived assets receive their own asset IDs and are inserted into the dependency
graph as outputs of the source node. They are cooked together with their parent
and invalidated together.

## Incremental Cook And Cache Reuse

AST-001C must compute a canonical cache-key digest from this complete tuple:

```text
CacheKeyV1(
    asset identity/type,
    exact source digest,
    canonical cooker-input metadata digest,
    metadata schema version,
    cooker contribution identity/version,
    effective settings digest,
    effective settings schema version,
    typed target ID,
    profile-presence tag byte (`0x00`; zero profile bytes),
    artifact envelope format version
)
```

`CacheKeyV1` is the canonical complete cache key for AST-001C. It intentionally
contains no dependency-content field because this slice rejects every non-empty
dependency set rather than pretending that V1 covers dependency content. The
typed target-ID field is the canonical length-delimited `headless-null` ID in the
current slice. It is followed by exactly one profile-presence tag byte. AST-001C
requires tag `0x00`, followed by zero profile bytes. Tag `0x01` is reserved for a
future compatible profile-bearing extension and is unsupported and rejected by
V1 in this slice. An omitted tag, an empty profile string, and a null profile are
not alternate encodings of absent profile data.

The tuple encoding is explicitly versioned; every variable-width field is length-
delimited, while the profile-presence tag is exactly one byte. It is not ad-hoc
string concatenation. The metadata digest is over only canonical
cooker-visible semantic metadata, never nondeterministic timestamps, paths,
diagnostics, or editor/UI state. Any byte-affecting input, contract, schema, or
format-version change changes or invalidates the key. A hit may be used only after
the immutable entry's artifact envelope, size bounds, and cryptographic digests
are verified and the envelope's full cache-key digest is compared byte-for-byte,
in constant time, with the requested `CacheKeyV1` digest. A miss must invoke the
selected cooker. An artifact found under the wrong key path is corruption handled
by the configured typed miss-or-failure policy, never a verified hit. A corrupt,
truncated, oversized, or symlinked entry must produce a typed cache failure and
must never be treated as active output.

A successful cook report must distinguish cooked artifacts from verified cache
hits. For identical inputs, fresh cooking and cache reuse must produce byte-
identical artifact and manifest bytes. Dependency-graph scheduling is not part of
AST-001C; a non-empty unsupported dependency set must fail explicitly rather than
being cooked in an accidental order.

Future dependency-aware cooking, outside AST-001C, must introduce a separate
versioned extension rather than silently changing V1. For example:

```text
CacheKeyV2(
    every CacheKeyV1 component,
    canonical ordered digest of dependency artifact identities/digests
)
```

The host canonicalizes dependency order before hashing; graph iteration or plugin
reporting order cannot affect identity. Any `CacheKeyV1` component change
invalidates the node. Importer changes do so only through changed canonical
source, cooker-input metadata, or effective settings digests, never through an
unbound hidden importer input. A changed dependency artifact identity or digest
changes `CacheKeyV2` and invalidates the node and every transitive dependent.
That future operation compares these complete versioned keys with stored immutable
entries and reports aggregate results without weakening per-entry verification:

```cpp
struct CookReport {
    std::size_t totalAssets;
    std::size_t cacheHits;   // verified immutable entries only
    std::size_t cacheMisses;
    std::size_t failed;
    std::chrono::milliseconds elapsed;
};
```

## Packaging

Release packaging groups cooked assets into **chunks** (also called bundles).
Chunks are logical collections of assets that can be loaded and unloaded
together. Examples:

- `core` — engine shaders, default materials, fonts
- `level_01` — assets for the first level
- `shared_characters` — character models and animations used by multiple levels
- `dlc_weapons` — optional downloadable content

```cpp
struct PackageChunk {
    std::string id;
    std::vector<AssetId> assets;
    std::vector<std::string> dependencies; // other chunk IDs
    CompressionMode compression;
    bool encrypted;
};
```

Chunks are assembled into `assets.horo`, a deterministic archive:

```text
assets.horo
    header
    chunk table of contents
    per-chunk compressed entries
    integrity block
```

The archive:

- is addressed by logical asset ID
- supports compression per chunk
- supports encryption per chunk for protected content
- supports delta patches between versions
- includes deterministic ordering for reproducible builds
- includes manifest compatibility metadata

At runtime, the engine mounts required chunks and unloads chunks that are no
longer needed. This enables level streaming and memory budgeting.

See [Release Architecture](../release/release.md) for packaging, signing, and verification
details.

## Runtime Loading

Runtime code requests assets through the `IAssetProvider` interface. Two loading
modes are supported:

- **Synchronous load**: suitable for small assets that must be available
  immediately. Blocks the caller thread.
- **Asynchronous load**: suitable for large assets (meshes, audio, textures).
  Returns a future-like handle and avoids frame hitches.

```cpp
class IAssetProvider {
public:
    virtual ~IAssetProvider() = default;
    virtual Result<bool> Exists(AssetId id,
                                const CancellationToken& cancellation) const = 0;
    virtual Result<std::vector<uint8_t>> Load(
        AssetId id, const CancellationToken& cancellation) const = 0;
};
```

Synchronous access is the provider contract. `AssetLoadService` adds bounded
`JobSystem` scheduling and returns a move-only `AssetLoadHandle` with poll,
wait, cancel, and single-consumption result operations. Payloads are owned bytes;
providers enforce an allocation bound before reading.

AST-001A provides:

- `FilesystemAssetProvider`: resolves only canonical `<AssetId>.cooked` files
  beneath an injected cooked-artifact root and rejects symlink artifacts.
- `MemoryAssetProvider`: deterministic concurrent headless/test provider.

`ArchiveAssetProvider` remains part of the later packaging/runtime-loading
slice and is not implemented by AST-001A.

Runtime loaders (mesh, texture, shader, material) consume the bytes returned by
the provider and do not know whether the asset came from disk or archive.

### Loading Strategy

- Small assets (< 64 KiB) may be loaded synchronously on the main thread.
- Large assets are requested asynchronously before they are needed. The runtime
  polls `IsReady()` each frame and installs the asset once available.
- Streaming assets (audio, video, large textures) use a dedicated streaming
  reader that reads chunks over multiple frames.

## Asset Deletion

When a source asset is deleted, the pipeline must clean up dependent artifacts:

1. Remove the sidecar metadata.
2. Remove the imported editor asset.
3. Remove cooked outputs for all targets.
4. Remove cache entries.
5. Mark dependent assets (materials, scenes) as missing-reference and emit
   `AssetReferenceBrokenEvent`.

The deletion is reflected in the asset dependency graph before the next cook.
Zombie cooked assets must not remain in the build output directory.

## Runtime Asset Unloading

Release builds do not support hot reload, but they do support chunk-based
unloading. When a chunk is unmounted, its assets are released. The runtime
loader is responsible for invalidating handles that point to unloaded assets.

## Hot Reload

During editor development, changing a source asset triggers hot reload. The
orchestration is handled by `AssetHotReloadService`:

1. A file system watcher (or explicit import trigger) detects a source change.
2. The change is debounced (typically 250 ms) to avoid partial writes.
3. The source asset is re-imported.
4. Dependent assets are re-cooked using the dependency graph.
5. `AssetReloadedEvent` is published on `EngineDataBus`.
6. Runtime systems subscribe to the event and reload the asset.

```cpp
class AssetHotReloadService {
public:
    void OnSourceChanged(const std::filesystem::path& sourcePath);
    void Tick(); // called every editor frame
};
```

### Per-Asset-Type Reload Strategy

| Asset type | Reload strategy                                                                                |
|------------|------------------------------------------------------------------------------------------------|
| Texture    | Replace GPU texture in-place if format matches; otherwise defer reload to next frame boundary. |
| Mesh       | Recreate vertex/index buffers; update bounding box and LODs.                                   |
| Material   | Update uniform values and texture bindings without recreating mesh draws.                      |
| Shader     | Recreate pipeline state objects and rebind materials that use the shader.                      |
| Scene      | Full scene reload or incremental merge depending on the change scope.                          |

Hot reload is editor-only. Release builds do not support asset modification at
runtime; they rely on chunk loading and unloading.

Terrain dataset reload is a generation replacement, not an in-place reaction to
`AssetReloadedEvent`. TerrainRuntime pins its current verified generation, prepares the
new manifest and its seam/dependency closure under World Streaming reservations, and
publishes only after all required consumers are ready. A failed/cancelled candidate
leaves the old generation Active; old artifact and native-resource leases retire after
their last reader rather than when `current.json` changes.

## Asset Cache

AST-001C's cache is a derived, immutable content-addressed store. The canonical
key is the digest of the versioned canonical `CacheKeyV1` tuple defined in
[Incremental Cook And Cache Reuse](#incremental-cook-and-cache-reuse): asset
identity/type, exact source digest, canonical cooker-input metadata digest,
metadata schema version, cooker contribution identity/version, effective settings
digest and schema version, typed target ID, the required `0x00` absent-profile tag
with zero profile bytes, and artifact envelope format version. Tag `0x01` is
reserved but unsupported and rejected in AST-001C; omitted, empty, and null
profiles are not alternate V1 encodings. The variable-width fields remain length-
delimited to prevent concatenation ambiguity. The digest, rather than a source
path or registration order, selects the entry. This is the complete key for
AST-001C, which rejects non-empty dependency sets; future dependency content
requires the separate versioned extension described there.

```text
<cache-root>/<first-two-digest-hex>/<remaining-digest-hex>.cooked
```

Writers stage a unique sibling file and publish without overwriting an existing
key. If another writer wins, the host decodes and verifies the existing envelope,
including a constant-time byte comparison of its full cache-key digest with the
requested key, and verifies the existing bytes are identical before discarding
its staged file. Readers decode the envelope, enforce configured bounds, verify
its normal identity, target, size, and cryptographic digest fields, and compare
its full cache-key digest byte-for-byte, in constant time, with the requested
`CacheKeyV1` digest before reporting a hit. An otherwise valid artifact under a
wrong key path is corruption handled by the configured typed miss-or-failure
policy, never a verified hit. Cache corruption, truncation, oversize, symlinks,
and cancellation produce typed errors and cannot alter `current.json`.

Changing the artifact envelope format necessarily invalidates prior cache entries
because its format version is part of the key. A versioned compatibility reader
may reuse an older envelope only under a separate, explicit compatible-cache
policy; it must validate that old format and may not treat it as a hit under the
new format version's key.

Shared-machine, CI/network cache transport, eviction, and signing are future
policies. They may move immutable entries but cannot weaken key construction,
verification, or generation publication authority.

### Future Cache Locations And Sharing

The following deployment and retention policies are outside AST-001C:

- per-preset local cache root: `build/<preset>/asset_cache/<target>/`
- local cache shared across branches: `build/shared_asset_cache/`
- CI cache artifacts uploaded and downloaded between builds
- an authenticated network cache with signed immutable entries

These locations contain the same verified content-addressed entries; a location
or transport is never part of artifact authority. When a size limit is
configured, stale cache entries are evicted by LRU policy. Eviction may remove
only derived cache entries: it cannot mutate an immutable cooked generation,
replace `current.json`, or relax digest and envelope verification on a later hit.

## Errors And Diagnostics

Import and cook errors are surfaced as structured diagnostics:

```cpp
struct ImportDiagnostic {
    enum class Severity { Info, Warning, Error } severity;
    std::string code;        // stable error code
    std::string message;     // human-readable
    std::optional<int> line; // source line when available
};
```

Diagnostics propagate to:

- GUI import log
- CLI stderr / JSON output
- MCP tool result
- `EngineDataBus::AssetImportedEvent`

## Adding A New Importer

1. Create `asset/importers/<Name>Importer.h` and `.cpp`.
2. Implement `IAssetImporter`.
3. Register in `AssetImportService`.
4. Add import tests in `tests/test_asset/`.
5. Add fixture files in `tests/fixtures/`.

## Adding A New Cooker

1. Define a stable cooker contribution ID and its exact `AssetTypeId`/target
   claims.
2. Implement the internal strategy for a built-in contribution, or expose the
   versioned C ABI function table for a trusted external binary.
3. Submit the descriptor and adapter through one candidate registration
   transaction; do not mutate the live catalog directly.
4. Add deterministic artifact, bounds, cancellation, malformed output, and
   conflict-policy tests. External binaries also require a separately compiled C
   fixture proving ABI version/size and ownership rules.
5. Verify the selected contribution by exact ID; never depend on load order.

## Future Host Commands

CLI/MCP adapters are outside AST-001C. Future GUI, CLI, and MCP cook adapters
must invoke one typed cook application operation rather than duplicate cooker,
cache, or generation-publication logic. Equivalent typed operations own import,
validation, and release/package behavior. The generic cook operation must not be
called "headless": `headless-null` will be the only target ID admitted by this
slice once implemented. Until a later slice admits another target, the operation
must return a typed unavailable-target error for every other target; none may be
substituted.

Navigation bake is the domain-specific application operation defined by
[ADR-106](../../adr/106-navigation-bake-ownership-transaction-and-cache.md).
Editor, `horo-engine navigation bake`, MCP and release cook submit the same typed
request and share its immutable capture, latest-wins, cache, cross-process lock,
staging, publication and recovery rules. The command/tool names are adapter
surfaces, not additional cook authorities.

The following block is a non-executable sketch of future command shapes. No CLI
or MCP cook adapter described here exists in AST-001C.

```text
# FUTURE COMMAND SHAPES — documentation only; not executable in AST-001C
# Import one asset
horo-engine asset import assets/models/cube.fbx

# Batch import
horo-engine asset import assets/models/*.fbx

# Cook all assets for the explicitly selected current editor target
horo-engine asset cook

# Cook an explicit target and profile (future target acceptance)
horo-engine asset cook --target desktop-vulkan --profile pc-high

# Cook multiple equal-peer targets (future target acceptance)
horo-engine asset cook --target desktop-vulkan --target desktop-metal --target desktop-opengl

# Cook a specific asset for a specific target
horo-engine asset cook --asset a1b2c3d4-e5f6-4890-abcd-ef1234567890 --target headless-null

# Bake one navigation definition/scope through the shared application operation
horo-engine navigation bake --definition a1b2c3d4-e5f6-4890-abcd-ef1234567890 --scope main

# The sole target this slice will admit once implemented
horo-engine asset cook --target headless-null

# Validate and migrate stale metadata
horo-engine asset validate

# Package a release for an explicit target/profile
horo-engine release --output dist/ --target desktop-vulkan --profile pc-high

# Package all configured release targets
horo-engine release --output dist/
```

Future MCP adapters expose the same catalog—single and batch import, current-
target cook, explicit/multiple-target cook, specific-asset cook, validation, and
release/package—and map their typed requests and results to the same application
operations as CLI. Existing names such as `import_asset` and `cook_assets` are
adapters only; they do not own pipeline policy.

## Related Documents

- [System Design](../foundation/system-design.md): module boundaries.
- [Engine Data Bus](../foundation/engine-data-bus.md): asset lifecycle events.
- [Concurrency And Job System](../foundation/concurrency-and-jobs.md): parallel import,
  cooking, cancellation, resource budgets, and progress.
- [Error And Diagnostics](../foundation/error-and-diagnostics.md): stable import, cook, and
  packaging diagnostics.
- [Rendering Architecture](./rendering-architecture.md): runtime GPU resource
  creation, upload, and shader/material validation.
- [Audio Architecture](./audio-architecture.md): cooked clip and stream formats,
  runtime loading, and audio memory budgets.
- [Prefab Architecture](./prefab-architecture.md): prefab asset format, expansion,
  and cook-time inlining.
- [Terrain And Foliage Architecture](./terrain-and-foliage-architecture.md): canonical
  Terrain source, deterministic dataset/tile cooking and runtime residency.
- [ADR-138](../../adr/138-terrain-source-cooked-tile-cache-and-streaming-ownership.md):
  Terrain import/cook contribution, cache, publication and generation-pinning boundary.
- [Release Architecture](../release/release.md): packaging and verification.
- [Horo Package System](../packages/package-system.md): bulk asset import from packages.
- [Asset Import Modal](../../../mock-studio/designs.md#architecture-runtime-asset-import-modal): React mock design for the
  import queue, diagnostics, and per-importer settings.
- [Asset Browser](../../../mock-studio/designs.md#architecture-editor-asset-browser): React mock design for the main asset
  browser with folder tree, grid/list views, and preview pane.
- [Testing Architecture](../delivery/testing-architecture.md): import and cook tests.
