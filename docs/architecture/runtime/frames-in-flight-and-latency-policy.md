# Frames-in-Flight and Latency Policy

- **Status**: Proposed; architecture recommendation awaiting human review, not accepted or implemented behavior.
- **Issue**: [RND-008.6](https://github.com/horocore/horo-engine/issues/343)
- **Jira**: [HORO-343](https://horo-engine.atlassian.net/browse/HORO-343)

## Context and scope

This document owns queue-depth and latency policy. It specializes, without
replacing, [ADR-033 presentation ownership](../../adr/033-presentation-and-display-ownership.md),
[ADR-028 effective capabilities and product profiles](../../adr/028-renderer-capability-limits-and-product-profiles.md)
and [ADR-173 submission and completion](../../adr/173-render-queue-submission-and-fence-contract.md).
The [rendering architecture](rendering-architecture.md#host-frame-pacing-and-presentation-evidence)
owns the existing CPU limiter and presentation evidence contracts. The rendering
overview and architecture index link here rather than duplicate depth/default
tables; ADR-033 and ADR-028 retain their separate ownership decisions.

The decision concerns the currently admitted single primary desktop output.
Multi-surface grouping and XR compositor scheduling remain with their owners;
neither offscreen views nor synthetic frames create extra admission credits.
Scene simulation, input semantics, quality recipes, resource budgets, present
mode negotiation and renderer selection retain their existing authorities.
This documentation change introduces no public API, persisted setting or native
implementation. Contract names below describe required downstream values.

## Recommendation

Use bounded, completion-owned admission with a balanced default of two frames.
Offer an explicit one-frame low-latency mode and a three-frame throughput mode.
Keep CPU pacing, GPU completion and display presentation independent. Never
achieve low latency by waiting for the GPU after each frame, freeing resources
early, changing the simulation clock or silently disabling FIFO.

### Distinct quantities

| Quantity | Meaning and release evidence |
| --- | --- |
| Active CPU frame | One acquired backend-global `FrameToken`, owned by `RenderFrameScope` through present/abort. There is at most one per backend instance under the current API. |
| Admitted frame credit | One frame reserved for encoding or submitted but not GPU-complete. It includes active reservations and submitted work; present return does not release it. |
| GPU completion frontier | The complete set of queue/timeline points protecting a frame's commands and resources, including parallel recording, GUI, compute and transfer use. Every relevant point must complete. |
| Presentation backlog | Successfully handed-off output not yet displayed/discarded according to qualified native evidence. This can outlive GPU completion; unknown is not zero. |
| Native image capacity | Swapchain images, layer drawables or default-framebuffer buffers. A physical pool size is neither a GPU completion count nor a latency guarantee. |
| CPU render candidate | At most one latest immutable, unsubmitted extraction candidate awaiting admission; not another queue of future rendered frames. |

All identities include their owning device/surface generation where applicable.
A frame ordinal, successful CPU encoding, present return, drawable identifier or
elapsed refresh interval cannot prove GPU completion. A command-buffer completion
cannot prove that the compositor has displayed an image.

### Modes and defaults

| Explicit mode | Maximum admitted frame credits | CPU candidate backlog | Intended trade-off |
| --- | --- | --- | --- |
| LowLatency | 1 | 1 latest candidate | Minimize Horo-controlled GPU backlog; less CPU/GPU overlap and potentially lower throughput. No promised input-to-photon milliseconds. |
| Balanced | 2 | 1 latest candidate | Default overlap with finite memory and retirement pressure. |
| Throughput | 3 | 1 latest candidate | Explicitly permitted extra overlap; higher possible latency and backing cost. Never an automatic reaction to a slow frame. |

Baseline, Standard, High and Ultra product quality profiles all default to
Balanced. Editor, Development, Profile and Shipping builds use the same default;
instrumentation does not select Throughput. Interactive hosts default to FIFO
and an independent rate cap of zero (no software cap), preserving existing host
defaults. A host/product may explicitly choose another supported mode, present
policy or 1..1000 Hz cap; changing one does not implicitly change the others.
Headless/Server/Null has no native presentation policy or display latency claim;
synthetic completion fixtures test admission without qualifying hardware.

The product supplies allowed modes and a default before composition. User/host
overrides cannot widen that policy. Resolve requested mode, exact credit count,
present mode, independent cap, capability revision, surface generation and
settings revision into one immutable admitted policy. Reject unknown values,
disallowed modes, contradictory limits or insufficient backing budget before
publication; retain the previous valid policy on rejection. No silent clamping
or backend substitution. An explicitly ordered host fallback list may select
another allowed complete policy and must expose the requested/resolved values
and reason. An omitted list grants no fallback.

The engine hard bound remains eight retained frames for general backend/resource
capacity, not a selectable latency-mode default. This policy admits at most
three credits. Effective support and finite memory/pin/recording budgets may
restrict that further; they never expand it. Checked arithmetic accounts for
per-frame upload, transient, query, GUI and resident-reference overlap before
allocation. A native pool's minimum may exceed the logical credit count: e.g.
two drawables may serve LowLatency with only one admitted frame. Such separation
must be implemented and qualified, not achieved by passing an invalid native
pool size or relabeling two outstanding frames as one.

### Admission and CPU/GPU pacing

The host owns requested policy and event-responsive sleeping. RenderFrontend
owns policy resolution and credit admission on the render-capable owner thread.
Each backend owns native completion, acquisition, submission and retirement.
Backend-private queues obey the same public contract; no peer supplies another
peer's implicit fallback or a special low-latency authority.

At each render safe point, poll no more than the admitted capacity's retained
records with zero timeout; complete bounded retirement work. Reserve one credit
before acquiring a native frame or allocating per-frame backing. The reservation
is transactional across frontend/native admission and becomes submitted exactly
once. Capacity exhaustion is typed retryable Pending/Backpressure, not fatal
device loss, success or a CPU completion wait. Return without native acquisition
when a credit is unavailable. No busy-spin loop, semaphore wait, fence wait,
device idle, callback join or unbounded queue is permitted on this path.

Before input sampling and expensive new extraction, the host must poll the
known credit/readiness state alongside `FramePacer`; this is advisory, never a
borrowed `FrameToken` or irrevocable native acquisition. Final reservation still
occurs before backend `BeginFrame`. Native readiness changing between those
points yields backpressure, not a hidden retry loop in one frame. Continue
Platform event pumping, cancellation and Runtime's existing clock/fixed-tick
policy. Any host scheduling sleep is at most the existing two-millisecond slice,
then readiness/cancellation are observed again. Rebase late cap deadlines;
never generate catch-up render submissions or queue stale input snapshots.

If simulation advanced while rendering was unavailable, retain only the latest
unsubmitted extraction candidate and its complete generation/revision identity.
Superseding it releases only CPU-owned unsent leases. Revalidate on admission;
never repeat simulation to retry rendering or replay a stale candidate merely
to fill the queue. Submitted work cannot be replaced by newer input. Pending
surface changes suspend native acquisition under ADR-033 while host lifecycle
continues; the existing scheduler decides simulation suspension independently.

GPU dependency waits remain GPU-side submission records under ADR-173. CPU
readiness is not GPU readiness. A completed graphics marker releases a credit
only if it covers all frame consumers; independent queues require the full
completion frontier, not an assumed shared physical queue. No normal-frame
`glFinish`, command-buffer completion wait, device-idle wait or blocking fence
wait may enforce a mode, reclaim a slot, resize or change policy.

### Native acquisition and presentation limitations

Native drawable availability must be tracked separately from GPU credit. Do not
enter a potentially blocking acquisition merely because a command completed:
the display system can still hold its image. Readiness observations must retain
their provenance and cannot fabricate a zero-time acquire guarantee. Native
present calls may be constrained by the window system; measure their CPU call
time separately, never disguise it as GPU completion or qualified scanout.

No engine-inserted normal-frame GPU synchronization is allowed to compensate
for those constraints. A backend advertises a nonblocking acquisition or strict
latency-budget capability only after its actual private mechanism and host
event responsiveness are qualified. If it cannot meet an explicitly required
nonblocking/latency contract, return Unsupported at policy admission; an explicit
fallback may choose the independently supported baseline. Do not silently turn
off FIFO, move native work to an arbitrary worker or advertise platform stalls
as solved. Baseline native API limitations remain observable migration gaps,
not a permission to add waits or a claim that every present call is nonblocking.

### Lifetime, cancellation and reconfiguration

An unsubmitted abort returns its reservation after unsent references release.
An aborted frame that already queued native commands retains its credit,
resource pins and backing charges until exact completion or explicit native
ownership-domain teardown. Cancelled worker recordings retain their finite
recording/parent budget until callbacks have actually drained; cancel intent
alone releases nothing used by another thread. Late completions cannot retire
a newer device/surface generation's slot.

For policy changes, validate the complete candidate at an owner safe point with
no active frame. Existing credits retain their original leases. When shrinking
below current occupancy, publish the new policy as pending, close new admission
and poll old work without waiting; commit only when occupancy fits the new bound.
Allocate growth/old-new overlap within the existing residency budget before
commit. Rejection preserves the old valid policy; close/loss outranks pending
changes. No live ring resizing or reusing old slots to manufacture a lower count.

Shutdown closes admission and native timing publication before draining or
abandoning owned work through the established backend/device-loss path. Explicit
bounded teardown, recovery and deterministic tests may use their declared finite
wait purposes; timeout/error never becomes successful completion. The existing
ADR-173 bounded-readback exception remains restricted to its explicit operation,
never ordinary frame pacing. Resource destruction still requires native safety
evidence or documented ownership-domain destruction, not CPU cancellation.

### Latency observations and optional providers

Publish requested/resolved mode and credits, current reserved/submitted counts,
capacity deferrals, pending policy revision, CPU present duration and separately
qualified GPU/display timestamps. Presentation backlog, refresh estimate and
input-to-display latency are optional/unknown unless the actual identity and
clock-domain evidence exists. Diagnostics must distinguish unsupported, pending,
discarded, stale, failed and complete; absent metrics are never zero latency.

One-frame admission bounds Horo's GPU backlog, not the driver's compositor queue
or end-to-end latency in milliseconds. A required hard millisecond budget needs
qualified evidence/admission for the complete path; unknown support rejects
that requirement. Do not claim a universal 16.7-ms or one-refresh guarantee.
[ADR-040](../../adr/040-reconstruction-frame-generation-and-latency-providers.md)
owns optional provider registration, real/synthetic identities and marker/wait
hints. Providers cannot increase credits, replace simulation/input clocks or
insert CPU/GPU waits on the normal path. Generated-frame buffering consumes
declared finite resources and participates in complete policy admission.

## Current implementation and migration

These are inspected source facts, not freshly executed native qualification:

Source evidence: [backend configuration](../../../include/Horo/Runtime/Render/RenderBackend.h),
[CPU pacing](../../../src/runtime/renderer/api/FramePacing.cpp),
[OpenGL credit polling](../../../src/runtime/renderer/modules/opengl/OpenGLExecutionAdapter.cpp),
[Metal acquisition and capacity](../../../src/runtime/renderer/modules/metal/MetalRuntime.mm),
[Metal completion retirement](../../../src/runtime/renderer/modules/metal/MetalSubmittedGraphQueue.mm),
[editor host integration](../../../apps/HoroEditor/app/HoroEditorApp.cpp) and
[current Vulkan boundary](../../../src/runtime/renderer/modules/vulkan/VulkanRenderBackend.cpp).

| Current seam | Required downstream migration |
| --- | --- |
| `RenderBackendConfig` defaults to2, validates1..8; effective limits also bound8. | Add typed mode/requested-resolved policy and distinguish credit capacity from native image count; stage persisted legacy count migration rather than add two writable authorities. |
| OpenGL `OpenGLExecutionAdapter` polls configured fences with zero timeout and retains command-stream leases; SDL port swaps the window. | Preserve fences after abort/GUI use; expose common credit/backpressure facts and qualify actual surface/present responsiveness. A double-buffered framebuffer is not a measured two-frame GPU queue. |
| Metal `CheckSubmissionCapacity` polls retained commands and worker recordings; `maxFramesInFlight`2..3 also sets layer drawable count. | Separate logical credit limit1/2/3 from the admitted native pool; account for compositor-held drawables and cancelled CPU leases. Existing acquisition does not prove strict nonblocking behavior. |
| Metal calls `CAMetalLayer.nextDrawable` on the owner thread. | Apple documents that acquisition can wait up to one second by default; completion-capacity checks alone do not remove this possibility. Qualify a private readiness/acquisition mechanism or leave the stricter capability unsupported. |
| `FramePacer` limits CPU cadence only; editor `WaitForFrame` pumps events/sleeps<=2ms, and later `BeginFrame` errors propagate. | Integrate retryable capacity as output deferral without terminating normal host work; add advisory early credit observation, final reservation and latest-candidate handling. Do not reinterpret rate-cap Ready as GPU capacity. |
| Vulkan `BeginFrame`/Execute/Present currently return Unsupported; Null supplies logical test behavior. | Implement and qualify the same admission/frontier/retirement contract in Vulkan and future D3D12; Null cannot establish native pacing or display latency. |

Apple's [nextDrawable contract](https://developer.apple.com/documentation/quartzcore/cametallayer/1478172-nextdrawable)
and [timeout policy](https://developer.apple.com/documentation/quartzcore/cametallayer/allowsnextdrawabletimeout)
are native constraints, not a Horo low-latency implementation. SDL's
[SwapWindow contract](https://wiki.libsdl.org/SDL3/SDL_GL_SwapWindow)
specifies the main-thread presentation boundary; its return is not display-time
evidence. Native qualification must record OS/backend/device/driver/surface,
mode/counts, workload, build profile and actual timing before performance claims.

The inspected editor supplies `maxFramesInFlight = 2` in application composition;
no persisted depth setting was found in the inspected editor/application owners.
The migration rules below constrain hosts/products that carry an explicit legacy
count and any future persisted policy; they do not claim a current schema exists.

The downstream frontend/host work owns the pure typed resolver, finite credit
records, policy transitions and output deferral. Concrete backend presentation
owners implement private image readiness/completion translation. Settings owns
legacy migration and requested/resolved display; resource/graph owners consume
admitted counts for finite storage rather than inventing their own defaults.
Legacy explicit counts1..3 map exactly to the corresponding mode only if product
policy and effective support admit them. Counts4..8 or contradictory old/new
fields require a typed migration diagnostic and explicit user choice; do not
silently truncate or lose authored intent. Persist through the established
configuration/project migration transaction. Rebuild all affected consumers
if shared API/layout changes; this decision itself changes none.

## Alternatives and consequences

- One frame for every workload: smallest controlled backlog, but disallows an
  explicit overlap/throughput choice and may waste CPU/GPU parallelism.
- Three frames by default or adaptive unbounded depth: can hide spikes with
  additional latency/memory; rejected as an implicit policy change. Explicit
  Throughput remains bounded and observable.
- Native image count defines latency: rejected because acquisition capacity,
  GPU completion and display ownership have different lifetimes.
- GPU-idle/fence wait after every frame: rejected; serializes the frame path,
  hides backpressure and violates normal-frame ownership/response constraints.
- Quality profile implies depth, immediate present or a vendor latency provider:
  rejected; quality, present negotiation and latency capability are independent.

The recommendation costs explicit admission state and migration of ambiguous
count fields. It bounds Horo-controlled backlog and retained memory without
promising unsupported native latency. LowLatency may reduce throughput;
Throughput may increase latency. No measured speedup is asserted by this decision.

## Validation obligations and review state

Downstream deterministic coverage must prove exact1/2/3 occupancy boundaries,
invalid/disallowed modes, unknown capacity, overflow/overlap budgets, no silent
clamping, failed admission with no acquired token, and no queue growth. Exercise
partial multi-queue completion, submitted abort, cancelled live workers, latest
candidate supersession, stale generations, shrink/growth/close races, and timeout
without false retirement. Verify policy/cap/present independence at every quality
profile, responsive two-ms host slices, and unsupported hard latency requirements.

Each advertised native tuple must qualify acquire/present saturation, FIFO and
explicit modes, minimize/restore/reconfigure/loss, repeated cancellation/shutdown
and resource pins under real GPU load. A delayed completion fixture must prove
that normal admission returns promptly rather than wait. Presentation backlog
and latency need actual native evidence, not assertion counts or frame indices.

This decision-only ticket validates links, terminology, defaults, ownership and
consistency plus all-provider Markdown preflight in the actual PR worktree.
No GPU/native tests or migration implementation are claimed by documentation.
Status remains Proposed until human architectural review accepts it; publication
or ticket review does not manufacture acceptance or downstream completion.
