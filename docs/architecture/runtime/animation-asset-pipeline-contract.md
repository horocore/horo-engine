# Animation Asset Pipeline Contract

## Status And Scope

This document is the normative ANI-001.8 decision for animation authoring data,
import settings, dependencies, cooked representations, diagnostics, and the
Animation/Asset Pipeline ownership boundary. It applies to skeletons, animation
clips, and skeletal-mesh bindings defined by ANI-001.3, ANI-001.4, ANI-001.6,
and ANI-001.7.

This decision does not implement a DCC parser, an animation graph format, a
retarget-profile format, renderer skinning resources, or a runtime animation
registry. Those capabilities may consume this boundary but cannot change its
identity, publication, or ownership rules implicitly.

## Decision Summary

Animation owns the meaning and versioning of animation payloads. Assets owns the
generic transaction that reads a tracked source, invokes an explicitly composed
contribution, hashes inputs, stores dependencies, publishes immutable artifacts,
and later supplies their bytes. Application composition registers Animation
importer/cooker contributions into Assets; neither subsystem discovers the other
through global state.

```text
tracked source + sidecar             AssetId / AssetTypeId authority (Assets)
             |
             v
host-bounded source bytes ----------> Animation importer
                                      validates domain semantics
                                      emits canonical authoring payload
             |
             v
AST cook request -------------------> Animation cooker
target + effective settings           validates exact dependencies
                                      emits Animation-owned artifact payload
             |
             v
AST cache/envelope/generation ------> immutable byte provider
             |
             v
Animation load boundary ------------> typed immutable SkeletonAsset /
                                      SkeletalMeshSkinningAsset /
                                      AnimationClipAsset
```

The selected public shape is an Animation-owned canonical authoring payload and
an Animation-owned cooked binary payload, each carried inside the existing
Assets identity, cache, and publication contracts. Source DCC bytes are never a
runtime format. Generic Assets JSON or maps never become an animation schema.

## Ownership

| Responsibility | Authority |
|---|---|
| Stable `AssetId`, `AssetTypeId`, sidecar, project path, and registry revision | Assets |
| Source-root/trust checks, bounded byte acquisition, cancellation, and operation lifetime | Assets/Application |
| DCC scene probing and parser integration | Import contribution owner, outside Assets and AnimationApi |
| Skeleton, clip, and skeletal-binding semantic extraction | Animation import contribution |
| Joint, socket, track, time, interpolation, additive, and binding invariants | Animation |
| Declarative import-setting presentation and persisted setting values | Assets schema transport; Animation owns IDs and meaning |
| Generic cook target selection, source/dependency snapshot, cache key, output sinks, staging, and atomic publication | Assets/Application |
| Compression profile, canonical animation payload, compatibility value, and decompression limits | Animation |
| Renderer palette/buffer realization | Renderer, after Animation produces an immutable pose/palette projection |
| Runtime asset publication and generation-safe retirement | Future Animation runtime owner using Assets byte leases |

`HoroEngine::Assets` must not depend on `HoroEngine::AnimationApi`. An Animation
import/cook target may depend on both narrow APIs. The host owns the mutable
catalog candidate and publishes it only after complete validation. Contribution
descriptors are inert metadata: constructing one cannot read files, register a
service, start a worker, or mutate a catalog.

## Canonical Asset Kinds

The following `AssetTypeId` values are reserved for this contract:

| Asset type | Animation value | Required typed dependencies |
|---|---|---|
| `core.animation.skeleton` | `SkeletonAssetData` | none |
| `core.animation.skeletal_mesh_binding` | `SkeletalMeshSkinningData` | one `core.mesh`, one `core.animation.skeleton` |
| `core.animation.clip` | `AnimationClipData` | one `core.animation.skeleton`; optional additive reference-pose asset after that asset kind has its own contract |

The sidecar-owned `AssetId` is the only persistent identity of the record. A
decoded candidate injects that identity into `SkeletonId`, `SkeletalMeshId`, or
`AnimationClipId`; the payload cannot override it. Publication generations,
runtime handles, pose handles, dense joint indexes, names, and source paths are
never persisted as identity.

The domain payload stores dependency identities where their semantic role is
required, while the importer/cooker also reports the same identities through
the host-owned dependency sink. The two projections must match exactly after
canonical sorting and duplicate removal. A missing, extra, mistyped, self, or
role-conflicting dependency rejects the candidate. Assets owns graph admission;
Animation owns the reason that a dependency is required.

Animation graph, blend-tree, and retarget-profile assets require their own
versioned contracts. They must not be encoded as an undocumented variant of one
of the three types above.

## Authoring Payload Contract

### Envelope

The editor payload is canonical UTF-8 JSON used at import/reimport and editor
load boundaries. It has one top-level object with these fields in this canonical
serialization order:

```json
{
  "schemaVersion": 1,
  "assetType": "core.animation.clip",
  "contractVersion": { "major": 1, "minor": 0, "patch": 0 },
  "dependencies": [],
  "payload": {}
}
```

`assetType` must equal the sidecar record type. `schemaVersion` owns this JSON
envelope; `contractVersion` owns the corresponding Animation model. The payload
does not repeat its own `AssetId`, project path, source filename, importer ID,
registry revision, or publication generation.

Canonical serialization uses UTF-8 without a byte-order mark, rejects invalid
Unicode, writes one trailing newline, emits no insignificant whitespace, emits
object fields in contract order, and sorts semantically unordered collections by
their stable typed identity. Numbers use the narrow integer or finite binary32
domain already owned by the typed model. Time is signed nanosecond ticks and
sample rate is a reduced positive integer fraction; seconds encoded as binary
floating point are invalid.

Parsers reject duplicate keys, unknown major versions, non-canonical identity
text, non-finite values, arithmetic overflow, invalid enum values, excessive
nesting, excessive strings/arrays, and trailing non-whitespace data. Unknown
fields are rejected in version 1 instead of being silently dropped and thereby
changing a future document on reimport. A later compatible minor version must
define an explicit preservation rule before admitting extensions.

### Skeleton Payload

The skeleton payload maps one-to-one to `SkeletonAssetData` except for the
sidecar-injected identity. It contains joints and sockets keyed by stable local
IDs. Joint order is canonical parent-before-child with stable-ID tie breaking;
socket order is stable-ID order. Parent, mirror, and socket targets use stable
joint IDs rather than array positions. Advisory names are NFC-normalized,
bounded UTF-8 and never identity. Transforms must be finite and pass the existing
reference-pose/inverse-bind validation.

### Skeletal-Mesh Binding Payload

The binding payload maps to `SkeletalMeshSkinningData`. It names the exact mesh
and skeleton dependencies by `AssetId`, then carries stable mesh-joint to
skeleton-joint remaps, LODs, sections, palettes, and bounded influences. It does
not contain renderer vertex-buffer layouts, GPU handles, native formats, or a
live skeleton generation. Canonical order follows ANI-001.4 validation.

### Clip Payload

The clip payload maps to `AnimationClipData`. It names one skeleton dependency,
exact duration ticks, reduced sample rate, wrap mode, clip kind, root-motion
intent, optional additive reference-pose binding, and stable-joint tracks. Tracks
and equal-time keys use the canonical order from ANI-001.6. Import never bakes a
runtime publication generation into the document; the loader binds the current
immutable dependency generation before `AnimationClipAsset::Create`.

Compression in authoring data is policy intent, not proof that bytes are runtime
compatible. Only ANI-001.7's complete compatibility value and the AST artifact
envelope can prove a cooked publication.

## Import Contract

Assets supplies immutable invocation-scoped source bytes, the normalized source
extension, captured setting values, a cancellation token, and host-owned bounded
output storage. The importer cannot retain those views, open another project
path, write sidecars, publish a registry, create cache entries, or call editor UI.

DCC parsing belongs to a dedicated import contribution or pinned parser adapter,
not `AnimationApi` and not generic Assets. One source may expose multiple takes,
meshes, or skeleton roots. Automatic selection is allowed only when exactly one
candidate satisfies the captured settings. Ambiguity is an actionable failure;
file order, display name, or "first take" is not policy.

Animation owns these stable import-setting IDs and scalar encodings. They map
directly to the kinds supported by Assets' `ImportSettingDescriptor`; a
contribution must not pack several values into an undocumented text field.

| Setting ID | Kind | Meaning |
|---|---|---|
| `coordinate_profile` | Choice | Stable ID for the source-axis, handedness, and unit conversion profile |
| `skeleton_root` | Text | Stable source-node selector; empty only when selection is unambiguous |
| `clip_take` | Text | Stable take selector; empty only when selection is unambiguous |
| `clip_range_start_ticks` | Integer | Inclusive source-time start in signed nanosecond ticks |
| `clip_range_end_ticks` | Integer | Exclusive source-time end in signed nanosecond ticks; must exceed the start |
| `sample_rate_mode` | Choice | `preserve_source` or `explicit` |
| `sample_rate_numerator` | Integer | Positive reduced-rate numerator; ignored only in preserve-source mode |
| `sample_rate_denominator` | Integer | Positive reduced-rate denominator; ignored only in preserve-source mode |
| `root_motion_mode` | Choice | `disabled` or `extract` |
| `root_motion_joint` | Text | Stable joint selector; empty only when root-motion extraction is disabled |
| `import_scale_numerator` | Integer | Positive reduced scale numerator composed with the coordinate profile |
| `import_scale_denominator` | Integer | Positive reduced scale denominator composed with the coordinate profile |

Contribution descriptors expose applicable fields through Assets'
`ImportSettingDescriptor`; serialized setting metadata uses the stable IDs above,
never localized labels or vector positions. A setting schema/version change that
can alter output changes the importer contribution version and canonical imported
payload digest. Project presets may retain only fields explicitly marked
`includeInPresets`.

Import preflights source bytes, source nodes, joints, sockets, takes, tracks,
keys, strings, hierarchy depth, and total output bytes against finite hard and
operation limits. Cancellation is checked during bounded parser/extraction loops.
Malformed or over-budget input returns no partial `PreparedAssetImport` and does
not authorize sidecar or registry mutation.

## Cooked Representation

Animation cookers consume only a fully parsed canonical authoring payload and an
exact immutable dependency snapshot. They never parse DCC source bytes. Every
cooked payload begins with an Animation-owned binary header containing:

- payload kind and format version;
- exact Animation contract version;
- stable primary and dependency `AssetId` values;
- canonical dependency/type list digest;
- Animation cooker contribution identity and version;
- explicit compression-profile identity and representation, where applicable;
- uncompressed logical size, section offsets/counts, and payload checksum.

All integers are little-endian fixed-width values. Variable sections use checked
offset/count pairs relative to the start of the Animation payload. Sections are
ordered by kind and stable identity, are non-overlapping, and fit both the
Animation hard limits and the enclosing `AssetCookLimits`. Strings are bounded
UTF-8 and are absent from frame-hot lookup tables unless required diagnostically.
Native pointers, ABI-dependent structs, `size_t`, source paths, timestamps,
registry revisions, process-local generations, renderer handles, and padding
bytes are forbidden.

The initial animation representation is target-independent and must support the
implemented `headless-null` cook target. A desktop target may reuse identical
Animation payload bytes while AST publishes a distinct target generation. A
renderer-specific skinning resource is realized later by Renderer and cannot be
smuggled into this payload. Unsupported target/profile combinations fail before
output writes; they never fall back to another target or compression tier.

The AST `CacheKeyV1` remains authoritative. Its source and metadata digests cover
the canonical authoring payload and cooker-visible settings. The effective
settings digest additionally covers the Animation payload format version,
Animation contract versions, coordinate/time normalization version, exact
compression profile, and every byte-affecting option. Dependency-aware animation
cooking must use the future AST dependency-aware key; it cannot pretend V1 covers
dependency contents. Until that host capability exists, a contribution requiring
non-empty dependency content must fail explicitly rather than publish an
under-keyed artifact.

The host owns output sinks, hashing, envelope encoding, cache verification,
generation staging, manifest replacement, and rollback. The Animation cooker
returns payload bytes, dependencies, and structured diagnostics only. A failed,
cancelled, stale, or over-budget cook preserves the prior active generation.

## Load, Reload, And Lifetime

Runtime obtains an immutable AST byte lease for the exact asset/type/target and
validates both the AST envelope and complete Animation header before allocation.
It resolves and pins all typed dependencies, constructs a detached candidate,
then calls the existing Animation `Create`/cook validation boundary. Only a fully
validated candidate may replace the active immutable publication.

Reload requires the same stable asset identity and a dependency snapshot that is
still authoritative at commit. Failure retains the last good generation. Old
Animation publications and AST byte leases retire only after all generation-safe
runtime/pose consumers release them. Shutdown closes admission, cancels and joins
owned load work, revokes runtime leases, and destroys Animation state before its
Assets providers.

Packaged/headless runtime loads cooked payloads only. It does not install a DCC
parser, read authoring JSON, scan source paths, or re-run compression.

## Diagnostics

Animation failures preserve the originating stable `animation.*` code. Import and
cook boundaries add structured context without parsing or rewriting messages:

- primary `AssetId` and `AssetTypeId`;
- source location as a host-approved project path plus byte/node/take/joint/key
  location when available;
- exact contract, importer/cooker, target, profile, and dependency identities;
- finite-limit name, actual value, and admitted maximum;
- operation ID and cancellation/shutdown/stale-publication state.

Assets may wrap the failure with an `asset.import.*`, `asset.cook.*`, cache, or
publication code only when it preserves the Animation cause and structured
context. Animation never reconstructs meaning from a generic diagnostic string.
Diagnostics are bounded and redact absolute paths and source content. Multiple
record errors use deterministic stable-identity/source-location order and report
the omitted count when the diagnostic budget is exhausted.

Required failures include malformed/unsupported source, ambiguous selection,
invalid hierarchy/binding/track, dependency missing/type/revision mismatch,
non-finite or overflowed data, limit exceeded, unsupported target/profile,
corrupt/truncated artifact, cancellation, shutdown, and stale commit. Every such
failure publishes neither partial authoring state nor a partial cooked/runtime
generation.

## Alternatives Considered

### Use DCC files directly at runtime

Rejected. Parser/tool versions, selection defaults, coordinate conversion, and
hostile-input risk would become runtime behavior. Packaged and headless products
would need authoring dependencies and could not prove deterministic artifacts.

### Put animation schemas in generic Assets

Rejected. Assets would acquire joint, interpolation, root-motion, compression,
and renderer-adjacent semantics and create a reverse dependency. Other domains
would either duplicate that policy or expand Assets into a feature registry.

### Let each Animation type own files, cache, and publication

Rejected. Separate skeleton/clip caches and path scans would compete with AST
identity, dependency, transaction, packaging, cancellation, and rollback
authority.

### Serialize public C++ structs as cooked bytes

Rejected. Layout, padding, endianness, container representation, and ABI changes
would silently alter artifacts and could persist process-local generations.

### Select import takes and compression from ambient defaults

Rejected. GUI state, locale, discovery order, or platform defaults would make
reimport/cook history-dependent and would omit byte-affecting inputs from cache
identity.

## Consequences And Implementation Order

The decision adds one explicit Animation import/cook adapter layer but keeps the
public Animation API backend-neutral and the Assets core domain-neutral. Imported
and cooked bytes have deliberate schema migration costs; in return their identity,
cache validity, portability, and failure behavior are testable.

Implementation proceeds in this order:

1. publish Animation-owned authoring codecs and hard limits for the three asset
   types;
2. add pinned DCC adapter contributions and canonical setting schemas;
3. add Animation cooker contributions and versioned binary decoders for
   `headless-null`;
4. compose contributions in GUI, CLI, MCP, and headless hosts through the same
   application service;
5. qualify byte identity, dependency skew, malformed/hostile input, cache reuse,
   cancellation, reload, and shutdown before admitting desktop/package targets.

No current runtime file format is grandfathered as canonical. Existing
illustrative JSON, test fixtures, or in-memory `Animation*Data` construction must
be migrated through the version-1 authoring codec when it is implemented.
Existing ANI-001.7 compressed values must be recooked into the versioned binary
payload; a loader must not synthesize a missing compatibility value. Source files
and sidecars retain their AST `AssetId` across that migration.

ANI-001.8 records policy only. It does not claim that Animation importer/cooker
contributions or runtime binary decoders are already implemented.

## Qualification

Downstream implementation must prove:

- equivalent semantic authoring candidates serialize byte-identically;
- malformed, duplicate, non-finite, oversized, excessive-depth/count, and future
  major-version documents fail before publication;
- DCC candidate ambiguity requires explicit settings and never selects by order;
- dependencies reported in payload and host sink match exactly and stale snapshots
  cannot commit;
- fresh cook and verified cache hit produce byte-identical AST envelope and
  Animation payload bytes for the same complete key;
- corrupt headers, offset/count overflow, truncation, wrong type/target/profile,
  and dependency mismatch fail before runtime publication;
- cancellation, reimport, reload, project close, and shutdown retain last-good
  state and produce no late callback or partial generation;
- GUI, CLI, MCP, and headless composition invoke the same backend-free operation;
- the `AnimationApi` public-header consumer remains limited to Foundation and
  Assets and no native parser/renderer type crosses the boundary.

## Related Documents

- [Animation Architecture](./animation-architecture.md)
- [Asset Pipeline](./asset-pipeline.md)
- [Animation Ownership, Update Order and Clock](../../adr/061-animation-ownership-update-order-and-clock.md)
- [Header Visibility and Ownership](../foundation/header-visibility-and-ownership.md)
- [Error and Diagnostics](../foundation/error-and-diagnostics.md)
- [Ownership and Resource Lifetime](../foundation/ownership-and-resource-lifetime.md)
