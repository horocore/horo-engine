# Prefab instance hierarchy

HORO-1059 / #1059 / [PFB-006.5] adds source-qualified hierarchy presentation to
existing Scene Prefab placements. It does not flatten source members into editable
Scene objects or add a second hierarchy mutation executor.

## Ownership and interactions

The workspace captures registered Prefab source and sidecar pairs through an
explicit host loader. The loader holds the cooperative Project mutation lease
through immediate cache publication. It discovers nested and variant edges using
`PrefabDocument`, then uses `PrefabSourceResolverSnapshot` and the existing
`ScenePrefabProjection`. No panel parses JSON, discovers a provider, or reads files.

A placement root and each expanded member have distinct typed selections. Member
identity is the existing `ExpandedPrefabObjectKey`, including the containing
instance and complete nested address. The normal Scene remapper supplies optional
collision-checked Scene identities. Presentation row aliases are separate,
collision-checked values; they never authorize SceneObject commands.

The hierarchy and Inspector show source ownership, retained instance override
state and source failures. **Open owning source** resolves the exact source asset
through the current registry and the existing source-open policy. **Locate owning
instance** selects and reveals the containing placement root. Expanded members
remain read-only here; editing their source or typed value overrides belongs to
the corresponding authoring workflow.

Whole placement duplication, deletion and reparenting use the existing
`SceneDocumentCommandExecutor` commands and history. A placement can move under an
ordinary authored Scene object or the Scene root. Members cannot be reparented,
deleted, renamed, duplicated, or used as ordinary object/drop parents through this
projection. Scene external-conflict and active Play states reject placement
mutations. Source navigation and selection do not dirty the Scene.

## Capture, refresh and lifetime

The host requests only the typed Prefab dependency closure of current placements.
The source capture admits at most 256 distinct sources, 64 MiB aggregate source
and sidecar bytes, and the canonical 32 MiB individual source ceiling. Hierarchy
presentation admits 65,536 rows, counting placement roots. Capacity failure is
explicit; it never publishes a successful partial prefix or erases authored data.

The original Asset Registry publication remains authoritative. Captured sidecar
identities, project versions and canonical source revisions are validated before
resolution. Digest rechecks detect observed filesystem drift while the cooperative
lease excludes participating writers. These checks do **not** establish an OS
atomic multi-file snapshot or retained-directory identity security.

Open, document commands and asset publication refresh the owner cache. Ordinary
Draw and unchanged selection synchronization use cached values without source I/O.
Failed refresh retains matching old source members with explicit stale/error
evidence. Scene replacement retires the cache using the document's existing
replacement epoch. A host-issued workspace incarnation and never-reused publication generation fence
gestures after source refresh, SaveAs and close/reopen, including an identical
saved Scene. SaveCopy and rejected SaveAs preserve the active publication. Panel detach releases cache pins;
retained immutable snapshots remain inspection values and carry no mutation power.

## API migration and qualification

`AssetCookInputSnapshot::CaptureSelected` is an additive `HoroAssets` public factory.
It accepts explicit unique registered IDs and retains the **original** registry
publication while reading only selected source/sidecar pairs. Existing `Capture`
callers require no source changes. Consumers using the new symbol must rebuild
and link a matching Assets library. Allocation, cancellation, identity, read,
capacity and drift behavior follow the existing synchronous capture contract.

`HierarchyNodeType` appends `PrefabInstance` and `PrefabMember`; exhaustive
presentation switches should handle both. `HoroPrefabHierarchyInternal` is a
non-installed, exact three-header interface. Pure projection/command code belongs
to EditorModel; source capture belongs to EditorServices; GUI adapters consume the
same cached values. No broad source-root usage requirement is added.

HoroAssets is included in the exported Gameplay SDK target closure. The additive
factory changes no object fields, vtables, descriptors or `GameRuntimeContext`
layout; it alone does not require an ABI epoch change. The owning public consumer
and real external Physics SDK fixture compile/link the new symbol. This describes
source scope, not a claim that those consumers have already run.

The headless SceneDocument suite covers real source capture, nesting, drift,
identity failure and collision-checked projection; Windows explicitly builds it
and the selected-capture Assets regressions. GUI-enabled Linux/macOS suites cover
actual controller history/persistence, protected actions, cached selection and
localized narrow panel rendering. Windows GUI execution is not configured by the
existing Windows preset and must not be reported as passed. Local and hosted
validation receipts identify actual executed tests separately from pending scopes.
