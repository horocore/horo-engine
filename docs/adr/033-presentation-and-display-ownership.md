# ADR-033: Presentation and Display Ownership

- **Status**: Proposed
- **Date**: 2026-08-31
- **Supersedes**: None
- **Scope**: Display facts, surface generations, output negotiation and present ownership
- **Issue**: [RND-008.1](https://github.com/HoroCore/horo-engine/issues/338)
- **Jira**: [HORO-338](https://horo-engine.atlassian.net/browse/HORO-338)
- **Normative document**: [Runtime Lifecycle](../architecture/runtime/runtime-lifecycle.md)

## Context

The current editor queries SDL drawable size, suspends zero-sized output, and
calls `RenderFrontend::Resize` before acquiring a backend frame. OpenGL and Metal
have private presentation ports. These are useful seams, but there is no shared
display snapshot, output negotiation, or multi-surface contract yet. Without an
ownership decision, window callbacks, renderer adapters, and settings widgets
could each become authorities for resize, HDR, fullscreen, or pacing.

This ADR defines those ownership boundaries. It preserves the
[runtime frame phases](../architecture/runtime/runtime-lifecycle.md#frame-phases),
[renderer parity](../architecture/runtime/render-backend-parity-contract.md),
and [ADR-028](028-renderer-capability-limits-and-product-profiles.md) device
capability model. Renderer resource identity is
[ADR-027](027-renderer-resource-identity-and-descriptors.md).
It does not implement a display service, pick a tone-mapping algorithm, or
redesign the runtime clock.

## Decision

### 1. One owner for each lifetime and policy

| Responsibility | Authority | Allowed consumers |
|---|---|---|
| OS windows, event pump, logical size, pixel size, DPI, focus, monitor association and window/fullscreen transitions | Platform adapter, invoked by the application host on the platform owner thread | Host and presentation negotiation consume immutable Horo values; editor screens do not own native windows. |
| Display enumeration and OS-reported modes/color/HDR/VRR facts | Platform display service | Host settings/diagnostics and renderer negotiation; no GPU feature inference. |
| Native surface attachment to a host window | Matching private platform presentation adapter | Selected renderer borrows the attachment; it does not destroy the host window. |
| Device/context, swapchain/drawable acquisition, native output formats/modes, submission and retirement | Selected renderer backend | Frontend consumes typed results and surface support; editor integrations borrow admitted access. |
| Requested output, display/window preference, latency budget and explicit fallback policy | Application host configuration | Frontend validates/applies the request; UI edits intent through host commands. |
| Resolved output configuration, active surface generation and frame admission | Render frontend | Backend realizes it; host/UI observe committed snapshots. |
| Scene color, composition and final output conversion | Renderer graph/shader owners | Backend realizes the declared format/encoding; Platform never tone-maps pixels. |
| Frame pacing and present timing | Host pacing policy using backend timing/completion observations | Runtime retains simulation-clock authority; GPU completion is not a new simulation clock. |

The host composes these owners. There is no process-global presentation manager
that discovers renderers, owns all windows, and bypasses their lifetimes. Native
types, callbacks, and handles remain inside matching backend/platform targets.
UI, settings, scene systems, CLI, and MCP consume typed Horo contracts.

### 2. Keep window, display, device, and output facts separate

Logical window extent, drawable pixel extent, content/DPI scale, scene render
extent, and offscreen viewport extent are different quantities. Resizing an
editor panel can resize its offscreen target without changing the native window
or display mode. Dynamic scene resolution is not an OS resize request.

The platform publishes revisioned display/window snapshots with explicit units
and unknown/unavailable states. These snapshots include monitor association and
the provenance of reported color/HDR/refresh facts. The selected adapter owns
monitor association for a spanning window; consumers do not guess it from stale
positions. For SDL, use its reported display association and pixel-size query
on the main thread. See [SDL display association](https://wiki.libsdl.org/SDL3/SDL_GetDisplayForWindow)
and [SDL pixel extents](https://wiki.libsdl.org/SDL3/SDL_GetWindowSizeInPixels).
Query failure is an error/unknown snapshot, not a fabricated zero-sized window.

The backend separately reports surface/device compatibility: present queue or
context constraints, usable format/color-space combinations, supported modes,
image-count bounds, and output-specific limits. Device support alone cannot
prove that the attached monitor, compositor, window mode, or surface supports an
output configuration. Platform facts do not prove the backend implements it.

The frontend resolves a host request against **both** snapshot sources and the
ADR-028 effective device contract. The result records requested policy, actual
mode/format/encoding, relevant revisions, and fallback reason when permitted.
Settings display requested and active values separately. Native snapshots and
window/display handles are machine-local, never serialized as portable project
identities. Optional remembered monitor preferences must tolerate disappearance.

### 3. Explicit negotiation and color boundary

FIFO is the default required interactive presentation policy. An explicit
unsupported mode returns a typed error; no backend silently maps Immediate to
FIFO, silently enables tearing, or clamps a frames-in-flight request. An Auto
policy, when exposed, is a host-declared ordered preference/fallback list and
publishes its resolved choice. It is not permission for arbitrary substitution.

Frame rate cap, refresh rate, present mode, tearing permission, VRR availability,
queued frame count, and measured latency are distinct fields. A VSync checkbox
or a GPU capability cannot stand in for all of them. Frontend/backend admission
checks the requested frames-in-flight bound against actual device/surface
support. RND-008.5/.6 own pacing and latency algorithms within that contract.
[ADR-040](040-reconstruction-frame-generation-and-latency-providers.md)
separates real/synthetic presentation identity, frame-generation buffering and
optional latency-provider markers/wait hints. Generated cadence never changes
simulation FPS or grants authority over present mode, input or frames in flight.

Scene-linear HDR rendering is independent from HDR display output. HDR/EDR
output requires the current OS/display state, native surface/format support,
implemented final conversion, and host policy to agree. When an explicit
required HDR contract disappears: if the last committed SDR output is still a
valid drawable, **fail the HDR request** with a typed result and keep that SDR
output active; if the OS/display has already invalidated the surface (no usable
drawable), **suspend or mark Lost** and do not invent a successful frame. An
Auto policy may choose its declared SDR fallback and report the change. Never
relabel unsupported HDR output as active or reuse stale monitor capabilities
after a move/hotplug.

The graph owns final tone/color conversion and composition against the resolved
output contract. Native attachment encoding and metadata must agree with that
contract; any hardware format conversion is declared so conversion is applied
exactly once. Platform owns OS/display settings, not scene exposure. Precise
scene color, exposure and SDR/HDR transform policy is owned by
[ADR-037](037-scene-color-and-hdr-architecture.md), while output modeling remains
with RND-008.2; this ADR supplies their boundary, not a second color pipeline.

Fullscreen/borderless/windowed mode is host intent applied by Platform. Renderer
validates the resulting surface and requests reconfiguration when necessary; it
does not independently change desktop mode from a draw/present call. A rejected
transition leaves the last committed configuration active if still valid, or
reports suspended/lost output when the OS has already invalidated it.

### 4. Surface generations and safe-point transitions

Every admitted window attachment and renderer surface has an owner and
generation. Display facts have a revision; device/effective-capability identity
remains separate. Reconfiguring one surface invalidates its old acquired-image
and output-plan references, not all device resources. Device/context replacement
invalidates device-owned state under
[ADR-027](027-renderer-resource-identity-and-descriptors.md) and ADR-028.
Late resize/hotplug/completion callbacks must match the current owner/generation.

The target per-surface state model is:

```text
Unattached -> Ready or Suspended
Ready -> Suspended -> Ready
Ready/Suspended -> Reconfiguring -> Ready or Suspended/Lost
Ready/Suspended/Reconfiguring -> Lost -> Reconfiguring or Closing
Any attached state -> Closing -> Unattached (old generation retired)
```

Initial admission may enter Suspended when a valid attached window has no
drawable pixels; it must not allocate an invalid zero-sized output. Unattached
means there is no active native attachment. Lost does not mean success and
never admits a frame until successful recovery. Closing is terminal for that
generation; a later window/surface is a new generation.

Platform event callbacks publish bounded pending state and return. Coalesce
ordinary resize/DPI/display changes into at most one pending reconfiguration
per admitted surface, retaining the latest revision. Complete superseded
observable requests explicitly as superseded; do not accumulate one operation
per OS event. A different host command class (fullscreen/borderless/windowed,
explicit present-mode change) arriving while the surface is `Reconfiguring`
does **not** cancel in-flight native realization of the current revision. That
revision completes or fails; the newer command becomes the next pending
candidate for the following safe point. Close/loss signals cannot be
overwritten by a later size or mode update; they invalidate acquisition
immediately.
The initial host admits one primary surface; future multi-surface admission
has an explicit host quota and rejects excess requests before native allocation.

Apply host/window commands at `ApplyQueuedOwnerThreadCommands`. Commit renderer
surface changes at entry to `RenderExecution`, before acquiring any backend
frame for that surface. No new runtime phase or platform callback may resize a
surface with an active backend frame. When native owners are on different
threads, transfer typed revisioned requests/results through a bounded handoff;
never synchronously block one owner waiting for the other in normal frames.
The surface remains pending/suspended until the required owner acknowledges it.

Logical observability must not wait until render execution: during
`ApplyQueuedOwnerThreadCommands`, publish current window/display facts and the
negotiated **pending output candidate** with a revision and explicit pending
status. Freeze that candidate for this frame. UI layout and CPU extraction use
its current logical/pixel extents, so they do not lag a successful resize by one
frame. Keep the last successfully realized **active output** separately visible;
publishing a candidate does not claim that native reconfiguration has succeeded.

Extraction that depends on output extent/configuration carries the candidate
revision. At `RenderExecution` entry, realize that same candidate and publish
active output only on success; compile/admit the output plan against its realized
surface generation. If realization fails, remains pending, or the revision no
longer matches, skip that surface's frame rather than submit new-size extraction
to the old surface. Ordinary later events wait for the next candidate; close or
loss still invalidates acquisition immediately. This separates early logical
layout from transactional native publication without silently accepting a stale
plan or performing an extra extraction pass in the same frame.

Present remains after `RenderExecution` and `RenderGui`, as defined by the
[runtime lifecycle](../architecture/runtime/runtime-lifecycle.md#frame-phases)
phases 8–10. Domain ADRs that still group “render execution and GUI
presentation” as one coarser step (including
[ADR-014](014-sequencer-ownership-clock-authority-and-binding-boundary.md)
cinematic presentation samples) occupy that same window: after
`RenderExtraction` and before `Present`. Sequencer presentation samples must
not mutate simulation during `RenderExecution`/`RenderGui`; authoritative
cinematic writes stay on the fixed-tick seam. A runtime `BeginFrame` phase is
not permission to acquire a native drawable before presentation admission.
Failed execution or cancellation skips normal present and releases/aborts any
acquired frame exactly once through the existing frame scope. Zero drawable
extent suspends presentation without stopping simulation or creating a fake
successful native frame.

Reconfiguration prepares the new admitted configuration and publishes it only
after native success. It is subject to
[ADR-034](034-gpu-memory-and-residency-ownership.md): reserve old/new overlap
and transient presentation backing **before** native realization; if that
reservation is denied, keep the last valid active output or suspend rather
than publishing a new active extent. Keep the old configuration only while the
native platform still considers it usable. Swapchain replacement, display
removal, or context loss can make rollback to the old native state impossible:
report Suspended or Lost and rebuild explicitly instead of promising
unconditional rollback. Retire old images/synchronization objects after their
backend-specific consumers finish; CPU publication alone does not make them
safe to destroy.

On close, stop new acquisition, finish or abort the current frame as permitted,
retire queued native references, detach the backend, destroy the private surface
attachment, then release the host window on its platform owner thread. Shared
device lifetime can continue for other admitted surfaces. Only documented
bounded teardown/recovery/test waits are allowed; normal transitions use the
existing non-blocking lifecycle and [ADR-010](010-job-waiting-and-operation-store-ownership.md).
Final destruction is committed through `CommitDeferredLifecycleChanges` once
retirement is ready, or through the equivalent explicit shutdown path.

### 5. Multiple outputs and non-window targets

One primary native output is the current implementation scope; an offscreen
viewport texture is not a second native surface. The future multi-surface
contract in RND-008.8 must address every surface explicitly, isolate acquisition,
generation, extent, configuration, and failure, and bound resources per surface.
An unavailable secondary output does not stall unrelated ready outputs unless
the host explicitly selected a grouped-present policy. Grouped atomic display
presentation is not guaranteed by this baseline.

A **backend-global frame token** is the `FrameToken` returned by
`IRenderBackend::BeginFrame` in
[Rendering Architecture](../architecture/runtime/rendering-architecture.md#backend-interface):
one outstanding acquired frame per backend instance, owned by
`RenderFrameScope` until present or abort. It is not a per-window identity and
cannot silently be reused as a multi-window token. RND-008.8 must migrate that
contract before exposing multi-window presentation; the existing API does not
already implement this ADR's full target model.
Similarly, D3D12's future pre-window metadata must describe its DXGI attachment
requirements; it cannot masquerade as OpenGL or Vulkan to fit existing tags.

Headless/Null hosts have no native display/surface authority and preserve the
runtime phases with omitted presentation capability. Offscreen rendering uses
explicit target readiness/results, without inventing monitor/HDR facts.
XR compositor-owned targets retain their own acquire/wait/release and session
generations under [XR Architecture](../architecture/runtime/vr-ar-architecture.md).
Desktop window pacing, fullscreen commands, and swapchain ownership must not
take over the external XR compositor's lifecycle.

### 6. M0 migration and validation

Current `RenderBackendConfig` exposes FIFO/Immediate and a frames-in-flight
request; `RenderHostWindowRequirements` is inert metadata. The editor queries
drawable size directly and resizes before backend `BeginFrame`, but it does not
yet check the SDL query result or implement the display/output snapshot model.
OpenGL/Metal ports own native attachments, while the runtime owns graphics
resources. Preserve these seams and migrate missing responsibilities as follows:

| Owner | Required downstream realization |
|---|---|
| RND-008.2 / #339 | Revisioned display/surface/output model with Platform; explicit unknown facts, units, color/HDR provenance, and resolved snapshots. |
| RND-008.3 / #342 | Typed requested/resolved present modes, explicit ordered Auto fallbacks and unsupported results. |
| RND-008.4 / #340 | Surface owner/generation state machine, safe-point reconfiguration, actual rollback limits, and deferred retirement. |
| RND-008.5 / #341 | Host pacing based on bounded native timing observations without replacing the simulation clock. |
| RND-008.6 / #343 | Frames-in-flight admission, resource/latency budget policy and truthful timing diagnostics. |
| RND-008.7 / #344 | Platform fullscreen/monitor/DPI/resize commands, event coalescing, query errors, and generation-safe hotplug. |
| RND-008.8 / #345 | Explicit multi-surface identity/frame contract and quotas, including future native presentation metadata tags. |
| RND-008.9 / #346 | Cross-platform lifecycle, output-negotiation and GPU/presentation qualification. |

Each native backend's presentation ticket realizes its own mechanics within
this common policy. RND-013 owns scene color, RND-003 owns device capability
policy, and host/settings/package services retain their established authority.
This ADR adds no public structs, native implementation, or new settings UI.

Deterministic tests must cover unknown versus zero extent, logical/pixel size
differences, stale callbacks, coalesced/superseded requests, close winning over
resize, unsupported explicit modes, Auto fallback reporting, HDR loss on monitor
change, and frames-in-flight rejection without silent clamping. Verify failed
reconfiguration does not publish partial state, active-frame resize is rejected,
and close invalidates borrowed access before native window destruction.
Also verify that phase-4 candidate extents reach same-frame layout/extraction,
that native failure never marks the candidate active, and that revision mismatch
skips output instead of executing an extent-dependent plan on the wrong surface.

Native qualification covers minimize/restore, DPI/monitor moves, hotplug,
fullscreen transitions, repeated resize/close, unavailable drawables, and
supported SDR/HDR output paths per backend/window system. Multi-surface tests
must prove isolation and bounds before enabling that feature. Fake snapshots
and Null tests do not certify native display or pacing behavior. M0 checks
document consistency and migration ownership, not unimplemented GPU behavior.

The [presentation qualification guide](../guides/presentation-qualification.md)
defines the pass criteria and reproduces the current desktop lanes.

## Consequences

The host owns intent, Platform owns window/display facts, and Rendering owns
admitted output and GPU lifetime. This prevents UI callbacks or backend-specific
helpers from silently changing durable policy. The cost is explicit revisioned
handoffs and requested-versus-active state during transitions.

Per-surface generations and honest Suspended/Lost states allow safe hotplug and
future multi-window work without pretending every native operation can roll
back. Color, display, and device support remain composable rather than one HDR
boolean. Native qualification remains necessary even when shared contracts pass.

## Rejected Alternatives

- **Renderer owns OS windows and all display settings:** conflates platform
  lifetime, host policy, and graphics resources; prevents no-renderer recovery.
- **Resize or present directly from event/UI callbacks:** bypasses frame and
  native-thread ownership and races acquired images.
- **One global display/HDR capability or present token:** cannot model window
  moves, different monitors, output revisions, or isolated surface failures.
- **Silent present-mode/HDR/latency downgrade:** hides a failed requested
  contract; only declared Auto fallbacks may change the resolved choice.
- **Always keep the old swapchain on failure:** native invalidation can make
  that impossible; publish explicit suspended/lost state instead.
- **Use vsync/GPU completion as simulation time:** changes gameplay timing with
  display/device behavior and violates the runtime clock contract.
