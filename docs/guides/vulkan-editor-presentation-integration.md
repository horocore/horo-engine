# Vulkan editor presentation integration

This integration extends the existing editor composition for HORO-325/#325 and
HORO-328/#328. Vulkan remains unavailable as a product editor selection until
the real WSI, GUI and viewport implementations are wired together and qualified.

## Submission contract revision

The existing editor frame is owned by `RenderFrontend`/`RenderFrameScope`.
Vulkan graph/material recording requires explicit Horo queue order and timeline
records, while the legacy editor GUI/static-mesh calls carry no such records.
A backend-private counter would conflict with explicitly authored graphs.

`RenderFrameScope::ReserveSubmission` therefore reserves records from one
frontend-owned bounded history, using the selected backend configuration's
queue assignments. Aliased roles share a timeline. Reservations use fixed
storage, are valid through the current frame, and do not submit native work.
Aborted/unconsumed identities are burned. Waits may refer to an already admitted
point or an earlier reservation; a reserved producer must be admitted first.
Exact admitted ranges prevent skipped values from becoming producer evidence.
The finite reservation/history envelope fails closed instead of wrapping or
growing in the frame path.

Affected callers are explicit graph/material execution through `RenderFrameScope`,
including `OpenXRMultiviewGraph`. Existing explicit records enter the same history
transactionally; a rejected batch cannot advance its accepted order or timeline.
Successful backend admission commits the candidate history. Native backend unit
fixtures may continue directly exercising the backend protocol, independently of
frontend reservation policy. The XR wrapper provides `ExecuteReserved` for hosts
migrating away from independently generated records. GUI/viewport production
composition must use this same reservation source for its ordered work.

Regression requirements cover aliases, cross-queue dependencies, rejection of
unissued or abandoned producer points, import/reservation collisions, batch
rollback, frame abortion, capacity, owner-thread rejection, move/close lifecycle,
and existing explicit-submission consumers.

## Surface lifetime

The SDL port borrows the selected Vulkan window and loader. The native runtime
creates its own instance, then asks the port for a surface attached to exactly
that instance before physical-device/queue enumeration. The runtime owns that
surface lease. Discovery uses a temporary instance/surface and destroys both
before relinquishing the loader; a selected backend gets a fresh nested lease.
Device/swapchain descendants must drain before surface destruction, which must
precede instance destruction and SDL window destruction. Partial initialization
follows the same reverse ownership order. Headless ports need no WSI extension
or surface; their default surface operation returns a typed unsupported error.

Interactive admission requires `VK_KHR_swapchain` and the feature/extension
`VK_EXT_swapchain_maintenance1`; the headless Vulkan 1.3 baseline is unchanged.
The pinned Vulkan-Headers revision includes its feature, presentation-fence and
image-release types. Runtime driver support must be queried and enabled before
use; header availability is not driver evidence.

Presentation fences prove presentation-resource retirement independently of the
existing command timelines. Both proofs are required before retiring a chain's
native objects. Ordinary frames may poll but must not wait for device/queue idle.
The Khronos specification defines this distinction in
[presentation fences](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)
and [semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

## Qualification status

The new allocator and SDL/surface implementation are under construction and have
not yet been compiled or executed. Existing XR repair validation covers its
prior frozen tree only. The Linux CI driver maintenance feature has not yet been
queried; the next coherent native WSI/GUI qualification must record the exact
feature and extension facts. GUI-off native command tests do not qualify a
selected interactive editor. No ticket closure follows from these source edits.

## GUI contract migration under construction

The existing `RenderFrameScope::ComposeGui` remains the single host composition
stage. Its executor request must carry the exact current frame, a typed borrowed
recording or explicit Graphics submission mode, and the actually drawn sampled
resource generations. Explicit records come from the same frontend allocator.
The existing completion lease pool pins those identities before any recording;
GUI work extends the existing frame lease when earlier graph/material work
already owns it. A later GUI tail must extend native command retention through
that tail's completion, even if the viewport's earlier timeline point completes.
A typed failure, cancellation or exception aborts the frame and preserves every
already queued native reference; a successful call cannot omit transfer proof.

The affected contracts are `IGuiPassExecutor`, `IEditorGuiRenderer`, and the
frontend/backend frame boundary. Production HoroEditor, its interactive UI-test
renderer, GL/Metal adapters, their native smoke and lifecycle fixtures, editor
renderer fakes and public-header consumer must migrate together. There will be
no second public Vulkan-only composition stage. Preparation is bounded and must
reject unknown, stale or foreign sampled IDs before native recording.

Private viewport/GUI composition passes the existing viewport owner's typed
texture-view generation alongside its opaque GUI ID. Registration changes with
that exact generation on resize and is removed on close; it does not create a
second owner or infer registry identities from native image values. GUI-created
images keep their existing frontend identities. The private adapters explicitly
recognize only their exact current upstream font-atlas ID as native-owned on
OpenGL/Metal; arbitrary unknown IDs never receive that exemption. Those atlases
remain alive in the upstream renderer until GUI shutdown, which the host performs
after frontend terminal drain. Metal uses the same retained-reference command
buffer and its existing teardown-only idle path. Vulkan instead uploads its atlas
through the canonical frontend resource/submission path and never invokes the
upstream Vulkan automatic font upload.

The pinned ImGui Vulkan encoder is copied into build output and transformed only
at audited sites by `scripts/project.py imgui-vulkan-slots`. The script verifies the full
source hash for revision `993fa347495860ed44b83574254ef2a317d0c14f` and rejects
ambiguous matches or output inside fetched sources. Canonical retired slot/index
and actual in-flight capacity replace upstream call-count rotation. Buffers are
prepared outside frame work; recording rejects overflow instead of allocating.
Descriptor resolution uses the validated generation-bound GUI identities. The
native command owner signals the image-owned binary semaphore alongside its
existing timeline; the binary signal creates no new order or completion source.
This source stage and its new native-call regression remain unvalidated.

The single GUI contract revision is now implemented in source: host calls to
`ComposeGui` remain unchanged, while executor implementations migrate their
no-argument `RenderDrawData()` to `RenderDrawData(const GuiCompositionRequest&)`
and override `PrepareComposition(frame)` when they draw frontend sampled images.
HoroEditor, the interactive test host and GL smoke explicitly refresh the private
viewport binding before composition. Public consumer assertions cover both new
executor signatures. Existing source-only fixtures cover wrong frame, foreign
resource owner, native admission rejection, same-allocator ordering, duplicate
image pins, partial rollback, exact known-font admission, unknown IDs and delayed
completion retirement. GL/Metal share their existing frame lease, and their native
fence/command owner retains it through GUI tail completion. None of these new
regressions has run yet on this synchronized source tree.


### Canonical GUI upload and encoder inputs

`ComposeGui` remains the sole GUI composition operation. Its existing preparation
now declares at most 64 sampled generations and 64 encoder-buffer generations.
A sampled image may declare an exact frontend staging buffer for a complete
base-level upload in the same explicit Graphics submission. Borrowed OpenGL and
Metal recording reject that upload mode. Their existing native-owned font path
remains unchanged.

Frontend resource `Ready` proves realization and generation validity, not defined
image contents. Vulkan admission checks the canonical resource contents/layout
state, including earlier recorded viewport producers. Undefined samples require
the declared CopySource staging generation and a Sampled|CopyDestination image.
The sole GUI tail records copy-before-sample barriers and the native copy, then
publishes the final initialized state only after successful canonical submission.
Abort and failed submission never publish initialized contents; aborted uploads
can retry using the same retained staging and image generations.

Vulkan GUI vertex/index buffers are frontend-owned, budgeted host-coherent
allocations. The pinned build-local encoder only borrows the exact retired
canonical slot's buffers, independently of swapchain image count and draw-call
count. Slot capacity is fixed until drain/reinitialization. Font, icon and viewport
views plus staging/encoder buffers are pinned by the same existing completion
lease. Unknown or retired image identities fail preparation. Multi-viewport and
custom ImGui draw callbacks are explicitly unavailable in this composition.

The private host-composed device-child hook releases GUI native pipelines and
bounded descriptor storage after canonical command/presentation drain, before
device destruction. It creates no frame, registry, timeline or queue authority.
The upstream GUI font uploader, queue submit and idle paths are unavailable.

Host shutdown retains the GUI, viewport, bridge, SDL port and window during every
pending frontend drain poll, including partial GUI initialization. The native
runtime first proves both WSI debts and canonical graph completion, releases
GUI native state and viewport material references through the registered device
children, then destroys native resources/device, surface, instance and loader in
that order. Only after terminal frontend registry retirement does the host
release the GUI/viewport wrappers and frontend, explicitly withdraw the Vulkan
bridge/port, and destroy the SDL window. A shared material or image lease cannot
be treated as completion: the graph retires its exact queued references and the
device-child callback drops viewport resident bindings before native resource
release. The GUI preparation snapshot contains identities only, not another
resource lease. Wrong-thread or unresolved native debt cannot bypass this order.
The native Linux qualification must exercise partial prepare, pending acquired
fence, submitted GUI and late completion through this same host composition;
macOS peer compilation and scripted WSI tests do not prove that native path.

## Prepared-frame and cooked viewport migration

The editor-private `IEditorViewportRenderer::ExecutePreparedFrame` now receives
its existing `RenderFrameScope` and frozen pass list. HoroEditor and the
interactive test host call it once at their existing execution phase. OpenGL,
Metal and the bounded fake adapters forward the existing borrowed recipe;
Vulkan reserves material/clear work from that exact scope. `ExecuteStaticMeshPass`
remains the borrowed recipe executor for those peers. No nested frame or private
submission timeline is introduced. Empty-scene color production, GUI tail and
presentation use the same frame and retirement owner.

The additive public `RenderFrameScope::Token()` exposes its existing token for
private composition validation. It grants no submission or resource access and
becomes invalid on move, cancellation, presentation or frontend destruction.
Existing callers require no migration. Public consumer and lifecycle regressions
cover its return type, transfer, closure and stale-generation behavior.

Vulkan viewport loading consumes a packaged HOROSHDR payload and separate
canonical reflection metadata. Compilation retains its exact DXC/SPIRV-Tools
tool set and compilation key. A distinct admitted SPIRV-Cross operation reflects
the final stage bytes; the portable cook binds that compilation key, actual
payload digest, Cross release/build identity, layout version and normalized
reflection into the final cooked identity. Runtime links only the value codec
and existing render contracts, never process execution or compiler acquisition.
Authored parameter names map to stable Horo IDs; actual descriptor slots, offsets,
matrix strides and stage interfaces come from bounded official Cross JSON.

`HoroEditorVulkanShaderCook` is an explicit offline tool for the locked Ubuntu
26.04 host. It verifies the actual host and admitted executable bytes, invokes
the existing compilation adapter and separate reflection operation, and checks
the resulting pair through the actual runtime loader before writing build
artifacts. Generated `.horoshdr` and `.reflection.json` bytes are not committed.
The application reads only the packaged `shaders/editor/viewport-vulkan` pair
with finite size bounds and returns a typed startup error when unavailable.
Actual locked-host cooking and native presentation qualification remain pending.

The current native viewport source profile admits static meshes/tint and at most
one directional light. Requested grid, selected-light overlays and other light
profiles return `editor.viewport.capability_unavailable` instead of silently
omitting requested behavior. Their cooked pipelines and shadow parity remain
unfinished; the installed Vulkan product stays unselectable. This source stage
is not a claim of backend parity or ticket closure.

The initial Vulkan output is the documented legacy unmanaged SDR route. It
requires an actual UNORM RGBA/BGRA surface attachment with the reported
SRGB_NONLINEAR presentation colorspace, preserving the existing GUI's encoded
pixel values without hardware sRGB encoding a second time. A surface exposing
only sRGB attachments or another colorspace returns `OutputUnsupported` before
chain creation/acquisition. Explicit backend output requests and per-pass output
encodings remain unsupported; this route claims no managed color conversion or
HDR support. Native format selection is covered by the scripted admission tests;
actual display-color qualification is still pending.

## Native qualification evidence

The existing Linux shader job uses the actual locked Ubuntu 26.04 host, while
its Windows peer retains the headless command contracts. The offline CLI's
`--describe-tools` mode exports the existing admitted catalog without acquiring
or executing a compiler. The job verifies both distribution and executable
hashes before cooking, stages the generated pair into the actual build editor
asset root, and runs the matching SDL Vulkan GUI/viewport composition under
Xvfb. Its test queries the selected physical device's maintenance feature and
records the admitted same-instance surface, frame count and attachment format.
Unsupported capability admission is a failure with its original typed reason;
it is never converted into a successful smoke run or another backend selection.

The new native smoke has the existing `gui` category: its required real cooked
assets and driver are provided by that dedicated qualification step. It does
not change existing Sonar suite labels. Sonar's Vulkan/GUI-enabled build still
compiles its translation units and the production GUI/viewport/runtime sources;
CI asserts those entries in the actual native compile database. These source
checks and scripted contracts do not substitute for native smoke execution.
Cooked install/package distribution and complete grid/light overlay parity
remain separate activation gates; the product still does not advertise an
interactive Vulkan renderer.
