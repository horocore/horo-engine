# Save thumbnail and presentation capture

`HoroEngine::Runtime` owns `SaveThumbnailCapture.h`. It reuses the existing
`SaveThumbnailPolicy` and `SaveSlotDisplayMetadata` contracts. The matching
`SaveThumbnailArchive.h` owns existing-format archive attachment/extraction.
Storage remains renderer-neutral; archive/container and index versions do not
change, and the existing storage provider remains the publication authority.

The host assigns slot, intended `SlotGenerationId` and opaque `SaveThumbnailId`
before archive finalization. On the owner thread, admit one `SaveThumbnailRequest`
with explicit availability and monotonic time. Dispatch only a Pending request
and its returned serial to a qualified renderer adapter. A headless or unavailable
composition resolves Optional to Omitted and Required to Failed; Disabled always
omits capture. No native renderer is discovered or selected by this API.

The renderer adapter owns asynchronous readback, bounded staging memory/fence
lifetime and encoding. It must never block the render thread or save safe point.
Prefer an already completed compatible image when available. Transfer completed
CPU PNG bytes, dimensions and actual runtime/scene/view/frame provenance back to
the owner in `SaveThumbnailCompletion`. The coordinator validates format identity,
dimensions, byte bounds and provenance, but does not decode PNG or certify its
compressed stream. The qualified encoder and any display decoder must independently
validate image integrity and bound decoded memory. Thumbnail source frame is
explicit presentation evidence, not a claim of the exact logical save tick.

Requests have one positive finite timeout, capped at 60 seconds, dimensions capped
at 4,096 per axis, and encoded data capped at 16 MiB (defaults: 500 ms, 1,024 and
1 MiB). Poll `Advance` with current source evidence. Optional provider failure,
timeout or source replacement omits the image; Required preserves a typed failure
that the save host must resolve before entering its storage commit gate. Cancel
and shutdown retire only the owner request. Late completions cannot change a
terminal request or a replacement request: serials are never reused within a
coordinator, and slot/generation/thumbnail IDs fence replacement coordinators.
Renderer readbacks independently retire their own resources, even after the save
host has acknowledged terminal capture or destroyed the coordinator.

Pass owned logical header/manifest/chunks, candidate publication/display metadata
and terminal capture to `SaveStorageAdapter::SubmitPresentation`. It returns the
existing polling operation immediately. Its existing JobSystem worker runs
`PrepareSavePresentationWrite`, adds optional raw PNG/provenance records, finalizes
with the production `SaveArchiveContainerWriter`, and then crosses the existing
storage commit gate before provider publication. Product/environment and client
user/profile must match the supplied namespace binding. Required capture failure
is rejected before worker/provider dispatch; preparation failure is NotCommitted.
Optional omission clears the reference and builds a normal valid save. Catalog
metadata carries the exact final ArchiveContentHash; invalid display UTF-8 is
omitted. No hashing, file I/O or waiting runs on the owner/render thread.

The reserved owner is `horo.save.presentation.v1`, optional with participant schema 1.
Its metadata/image record UUIDs are
`b27b6271-528b-4a13-8cc1-0d3d8c6e0001` and
`b27b6271-528b-4a13-8cc1-0d3d8c6e0002`. Writer admission rejects collisions with
logical owner/records. Both use existing raw Chunk framing. Metadata is exactly
104 bytes: magic `HSTHMB1` plus zero terminator (offset 0), canonical slot UUID (8),
generation UUID (24), thumbnail UUID (40), little-endian uint64 runtime/scene/view/frame
(56/64/72/80), uint32 width/height (88/92), and uint64 encoded PNG length (96).
The second record contains those exact PNG bytes. Entry digests and the complete
archive-content digest authenticate both; the independently captured CanonicalStateHash
remains unchanged because presentation never participates in gameplay restore.

Existing v1/v2 readers accept these manifest-owned chunks without new wire
semantics. Hosts that do not recognize the optional owner preserve its stored
bytes through existing unknown-data/migration policy; only an explicit sealed
drop policy may omit it. The owner must not be registered as a world-state restore
participant. Its schema is independent from logical SaveSchemaVersion; a future
unknown presentation schema may remain opaque, while thumbnail display rejects it.
A copy/import that assigns new publication IDs must reconstruct owned presentation
metadata for the destination rather than display stale source associations.

On a verified committed archive, `ReadSaveThumbnail` verifies exact slot/generation,
ArchiveContentHash, thumbnail reference, optional owner/schema, raw record inventory,
byte/dimension limits and bounded provenance before returning detached CPU ownership.
An absent image remains absent; malformed optional presentation may be omitted by
the UI. The generic reader still admits the independently valid save. After durable
storage, `MakeSaveCommittedPresentation` can join retained capture with the same
publication and advisory display values. Neither path edits the index or appends
late images to a committed archive.

All coordinator calls and destruction belong to one owner thread. The capture coordinator retains no jobs/callbacks/native handles. Storage
composition uses only the existing JobSystem/provider seam and introduces no
renderer target usage requirement. Concrete readback/encoding remains owned by
the qualified renderer adapter selected by host composition.
