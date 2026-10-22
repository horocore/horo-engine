# XR swapchain import and external graph release migration

HORO-2094 / #2140 / [XRA-004.3] extends external image ownership under ADR-160.
The runtime owns allocation and native acquire/wait/release/destruction. Renderer
owns external registration, graph use, encoded native final states and retirement.
A native image index, submitted timeline, declared final state, or successful
`ExecuteGraph` return is not image release evidence.

## Native control-thread ownership

`OpenXRSwapchain` is a non-installed `HoroXROpenXRHostInterface` contract. It owns
one native swapchain under the existing `OpenXRNativeSession`; its image translator
and session must outlive successful explicit `Close`. The host retains the exact
selected device and serializes native/image/renderer operations on its render-capable
control thread. This does not create a second frame coordinator: hosts follow and
record completed operations in `XRFrameLifecycle` and own legal abort/end-frame.

Creation intersects runtime-advertised formats with the selected translator's exact
format/usage/role/sample/extent/array admission. Runtime discovery is bounded to 128
formats and 64 images. Unsupported role/extension tuples must fail adapter validation
before native creation. Native image enumeration and graphics API handles remain in
the deliberately linked private translator; public XR and Render headers contain no
native handles. Active render rectangles and view slices remain frame-plan data,
separate from the immutable native allocation descriptor.

One acquisition is open per swapchain. A bounded wait timeout keeps the obligation;
shutdown polls ready waits with zero duration. Reacquiring the same native index
advances the acquisition generation. A retained publication is revoked before
renderer retirement starts; failures remain owned and retryable. Failed imports
must have no retained renderer/native work. Once any work has been accepted, the
translator must retain its returned registration through canonical retirement.

Replacement uses distinct target generations/owners so old resources can retire
without mutating the new candidate. No destruction occurs while imports are pinned.
An actual native `XR_ERROR_SESSION_LOST` is distinguished from stale application
admission: after native loss the owner destroys resources without manufacturing a
successful image release. Application admission loss alone does not prove that
native ownership has ended.

## Graph authoring change

Existing callers of `SynthesizeRenderGraphSynchronization` retain their four-argument
behavior. External-image consumers supply a fifth span of `RenderGraphFinalState`.
Each obligation names one used `External` resource and a defined external-scope
state. Duplicate, foreign, transient, persistent, unused or malformed obligations
fail with `render.graph.synchronization.external_release_invalid`.

Both synchronization and compiled execution plans own `ExternalReleases()`.
Each record carries its last retained producer and exact old/required state. These
records must be encoded after that producer and before the covering queue completion
signal. They are requirements, not completion receipts or invented external queue
release evidence. A backend without a qualified encoder rejects before commands.

Affected callers are XR graph construction, synchronization/execution compilers,
selected native graph encoders, graph inspection/support consumers and frame
admission. Existing non-XR graph authors need no migration. No new public headers
are introduced; the existing RenderApi ownership assignment remains authoritative.

## Qualification and composition

`HoroXROpenXRVulkan` is configured only when both native XR and Vulkan are enabled.
Its dedicated `HoroXROpenXRVulkanHostInterface` exports only the native host header;
`HoroRenderExternalImagesInterface` and `HoroRenderVulkanExternalImagesInterface`
provide the narrow common and selected-backend bridge contracts. Dependency-policy
entries explicitly authorize these edges. No installed public header exposes a
native handle and no new installed-header ownership entry is needed.

The adapter uses the exact stable session Vulkan binding, queue index zero (the
current native renderer queue policy), and the canonical device incarnation. Source
publication checks acquisition liveness; actual native retirement records the
committed layout/queue before releasing HORO-326's single RAII lease. A bounded
registry retirement drain observes that same lease; no second GPU timeline or
drain ledger exists. Import preparation uses the existing bounded registry and
native view allocation outside an active renderer frame; this is not a measured
allocation-free XR frame claim.

A new `RenderGraphDepthAttachment` alternative is appended to the workload variant,
preserving previous alternative indices. It represents a normalized whole-array
depth clear/store. Vulkan records array color/depth rendering and a post-producer
barrier before the canonical submission signal. Other backends reject unimplemented
external release/depth workloads before encoding. Motion/density and requested
fixed/gaze foveation remain explicitly unsupported by this adapter until their
complete backend/runtime contracts exist; they are never reported active or
silently translated to color rendering. Projection depth requires negotiated
`XR_KHR_composition_layer_depth`.

The Linux native-command CI composition additionally enables OpenXR and builds the
actual adapter plus its dedicated header consumer. It asserts the discovered XR
integration case exists and selects Catch `target::case` names explicitly, so a
standalone smoke executable cannot hide an omitted contract suite. Regressions drive the production
OpenXR owner, Vulkan image adapter, frontend registry, graph compiler, native graph
recorder and retirement through injected official dispatch. Coverage includes color
and depth arrays, native operation order, timeout/retry, stale/reacquired plans,
presubmission failure, deferred retirement, recreation, loss and shutdown. The
existing installed-driver Vulkan smoke remains separate from fake-dispatch XR
qualification. Physical headset/compositor/device qualification remains NotRun. Hosts close external
images/swapchains and the native XR session while the selected graphics device is
still alive, then complete frontend/backend device shutdown.

The native ownership/layout basis is the Khronos OpenXR Vulkan extension contract:
https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/khr/khr_vulkan_enable.adoc
The runtime supplies attachment-compatible ownership on the session-bound queue
when an image becomes available and requires the matching layout/queue on release.
This does not authorize fabricating a previous queue-family release for arbitrary
external allocations. The external source must come from the real completed native
acquire/wait path.
