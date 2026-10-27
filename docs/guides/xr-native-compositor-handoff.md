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

## Explicit authored tracking root

The selected editor XR host requires one authored `SceneObjectId`, supplied by the
installer-owned host services or explicitly selected with `--xr-tracking-root <id>`.
The invocation only selects an object; it grants no package, signature, renderer or
runtime availability. Duplicate, malformed, zero, missing-value and conflicting
host/invocation selections are rejected. There is no camera or first-object fallback.

Scene preparation must resolve this exact object through the existing canonical world
transform resolver. Only a finite rigid positive-orientation transform is admitted;
scale, shear and reflection are rejected (orthonormality tolerance `1e-5`). The
retained value maps native Local/Stage base coordinates into authored World. No
Scene entity/view/component borrow is retained by native frame retirement.

OpenXR's `poseInReferenceSpace` instead expresses the new space in its parent base,
so the actual World reference child is created using the rigid inverse of that
mapping. World is a distinct native child and receives its own Horo space identity
only after successful native creation and canonical session binding. Its space
budget is additional to Local/Stage, View and action spaces. An absent mapping
admits no World borrow; stale session/origin/kind and retirement reject old borrows.

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

The selected App owner treats Lost as destructive recovery, separately from normal
STOPPING. It revokes its exact Scene/Input publication and closes new waits, while
retaining the acquired image, compositor protocol and same scene/GUI completion
leases. It polls those leases and destroys the swapchain/views only after actual
graphics retirement. A native release failure remains an error; lost-image
destruction never supplies a successful composition-layer release receipt.

Only after graphics children retire does the same application session destroy its
actions/spaces and call `xrDestroySession`. Failure retains the issued candidate and
all remaining cleanup state for retry. Success is stored as an exact candidate-owned
physical receipt before resetting the compositor/logical frame gate. Canonical host
retirement then releases that occurrence while preserving its session-generation
history, permitting a new higher-generation Play. Ready never revives the Lost
occurrence. A later outgoing Scene retirement accepts only the exact already-drained
Scene identity and cannot revoke a replacement. This modifies no Scene authority.

This recovery follows the OpenXR [session destruction contract](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#xrDestroySession):
destruction is legal in any native session state after concurrent session calls stop.
The stricter [swapchain destruction contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrDestroySwapchain.html)
still requires completion of every submitted graphics command referring to it.
Resetting an engine gate or observing a lost event does not meet either obligation.

## Private header and consumer boundaries

The existing non-installed `HoroRenderVulkanHostInterface` is the canonical
configured header set for Vulkan host types. Backend implementation and whitebox
test composition link it privately. Their private implementation headers use angle
includes for `VulkanBackendModule.h`, `VulkanEditorGraphicsBridge.h` and
`VulkanHostGraphicsRequirements.h`, so a translation unit cannot combine the staged
host contract with a second physical source-tree copy. The source declarations
still own those types; no installed header, native ABI or public dependency changes.

Tests that compile the shared renderer backend fixture explicitly link the existing
non-installed `HoroRenderExternalImagesInterface`; Runtime UI gains no production
dependency on backend-private contracts. Lifecycle enum representation checks
belong to the XR Runtime public-header consumer. The XR Input Bindings consumer
retains its input-only dependency closure instead of gaining Runtime visibility
solely for those unrelated checks. These consumer builds qualify the corrected
header edges; they do not replace native Vulkan App or physical-device evidence.

The existing non-installed `HoroEditorVulkanXrHostInterface` forwards its access
header to the canonical source declaration. The complete borrowed
`EditorViewportTextureView` lives once in the backend-neutral internal
`EditorViewportTextureView.h`, included by both the ordinary viewport contract and
that access contract. App frame composition needs no repository-wide source
include path or complete viewport renderer class merely to consume a mirror value.
The real application-host consumer explicitly links the same internal interface
and checks the complete value/default bindings, rather than compiling only a
member pointer that permits an incomplete return type. This moves no declaration
into the installed API and changes no field, GUI identity, sampled generation,
resource owner or completion lease.

## Timing and color/depth policy

`ObservePacing` takes an explicitly converted timestamp in the runtime prediction
clock domain. It reports the runtime period, exact predicted display time, finite
CPU submission deadline (`prediction - GPU budget - compositor margin`), remaining
budget and CPU/display deadline misses. It never sleeps, polls a GPU, reads a wall
clock, assumes a refresh rate or changes the foundation wait/begin/end order.
Periods too small for the admitted budgets, invalid clock evidence and stale frames
are actionable failures. A missed deadline never removes an outstanding cleanup
obligation. The host uses these observations before issuing rendering work.

The actual App adapter samples the enabled same-instance timespec/QPC conversion
again before acquisition/binding. Both this result and `xrWaitFrame` prediction are
official `XrTime` nanoseconds, so the private clock boundary can label the converted
sample for presentation deadline comparison without a Foundation/simulation epoch
cast. A missed CPU/display deadline aborts rendering and progresses zero-layer
cleanup through the same tail; it adds no sleep, desktop camera or second limiter.

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

### Same-owner native Ready after Idle

`XRSessionEvent::Ready` is appended with value 9; existing event values 0–8 and
all identity layouts remain unchanged. Native adapters must fence the actual
native session incarnation before normalizing Ready. Idle revokes exposed session,
capability and accepted-plan publication, while the lifecycle privately retains
that exact owner's sealed plan and resources. A subsequent genuine Ready can
republish only this same owner and revision; it does not issue a candidate,
rediscover capabilities or repeat resource preparation. Frames remain denied until
Started, and actions until focus is established again. Loss (including loss while
Idle), replacement and shutdown revoke this restart path. Consumers with exhaustive
event switches or closed-range validation must add Ready; no persisted event codec
or wire format is introduced. The staged Input public consumer checks the unchanged
old values and the appended value. Lifecycle regressions cover two complete cycles,
failed replacement rollback, foreign events and irreversible loss/shutdown.

### Cold topology with exact visible-read binding

The noninstalled graph template can declare a closed set of optional persistent
vertex/index/uniform reads for one graphics pass. A bind selects exact unique
resource/role pairs in existing bounded storage; the original attachment writes,
pass identity/order/queue and synchronization/retention superset do not change.
All resident superset generations are validated even when a read is not selected,
because a precompiled barrier can still touch it. Stale unused backing therefore
rejects and requires lifecycle rebuilding. The native multiview semantic validator
is unchanged and still compares the bound pass's uses to the actual visible draw
set. There is no hot graph compilation, heap fallback, independent schedule, or
relaxed workload predicate. Mutation is legal only on a retired unqueued instance;
the existing combined scene/GUI completion lease alone permits its next bind.


### Executable-owned installation bootstrap

The actual `HoroEditor` entry point calls the installed-host factory during
`PrepareEditorStartup`, before backend/window creation. Ordinary desktop startup
supplies none of these inputs and creates no XR owner. An explicit XR launch must
supply all three normalized absolute paths together:

```
--xr-host-roots <independently-approved-publisher-policy.json>
--xr-host-catalog <signed-xr-host-catalog.json>
--xr-host-catalog-signature <detached-signature.json>
--xr-tracking-root <authored-scene-object-id>
```

The roots document uses the existing package publisher-policy codec. Its source
is an explicit operator/installer approval; neither project metadata nor a
catalog signer can select or create those roots. Parsing remains separate from
approval. The canonical decoder takes an explicit policy seed, preserves its
resource/audit bounds and existing records transactionally, and rejects a
combined publisher count above the caller's bound. The executable's fresh
approval snapshot deliberately starts with an empty default policy, rejects
unsigned policy, and builds the actual `ArtifactVerifier` from active approved
records only. No release key or implicit development root is compiled in.

The bounded schema-one signed XR catalog selects the package, component,
deployment and binding document paths, exact publisher/key, native bridge
contract and finite product intent. Embedded loader/deployment signature
envelopes remain distinct artifact evidence, bound to that same signer. Catalog
verification does not install a package, verify those other bytes, open a native
library, establish dependency closure, or grant runtime/renderer availability.
The existing component/deployment/loader gates still enforce those obligations.
In particular an absolute loader path and `RTLD_LOCAL` do not constrain ambient
SONAME resolution; the explicit signed deployment contract must admit the exact
remaining OS ABI closure before loading.

The factory drives the real canonical `PackageProjectHost` in offline install
mode using its existing sealed install publication. Its startup-only wait is
bounded to ten seconds and never activates arbitrary extension contributions.
Physical XR declarations are read only from that exact installed archive and
joined to the actual router's existing actions/contexts/player assignment. The
same host's worker scheduler is borrowed by the editor and its shutdown guard;
there is no second package store or independent worker lifetime. Immutable
catalog/root expiry and package/configuration generations are rechecked at the
existing application admission boundary. Native children and completion debt
must drain before these parents are released.

This source path requires externally provided genuine signatures, qualified
native deployment and compatible runtime/device evidence. The existence of the
factory, a valid catalog, or a successful offline install is not native XR or
compositor qualification. The acquired-eye Scene/frame/mirror path and its
actual completion/retirement coverage remain separately required.

## Play retirement and retained ownership

The non-installed `XRSessionHostControl` joins the **same** session coordinator,
frame gate and resource adapter. It is not a second session issuer. The selected
host first progresses physical native session end and scene/UI/GUI/image lease
retirement; its resource adapter then observes that completed cleanup. Pending
cleanup or the original native failure preserves ownership and publication. A
running epoch, outstanding frame, foreign gate/resource adapter, stale candidate
or repeated retirement is rejected before ownership can be discarded.

Successful retirement releases completed-stage bookkeeping in reverse order and
clears the old configuration and plan. It preserves both monotonic issuers, so a
later Play receives a higher session generation without reconstructing the
coordinator or resetting its clock. `Shutdown` remains permanent. A Lost owner
cannot return to Ready: explicit recovery must first prove native and queued
resource cleanup, retire that owner, and activate a new generation. No normal
frame waits or caller-supplied completion booleans authorize this transition.

Native system identity is likewise issued only after successful actual system
selection. It is fenced by the admitted preparation and runtime generation,
survives session-only retirement, and advances after native instance/system
rediscovery. This identity alone grants no capability, session or frame
admission. Existing public session/resource declarations keep their layouts and
virtual contracts; the host control and its fallible cleanup observer are staged
only through `HoroXRRuntimeHostInterface` and are not installed.

The selected Vulkan viewport cold-prepares its eye graph and complete resident
buffer/barrier superset before interactive admission. Fresh acquired color and
per-layer views are rebound through that existing canonical template; dynamic
selection never compiles a graph or skips validation of unselected retained
resources. A cold control block retains fixed snapshot storage through queue
completion; successful binding constructs only fixed value storage and aliases
that owner. Normal desktop execution and its texture accessor remain separate;
the XR mirror accessor returns only the actual admitted first-eye sample.
These new source paths require their next owning compile, CPU regressions and
warmed-allocation measurement; they do not constitute native execution evidence.

`EditorViewportResources::PrepareMeshesCold` is the explicit load-time admission
for this graph. It advances the existing resident buffer-to-mesh dependency
stages through the same frontend upload queue, with at most the initial queued
request count plus three requests per incoming mesh worth of CPU drain calls.
Per-drain request/byte limits remain effective; no GPU completion is awaited.
Success requires the exact requested generation, Ready buffers and mesh, and
their actual descriptors before graph imports. A stalled/exhausted preparation
returns typed pending while retaining its partial resources; completed failures
preserve their original error. Pending work may resume; failed partial resources
remain with the existing viewport owner until its explicit `Shutdown` recovery.
XR graph retirement cannot clear unrelated or unstarted shared viewport state.
Canonical frontend cancellation/retirement keeps queued references alive. The
incremental desktop `Prepare` and `PrepareMeshes`
contracts are unchanged; the selected XR graph caller migrates to the cold method.


### Camera-free selected Play geometry and Scene ownership

The non-installed `EditorXrSceneSourcePort` comes from the existing Workspace
controller's document, primitive cache and imported mesh cache. It holds only a
weak owner-lane fence. Capturing it does not select a backend, start Play, load
another asset or retain the controller. Capture checks the exact definition and
document revisions, then maps authored object IDs onto the real unpublished
Scene's `EntityRef` generations. Parent order and capacity storage are prepared
cold. The normal update validates the entire mapping and computes into owned
scratch before committing instance transforms; it retains no Scene/component
pointers. Cancellation, source edits, cache-clear generation changes and owner
retirement deny future reads while already-owned immutable mesh leases survive
until their queued renderer references retire.

`EditorPlayViewSource::SelectedXr` is deliberately private. Its Start call requires
an actual selected host and a factory whose produced host identifies that exact
XR participant. The real Project Play factory registers the participant before
UI. Missing/wrong/failed XR composition rejects before Scene publication; the
ordinary authored-camera default and desktop extraction capacity remain intact.
The XR-only 128-instance policy never truncates the desktop scene. Imported meshes
must already exist in the canonical cache for XR; no missing resource is silently
omitted or substituted.

The App participant prepares native actions and the exact tracking-root value,
then obtains observed projection evidence before the sole XR lifecycle issues a
candidate. That candidate activates the SAME prepared native session and its
swapchain, graph and Input reservation. Scene publication enables only this
prepared producer; native Ready/Begin events still govern frame admission.
Outgoing Scene Shutdown closes its exact generation and retains native/renderer
debt in the app. It does not retain an outgoing Scene view, release an image from a
CPU callback or manufacture a compositor success. Typed release errors retain the
unreleased suffix for retry. The existing presentation recovery owner destroys
this host before its native startup/device/loader parents only after actual drain.

### Native eye observation identities

Native base/head/action/World spaces and logical eye observations use the SAME
bound per-session semantic-space allocator and generation. After an actual
primary-stereo configuration census, presentation setup reserves distinct eye
identities cold. Only a complete successful `xrLocateViews` publishes them with
that exact session, origin, runtime ordering and independent native validity flags.
They describe observed view coordinates; they are not native `XrSpace` children
and cannot pass the private native World-space borrow. Loss, retirement or a
replaced bound space generation revokes them. No center-head space is relabeled
as an eye, and no pose validity is synthesized from an identity or default value.


### CPU GUI projection and acquired-eye binding

The selected Vulkan viewport keeps the existing reserved GUI image identity while
its cold XR graph or retirement debt is owned. `TextureView()` projects only this
logical identity during CPU GUI collection, with no sampled view. It does not
change physical `IsReady()`, issue a native resource, or prove session/image
availability. Failed preparation and pending retirement cannot silently expose a
desktop-camera image. Ordinary desktop projection resumes only after the same XR
owner is actually retired.

After the same App frame executes the acquired-eye graph, the private
`OpenXRPlayHost::MirrorView(frameNumber)` borrows the full existing
`EditorViewportTextureView`. The App frame number must match the successful
canonical binding; wrong-lane, retiring/lost, cancelled, already-GUI-finished and
unsubmitted acquisitions produce an empty value. The first-eye sampled generation
must exactly match the retained acquisition. This is synchronous GUI encoding
input, not a transferable ownership capability. The same scene and GUI queue
leases retain the native image through the exact post-GUI state restoration and
completion tail. The GUI resolver still requires the captured logical identity
and a valid sampled generation; it cannot resolve the CPU-only projection.

The real App now joins this contract after the ordinary UI-parent adoption:
CPU GUI/Update/Draw and cold Scene preparation precede the sole native Begin;
selected-XR preparation and graph execution use that same scope, followed by the
existing UI/GUI/Present tail. The Game panel projects the unchanged private
`EditorPlayViewSource` values, now declared alongside `EditorPlayState` in its
existing non-installed host interface. Ordinary authored-camera callers keep the
same defaults. A selected-XR image has a localized left-eye mirror label and does
not weaken physical `IsReady()` or reinterpret the logical CPU projection as
readiness. An empty acquired binding fails explicitly instead of selecting a
desktop camera. These new caller joins remain uncompiled/unrun; they do not
establish native headset, GUI pixels, full-App or hardware qualification.

The app-selected XR startup negotiates Immediate output before any image
acquisition. The canonical native wait supplies pacing only when the same session
admits frames and the same frame gate is idle; pending native/queue debt keeps
selection but uses the existing desktop event-loop cap. `FinishGui(false)` and
same-scope reset run on native-Begin/scene/UI/GUI failure, preserving the triggering
error. `RuntimeHost` immediately shuts down after phase failure, so normal
`EndFrame` alone is not a cleanup guarantee. The retained presentation teardown
owner keeps device/session/image parents and polls actual retirement after such
failure. On the success path `EndFrame` polls the same compositor tail after
presentation resets the scope; desktop Present success is never an XR release or
compositor receipt.


The fixed private `EditorVulkanXrMirrorProjection` is the viewport graph's actual
receipt storage. Normal next-acquisition clearing preserves selection; exact
Scene loss/retirement uses `Revoke()` and cannot rebind that graph. Only terminal
physical retirement removes its logical identity. The four owning CPU regression
cases cover unacquired projection, exact frame/view generation and invalid atomic
binding, revocation without desktop fallback, and irreversible retirement. They
are registered with `HoroRenderVulkanTests` only when the actual editor Vulkan XR
interface exists; no native or GPU execution is inferred from these value tests.
This tranche's new tests and production changes have not yet been compiled or run.


A minimized or unavailable desktop mirror does not starve a running native
session: the same RenderExecution phase still services selected-XR prediction,
then explicitly cancels that prepared acquisition and finishes the existing
zero-layer tail when no CPU mirror extraction was admitted. It creates no desktop
frame scope, replacement camera or successful projection receipt in that path.
The native pacing and acquisition calls are the same owners as the visible path;
no second counter or presentation clock is introduced.


### Application clock ownership

The noninstalled `HoroOpenXRApplicationClock` target owns the single production
`OpenXRInputClock.cpp` implementation and its staged private host header. It depends
on the existing native-session host interface and Runtime scheduler contracts, and
has no renderer selection or Vulkan device ownership. `HoroOpenXRApplicationHost`
links this same library instead of compiling a second copy. XR-off compositions
create neither target; headless XR tests may exercise the actual host-platform
clock conversion without enabling a concrete renderer.

The clock tests use official scripted conversion dispatch on the existing native
owner and real scheduler callback evidence. Such tests prove lifecycle and time-domain
behavior; they do not qualify a platform OpenXR runtime, Vulkan application linkage,
or headset operation. After this ownership change, hosted application qualification
must include the six remaining application TUs plus the same clock library and the
real application consumer/editor link.


### Borrowed native dispatch migration

`NativeOpenXRDispatch` is the single noninstalled value for an official OpenXR
resolver borrowed from the already retained loader/instance. It replaces the
compositor-specific value at the existing native host boundary. Clock binding,
event binding, presentation action preparation and graphics preparation consume
this same value. Callers pass `{resolver}`; implementations retain the exact official
PFN ABI and perform the same null/currentness checks. This migration adds no
callable owner, heap allocation, native factory, session issuer or installed API.

The affected application calls and native graphics/event/pose/clock fixtures migrate
in the same batch. Existing native calling conventions and loader retention remain
part of the regression obligations; a generic `std::function` is not substituted
for an official foreign ABI function pointer.

The noninstalled clock ownership graph is enforced in `HoroDependencyPolicy.cmake`,
including the XR-disabled absence check. `HoroOpenXRApplicationClockConsumer` compiles
only the staged clock header and its declared usage requirements; the clock tests
build that consumer before exercising the same production clock library. This is
compile-only boundary evidence, not an additional runtime test or native-device claim.

### Const Input collection query migration

`InputRouter::ActionCollection` is a const query because it reads the current
routing, configuration and player assignment to return the same weak collection
port. It retains the existing owner-thread and lifetime/context validation;
querying does not register a source or change the cutoff ledger. Ordinary calls
through mutable routers keep working. Code storing an exact pointer-to-member
must use the const-qualified function type, and Input consumers must recompile.
`InputCollectionPublicContract.cpp` checks use through a const router; the existing
player-assignment collection regression now queries through a const reference
before and after assignment, then exercises normal commit and stale-assignment
rejection. `Append`, `Commit`, `Cancel` and mirror revocation retain their mutable
cutoff/publication command contracts.

### Complete-view depth admission

The Vulkan viewport previously requested two-layer depth through ordinary
`CreateTexture`, whose implemented frontend envelope is deliberately single-layer.
The matching viewport now calls the noninstalled
`RenderFrontendResourceAccess::CreateMultiviewDepth`. This cold-only operation fixes
the descriptor to Depth32Float, attachment-only, empty initial data, one mip and one
sample; its view count must fit the selected backend's existing complete-view limit.
It queues through the same frontend registry, memory scopes/budget and upload owner.
Pending, failed, replacement-generation and deferred-retirement rules remain unchanged.

Vulkan initialization queries the exact D32/2D/optimal/depth-attachment image tuple.
The published complete-view count intersects implemented views, framebuffer layers,
physical array layers and the queried format's array limit. Missing query dispatch
or a native query failure retains its typed error; an unsupported tuple advertises
no complete views. No native query runs during ordinary frames. Where complete views
are available, the common `maxTextureDimension2D` bound conservatively intersects the
queried depth extent. Existing `RenderResourceLimits::Supports` consumers and texture
admission diagnostics therefore see a tighter common bound, never an increased limit.

Only the Vulkan viewport depth caller migrates; ordinary `CreateTexture`, sampled
materials and UI-paint creation keep their previous admission policy. Other backends
must explicitly implement and publish the same complete-view contract before this
operation is available. No public fields, native handles, new header owners or backend
selection rules are introduced. Rebuild frontend/public-header consumers and the real
installed-host fixture consumer. Portable tests prove owner/phase rejection, descriptor
policy, budget rollback and generation lifecycle; Vulkan query and layered-storage
tests plus installed lifecycle execution still require the actual Vulkan test graph.

The XR graph retains each returned depth/view creation pair. Retirement uses normal
frontend release first and recognizes completed rollback only when that exact stored
operation is terminal failed and release reports its generation already stale. Live
prefixes still release normally; unknown journals and other release errors remain
errors. This permits a fresh XR graph generation after already-completed depth rollback
without clearing unrelated viewport resources or changing ordinary release behavior.
Other retained cold-mesh failure state still requires the explicit full-owner recovery
described above. Native depth/view failure, successful-prefix retirement and retry are
covered through the actual viewport owner; this remains fake-dispatch, not GPU proof.
