# Terrain authoring edit model

TRF-006.2 introduces the headless `HoroEngine::TerrainAuthoring` editor target.
`Horo/Editor/TerrainAuthoringDocument.h` belongs exclusively to that target. Its
only public dependency is `HoroEngine::TerrainImport`, which supplies the existing
canonical source and Terrain/Assets identities. Runtime targets do not depend on
this editor owner. There is no public/backend contract break or existing tool API
migration: terrain viewport tool adapters are subsequent tickets.

Hosts transfer an existing validated `TerrainCanonicalSource` into one owner with
a fresh host-issued session, explicit editing permission, power-of-two derived tile cell size
and finite limits. The host must admit at most one writable session for a source
asset. The earlier import `TerrainSourceDocument` remains an Assets candidate
publication fence; it is not a parallel edit/history owner. Once transferred,
tools receive immutable queries and submit `TerrainEditOperation` through the
application's owner-thread path. They must not retain writable canonical buffers.

Raster payloads are exact row-major source results in global increasing X/Z sample
coordinates. All layers of an affected weight pixel travel together and must sum
to 65535. Holes are binary and unsupported channels fail explicitly. A sample has
one canonical owner even on a tile seam. Rectangles affecting the same channel
cannot overlap; disjoint channels can. The executor sorts the tile invalidation
closure and includes shared seam tiles and a clipped one-sample dependency apron.
LOD-zero tile addresses use the existing cook floor-quantized world origin; downstream cooks expand this source invalidation to their captured LOD profiles. It records each source element once rather than duplicating shared samples in
per-tile storage. Tool-specific brush expansion, spline algorithms and viewport
gesture capture remain adapter responsibilities. An adapter submits the complete
final gesture closure once; previews never mutate this owner.

Placement edits assign or remove stable authored instance IDs with stable foliage
type identity and finite sample-relative transform values. Cooked clusters, runtime
handles and generation algorithms are absent. Before/after placement snapshots
contain only the affected IDs, and node transfer publishes preallocated values
without copying the placement dataset. Undo and redo restore exact source regions
and placement deltas without re-running tools. Both advance the existing source
revision while restoring the record's separate content state ID. No-op operations
preserve revision/history and a new edit after undo clears redo.

Transaction admission charges before/after payloads, snapshot metadata and dirty
closure against finite byte/sample/patch/placement ceilings. Retained undo plus
redo has item/byte ceilings; eviction changes no source or saved state. Byte charges
are conservative semantic record sizes rather than allocator instrumentation.
Temporary vectors/maps/sets are bounded by the same patch, placement and closure
counts; there is no full-heightfield or whole-placement copy per transaction.
All allocation, validation and cancellation checks precede allocation-free owner
publication. The API is synchronous and confined to one owner thread. Expensive
algorithms may prepare bounded results externally against an immutable captured
revision; completions can only submit through the same fence. This target launches
no jobs or native work and retains no tasks or borrowed external objects.

Dirty is content-state inequality, independent of undo depth. A durable source-save
owner captures the session and `State()` and calls `AcceptSavedState` only after
successful publication. An older save completion records its exact state without
clearing later edits. Cook, preview and recovery must never call this method.
This ticket supplies the edit/history model, not a source-file serializer, source
save UI, recovery service or runtime preview composition. Close stops edit/history
and save completion admission and retains the last canonical view until the host
retires the owner. Replacement closes the old object and opens a new session;
old operations fail even if asset/dataset and numeric source revision coincide.
