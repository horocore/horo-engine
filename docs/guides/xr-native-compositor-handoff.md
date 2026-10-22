# Native XR frame and compositor handoff

`OpenXRFrameCompositor` is an explicitly composed, non-installed XROpenXR host adapter.
It executes official `xrWaitFrame`, `xrBeginFrame` and `xrEndFrame` dispatch using the
retained native session. `XRFrameLifecycle` remains the single logical ordering and
acquisition/release authority. The adapter does not start a session, discover a
renderer, locate views, acquire images or manufacture GPU completion.

The application host retains the loader, started native session, logical session/frame
gates, exact reference-space owner, swapchain owners and selected Renderer device.
They outlive the adapter and every synchronous native borrow. All operations are
serialized on its construction thread; there are no native callbacks into engine state.

## Explicit sequence

1. Configure the existing frame gate, then bind official dispatch and finite pacing
   budgets. The release-complete path supports exactly one frame in flight; other
   queue-depth requests fail explicitly before native work.
2. Call adapter `Wait`. A failed native wait cancels only its reserved logical frame.
   Every successful native wait returns a retained frame identity, even when its
   render-admission status is non-Ok. Call `Begin` for that exact frame. The private
   adapter retains the raw wait timestamp for exact native zero-layer cleanup even
   when that value cannot enter Horo's positive prediction-time contract.
3. Locate views through the selected native space owner and record the complete view
   count in the frame gate. Build the existing immutable `XRViewRenderPlan` from
   current prediction-purpose poses, asymmetric projection and acquired target facts.
4. Acquire/wait/import the native swapchain images through their existing owners.
   Record acquisitions in the frame gate, render through the canonical frontend and
   observe actual completion. Record logical `Submit` only with complete completion
   evidence. Poll native image retirement/release without waiting for device idle;
   record every successful native release in the frame gate.
5. Call `EndProjection` with that same plan and one owner per unique released image.
   The adapter fences frame membership and the exact latest released acquisition,
   native allocation format/extent/layer/sample/role, complete stereo views, current
   reference space and depth conventions before encoding one projection layer.
   Validation failure performs no native end. The host may fix inputs or choose
   logical `Abort` and `EndEmpty` after completing image releases.
6. A runtime-disabled frame uses `Begin`/`EndEmpty` without image work. Shutdown closes
   new waits while allowing the outstanding frame to retire. A native begin/end
   failure fences further work; the host must prove native quiescence and retire
   Renderer/image leases before resetting the adapter and logical gate. Reset is
   never evidence of native success.

## Timing and color/depth policy

`ObservePacing` takes an explicitly converted timestamp in the runtime prediction
clock domain. It reports the runtime period, exact predicted display time, finite
CPU submission deadline (`prediction - GPU budget - compositor margin`), remaining
budget and CPU/display deadline misses. It never sleeps, polls a GPU, reads a wall
clock, assumes a refresh rate or changes the foundation wait/begin/end order.
Periods too small for the admitted budgets, invalid clock evidence and stale frames
are actionable failures. A missed deadline never removes an outstanding cleanup
obligation. The host uses these observations before issuing rendering work.

`RuntimeDefault` deliberately leaves runtime compositor primaries unchanged. Explicit
Rec.709/Rec.2020 require negotiated `XR_FB_color_space` and successful official native
selection at binding. There is no silent color fallback. Transfer encoding remains
the immutable target format: an sRGB target performs its declared transfer conversion;
a linear target contains linear output. Horo does not apply another transfer function
while constructing a projection layer.

Depth requires negotiated `XR_KHR_composition_layer_depth`, exactly one depth target
per view, matching color/depth rectangle dimensions and explicit normalized depth
range plus finite positive, unequal metric clipping planes. Reversed depth uses
reversed metric near/far values. Missing/partial depth and unsupported blend/view
profiles fail before native submission. The initial production path accepts complete
primary opaque stereo; bounded storage does not imply quad/mono production admission.

## Contract migration and evidence

The existing `XRFrameLifecycle::ValidateCurrent` predicate is exposed for native
preflight so replacement between wait and begin is rejected before native work.
The additive `ValidateEnd` separates preflight from committing a successful native end. `ValidateReleasedImage` proves that each encoded image belonged
to and was released by that exact logical transaction. Existing frame-gate callers
remain unchanged. Native callers use preflight, native end, then logical `End` under
the same serialized host operation. `OpenXRSwapchain::BorrowReleased` exposes only a
synchronous private borrow invalidated by successful reacquisition, loss or Close.
No native types were added to installed headers and no second frame authority exists.

`HoroXROpenXRTests` covers the adapter through official-signature scripted dispatch:
projection array layers/FOV/pose, complete depth chains and reverse depth, release
completion, stale space/image fences, failed dispatch, invalid budgets, missing
functions, single-slot backpressure, missed deadlines, zero-layer cleanup and shutdown.
`HoroXRRuntimeTests` covers nonmutating end/release preflight. The dedicated
`HoroXROpenXRHostInterfaceConsumer` checks the private header boundary.

These scripted results are contract evidence, not hardware qualification. Physical
OpenXR/runtime/driver timing, depth, color and visual evidence remains required for
each supported tuple. Vulkan desktop WSI and Vulkan editor GUI composition are
separate responsibilities and are not implemented by this XR adapter.

The native-private `Bind` surface takes `NativeCompositorDispatch`, a value holding
only the official loader resolver. Embedding callers migrate from passing the
resolver directly to passing `{resolver}`. The same session/loader lifetime is
required; null dispatch and failed resolution publish no adapter state. This
keeps official ABI dispatch distinct from helper callable templates without heap
allocation or another session/frame owner. Native binding rejection and frame
cleanup regressions cover this migration; the new source awaits revalidation.
