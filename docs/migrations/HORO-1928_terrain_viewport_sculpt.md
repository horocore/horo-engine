# HORO-1928 / #1972: terrain viewport sculpting

The workspace opens explicitly imported `core.terrain` assets as persistent Asset
documents. Raise, Lower, Smooth, Flatten and Noise produce exact height patches
for the existing `TerrainAuthoringDocument`; that document remains the only source,
revision, saved-state and undo authority. Scene source and live runtime state are
not brush destinations. One release publishes one history transaction. Noise uses
the captured seed; undo and redo replay recorded samples.

The built-in importer admits **`.terrainraw` only**, containing little-endian
finite RAW-F32 heights. It does not claim generic `.raw` files. Its versioned Assets
sidecar records dimensions, spacing, projected/local coordinates and CRS. The
payload remains RAW-F32 in the normal `.horoasset` asset representation. Stable
Terrain dataset identity derives from the actual AssetId. The editor adapter
explicitly admits height-only source; weights, holes and unknown settings are
rejected rather than discarded. Other canonical importers and formats are unchanged.

The isolated source view draws a bounded wireframe sampled from canonical heights,
and a surface-following brush outline. Picking traverses actual cooked-diagonal
triangles with a 4,096-cell ceiling; no infinite-plane substitute is used. Source
coordinates retain their double-precision origin. The source preview frame maps
the first sample to its local origin, and camera, picking, grid and brush use the
same frame. This interaction overlay does not claim high-fidelity TerrainRuntime
activation, GPU terrain rendering or a successful downstream cook.

A workspace admits four terrain documents. Each admits at most 1,048,576 samples
(4 MiB heights), 64 stroke stamps, 16,384 affected samples, 1,048,576 producer sample
visits, 512 KiB transaction storage and 8 MiB/64 entries of patch history. Thus
retained canonical heights and history are bounded by 16 MiB and 32 MiB per
workspace, in addition to bounded command and owner metadata. The camera framing
profile rejects extents or center elevations beyond 8,192 meters rather than
silently losing viewport precision. Brush work runs synchronously only at command
application; drawing has fixed sample/segment counts and performs no source I/O.

Uncommitted gestures are disposable. Escape, pointer/focus loss, changed camera,
viewport extent, document revision/capability, document replacement, Play state,
close and shutdown revoke a stroke. Delayed commands carry the complete workspace,
document session, dataset, revision and capability fence. Dirty tab close is denied;
Save, explicit Discard and continued editing remain available. Save All and project
leave include terrain documents and exact delayed-leave revision guards.

Source save reuses `PublishAssetSource`, the shared project mutation coordinator
and native source/sidecar leases. Both accepted byte identities must match. Only
height bytes are replaced; importer metadata is retained. A replacement followed
by directory-sync uncertainty advances the accepted source identity and leaves
the document dirty for an explicit durable retry. Save does not imply cook success.

## Initial import publication migration

The old private `Assets::ProjectAssetImportCommitter` wrote the payload and sidecar
with sequential truncating streams. Its replacement is the single concrete
`Application::ProjectAssetImportCommitter`, owned by the new narrow
`HoroProjectAssetImportApplication` target. The bridge depends forward on Assets,
Foundation and the existing project mutation guard; Assets has no Editor/GUI
dependency. The public header has exactly one target owner.

Hosts inject their existing mutation coordinator and durable filesystem. The
editor import modal accepts an explicit shared `IAssetImportCommitter` capability;
missing authority fails publication. The actual GUI host supplies the bridge.
The import route supplies its explicitly selected project root, never an ambient
working-directory substitute. Root capture resolves an existing directory once;
invalid selections fail closed. The explicitly invoked legacy root-discovery
overload remains separate from GUI source selection.
Native integration and modal lifecycle fixtures supply the same real storage
authority explicitly. Initial imports reject existing destinations; replacement
belongs to explicit reimport rather than a hidden truncating overwrite.

Terrain imports require their identity/coordinate settings sidecar. Other
importers retain explicitly supported metadata-optional payload publication;
such payloads are deliberately untracked by the ordinary registry.

At most 64 items and 64 MiB of payload/requested sidecar bytes are prepared. All application
stages are durably written and verified before any destination is promoted, under
project and per-source writer leases. The authoritative registry is published only
after the complete durable batch, including mandatory terrain pairs. A prepublication failure compensates only proven
owned new files. A post-rename durability failure remains a reconciliation outcome
even if compensation succeeds. No existing generation or unrelated file is adopted
or removed.

This provides consumer-atomic registry visibility, **not a cross-crash filesystem
pair transaction**. There is no new recovery journal. A crash between renames can
leave an incomplete pair; existing registry rebuilding diagnoses and excludes
missing-source/missing-sidecar records, and terrain opening requires both. All
cooperating writers must respect the explicit leases; rename is not a content CAS
against noncooperating external mutation.

`IAssetImportCommitter::Commit` failure no longer implies that nothing was
published: derived-index persistence can fail after authoritative registry
publication. Such a failure retains the complete source pair and registry,
reports durability reconciliation and exposes the bridge's scalar receipt.
Callers must not overwrite/delete destinations merely because Commit failed.
The derived index can be rebuilt from the retained canonical pair.

Focused coverage exercises exact sculpt history, real triangle/DDA picking,
origin-frame/nonuniform placement, real import publication and compensation,
source/metadata conflicts, durability retry, workspace command routing and
headless localized controls. Automated rendering is distinct from manual native
editor interaction; this change does not restart an editor with unsaved changes.
