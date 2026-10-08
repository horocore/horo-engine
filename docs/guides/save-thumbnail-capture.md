# Save thumbnail and presentation capture

`HoroEngine::Runtime` owns `SaveThumbnailCapture.h`. It reuses the existing
`SaveThumbnailPolicy` and `SaveSlotDisplayMetadata` contracts. Storage remains
renderer-neutral; this addition does not revise archive/index schemas or invent
a second metadata authority.

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
terminal request or a replacement request because serials are never reused.
Renderer readbacks independently retire their own resources, even after the save
host has acknowledged terminal capture or destroyed the coordinator.

Include accepted encoded bytes and their source evidence while finalizing the
archive, and put the exact thumbnail reference in its publication metadata. An
omitted image requires an absent thumbnail reference. Never reopen a published
archive to append a late image. After independently acknowledged durable storage,
`MakeSaveCommittedPresentation` joins terminal capture with matching slot,
generation and thumbnail identity. It returns detached CPU ownership and existing
catalog values; invalid/oversized UTF-8 display metadata is omitted without
invalidating the trusted save. A newer generation must be projected with its own
capture; old artifacts fail generation admission. Persist/catalog these values
through existing archive/storage/index authorities, rather than updating the index
from a renderer callback.

All coordinator calls and destruction belong to one owner thread. The API schedules
no jobs, retains no callbacks/native handles and introduces no renderer target
usage requirement. Concrete readback/encoder integration stays at host composition.
