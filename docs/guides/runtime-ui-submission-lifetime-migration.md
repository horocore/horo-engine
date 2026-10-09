# Runtime UI submission lifetime migration

HORO-762 / #762 / [RUI-007.6] connects owned UI generations to the existing
renderer graph submission lease. It depends on the submission-retention producer
from PR #3342; it does not add a UI draw workload or claim UI pixel rendering.

## Host handoff

Include `Horo/Runtime/Render/UiRenderSubmission.h` through HoroRenderFrontend.
Realize current image pages with `CreateUiImageTexture` and current atlas pages
with `CreateUiGlyphAtlasTexture`, then import their exact texture handles into the
compiled graph. Supply complete tightly packed page bytes. Malformed uploads
return a typed failure before a provenance token is issued or an upload is queued.
Existing texture admission remains authoritative: currently unsupported sRGB and
Alpha8/R8 requests fail rather than being silently converted to linear RGBA.

At admission, bindings must use the frontend-issued realization and the exact
current source owner. Matching numeric IDs or an unrelated valid imported texture
cannot substitute for that authority. Resource uploads must be ready before graph
execution. Realization is a load/update operation, not provider discovery or I/O
inside frame submission.

Pass the geometry plan, image bindings, face/page bindings and `SealFrame` result
to `SubmitUiGraph` (or the UI-owning `RenderFrameScope::ExecuteGraph` overload).
All input pointers are synchronous borrows. Successful admission copies the
owned immutable sources and transfers the sealed atlas pins into the same bounded
lease that already retains native graph resources. Present, resize and reload
do not retire that lease; native completion, unsent abandonment or terminal
renderer shutdown does. Source facades may be destroyed after admission.

## Font and atlas callers

Native-submitted fonts declare paired source asset/revision provenance in
`UiFontFaceDescriptor`. Shaping-only callers can continue omitting both fields,
but those faces cannot authorize native atlas bindings. `UiGlyphAtlasGlyphKey`
now requires the exact nonzero `fontRevision`. Upload callers provide
`UiGlyphAtlasRasterData::sourceFace`; the atlas copies its immutable ownership,
not the caller's pointer. Logical/headless atlas uploads may omit this source,
but do not establish native font provenance.

Provide one `UiRenderFontBinding` per used face/page pair. A single face can span
multiple pages, in any binding order, within the fixed 32-binding capacity.
Every snapshot glyph must match exactly one binding using the sealed frame's
owned font generation, page and UV placement. Missing pages, wrong page textures
and duplicate coverage fail before backend execution. Glyph checking is bounded
by the 4,096-glyph submission ceiling and fixed binding capacity.

`SealFrame` transfers retirement responsibility to the move-only frame lease.
Do not call raw `RetireFrame` on a sealed frame or retire pins merely because
presentation returned. Failure before native acceptance abandons the lease once;
successful submission delegates release to native completion ownership.

## Regression evidence

`UiRenderSubmissionTests.cpp` exercises the actual frontend handoff with an
explicit asynchronous test backend: two-page font coverage, missing/wrong pages,
foreign atlas/image ownership, malformed upload rollback, source destruction,
reload, resize, unsent failure and shutdown. This is host contract coverage, not
a GPU-completion or pixel-rendering claim. Execution and supported native-backend
evidence must be recorded separately during validation.
