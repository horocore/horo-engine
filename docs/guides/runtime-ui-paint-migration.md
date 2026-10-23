# Runtime UI native paint migration

## Reason and ownership

The existing render snapshot retains clip/mask indexes and leaf alpha, but it
cannot express rounded mask ancestry or isolated opacity groups. The existing
frontend UI submission retains image/font/geometry leases without issuing a draw
workload. The new paint path extends those authorities rather than replacing the
canonical document, style resolver, RuntimeHost scheduler or renderer selection.

## Caller migration

Existing alpha masks and ungrouped draw projections remain source-compatible:
new parent/group members default to their absence sentinels and new group capacity
defaults to zero. Callers using groups reserve explicit bounded capacity, publish
parent-before-child nonempty intervals, and assign each command its deepest group.
A group opacity is applied once at composite; existing leaf opacity is unchanged.
Rounded/rectangle masks carry no invented texture identity. Alpha masks retain
exact asset revision and role-specific realization evidence.

Runtime UI producers continue to publish `UiRenderSnapshot`/geometry leases.
The host-owned frontend bridge translates them to renderer-only prepared workload
values. RenderApi does not depend on RuntimeUi. New public renderer headers have
one explicit target owner and public consumer coverage; native handles stay in
private backend targets. Shader realization, buffers and effect targets are
prepared at explicit lifecycle boundaries, not discovered during a widget draw.

The real host source is the committed `UiAnimationRuntimeParticipant::Acquire`
publication. Composition must register/select that source and the paint owner;
a test-only optional callback, arbitrary desktop document or dormant factory is
not a completed host integration. Presentation receipts follow successful native
presentation and the exact view/canvas/snapshot/interaction lineage.

## Lifecycle and qualification

Paint reservations are finite. Capacity, malformed topology, stale source,
unsupported target and preparation failure retain prior admitted state. Frame-hot
work does not allocate, compile, block on I/O or wait for GPU idle. Source, mask,
shader, buffer and transient target generations remain pinned until native
completion, terminal device loss or unsent abandonment.

Native execution requires separate OpenGL, Metal and Vulkan encoder integration
and pixel evidence. Null/model tests qualify admission and lifetime only. During
staged development no backend capability or ticket closure is claimed before the
complete encoder/host/presentation path has executed.

## Explicit empty startup and source admission

`UiAnimationOwner::Create` requires an issued tree in a real `UiReloadGeneration`;
its admission remains unchanged. The app registers one finite UI lifecycle adapter
before `RuntimeHost::Startup`. That adapter starts in `NoSource`, and performs an
empty `ComposeUi` contribution without manufacturing a document. A real SDK source
attaches a complete admitted generation at an explicit structural safe point. Only
then does the adapter construct the canonical owner and participant with its actual
`RuntimeDispatchSource`. Failed attachment preserves the previous state/publication.

The order is startup, explicit source attachment, VariableUpdate preparation,
RenderExtraction publication and immutable acquisition, native paint composition,
presentation receipt, structural reload, and finally detach/shutdown. Detach closes
new source/paint admission, enters `Retiring`, and drains jobs, frame leases and
native completion leases before destruction. `Failed` is explicit; it never selects
a fallback renderer or silently replaces the canonical source. The adapter remains
registered for the host lifetime; no late `AddParticipant` or ambient registration
is introduced.

## Shared service publisher

Production attachment preserves `RuntimeUiService::Admit` and its
`UiSceneReconciliation` registry as the sole authored-generation publisher. An
animation source reservation pins the shared service state and exact instance;
it does not copy the generation or take a second `UiHotReload`. Animation owns
only derived clocks, styles and frame publications. The service keeps weak bounded
reservation entries, and stores no animation-owner pointer or callback.

The trusted SDK host uses the coordinated service reload overload, synchronously
borrowing its actual animation owner and supplying its existing allocator, style
registry/resolver, complete animation definition and reload policy. Exact service,
instance, current generation and reservation identity must all match. Preparation
retains the old source, derived frame, client grant and queued action on failure.
Successful canonical commit invalidates attribution once and then swaps the fully
prepared derived owner. Native modules retain existing `RuntimeUiClient` grants;
they receive no service, allocator, publisher or animation-policy privilege.

The old service reload overload remains valid for unanimated instances. For an
attached animation source it reports that coordinated preparation is required;
callers migrate to the typed overload rather than receiving permanent busy. Actual
concurrent preparation or retirement remains busy. Scene replacement and unload
must first coordinate or retire the same derived source; they cannot publish a
replacement ahead of its derived candidate. Animation detach never shuts down or
collects the service's publisher. Service shutdown revokes access while immutable
frame and native completion leases drain without a normal-frame wait.

The source reservation retains an exact immutable generation lease even before the
first frame. Service shutdown revokes operation admission immediately, while that
pin prevents canonical entry collection beneath the attached derived owner. A
coordinated reload pins its private candidate before commit, transfers the pin only
on successful publication, and leaves the old pin on every preparation/cancellation
failure. Detach and native completion drain precede final reservation release.

### Reusable native paint slots and shared frame completion

`UiPaintFrameBuffers` names three distinct frontend-owned host-visible residents. Allocate a finite ring when a real source is
admitted, before the first paint frame. `UpdateUiPaintFrame` borrows complete immutable vertex/index/parameter views only for
its synchronous call. It validates the entire payload and resolves exact frontend generations before calling the selected
backend. Native adapters must prove resource namespace, usage, mapped capacity, advancing slot revision and completed prior
GPU use for **all three** destinations before the first write. A pending slot returns a typed pending result; no frame wait or
per-frame buffer/pipeline allocation is permitted. A native upload failure never publishes the new accepted UI revision.
The caller keeps its prior prepared frame until a complete new slot is admitted.

A paint workload names that exact accepted slot revision and uniform range. Vertex colors remain straight-alpha inputs;
ordinary image/glyph sources are premultiplied once in the cooked shader. An isolated group's completed texture is explicitly
`Premultiplied` and its opacity is applied once when composited. Target-origin flags describe render-target sampling without
changing canonical layout or hit testing.

The named UI stage follows scene execution and precedes `ComposeGui`. It extends the renderer's **same** canonical active
completion lease with bounded UI source, resident, isolated-target and cooked-pipeline ownership. It does not reopen generic
`Execute`/`ExecuteGraph` admission, replace already submitted scene pins, invent a second GUI submission authority, or release
an existing native frame lease on a rejected extension. Explicit queues use the existing frontend submission allocator and
wait on already admitted scene signals. GUI preparation then extends that same lease through its existing contract.

The reusable frame update API owns its typed outcomes in RenderApi through
`UiPaintFrameErrors`: malformed/stale inputs are `InvalidInput`, retained native
slots are `Pending`, pre-write cancellation is `Cancelled`, and an inactive or
unsupported owner is `Unavailable`. Native submission failures preserve their
backend error. These new paint callers do not depend on the separate
RenderUpload service or its lifecycle errors. No published compatibility API is
replaced; consumers of the new update contract compare its domain and code.

`UiPaintFrameBufferPool` belongs to RenderFrontend and creates exactly three
host-visible residents per explicitly admitted slot (at most eight and bounded
by the selected device). It owns capacity/revision metadata, while the frontend
remains the sole resource owner. Successful updates reuse resident identities;
pending, cancellation, malformed input and failed admission preserve the previous
revision. Pools close on their creating render thread before normal frontend
shutdown. A lazily allocated private owner marker is revoked before frontend
destruction; a surviving pool rejects update and releases only metadata, while
the frontend drains its own native/GPU resource claims. The marker contains no
callback, ambient lookup, new thread synchronization or second resource registry.
New hosts use this pool at real source admission; NoSource creates no pool.

The unpublished paint raster ABI now uses exactly 64 bytes at each existing
256-byte uniform offset: extent/opacity/sampling/flags/reserved occupy bytes0–31,
mask-local extent and reserved words bytes32–47, and clockwise corner radii
bytes48–63. Runtime validation and native reflection require that exact shape.
Rounded coverage uses the original affine quad UVs and checks each individual
corner region, including asymmetric radii beyond the rectangle midpoint. Its
local distance is antialiased using fragment derivatives; CPU tessellation does
not substitute another mask boundary. Coverage targets begin transparent, paint
the shape, and multiply complete ancestor coverage over the full target using a
separate closed `MultiplyCoverage` operation. No outside-mask white region may
survive. These source additions remain unqualified until actual native execution
and the complete prepared program path are tested.


### Immutable prepared program and ordinary frame changes

`UiPaintProgram` is owned by RenderFrontend; its public header is staged only by
that target. Its actual consumers supply canonical `UiRenderGeometryPlan` leases
and the existing image/font submission bindings, mapped to the prepared slot via
`TextureResource`. RenderApi remains Foundation-only. `Create` is explicit safe-point
work: it compiles complete potential pass/resource roles, realizes distinct primary
and linear RGBA16F kernels, and reserves two CPU candidates per finite resident slot.
`Prepare` reuses those arrays and native residents. It validates current source,
resource realization and the complete geometry before writing; cancellation or a
busy slot preserves the issued accepted publication. Ordinary geometry, text glyphs,
colors, mask transforms, visibility and opacity do not compile another graph.
Changed source generations, group/mask topology or external texture inventory require
a complete replacement. The App owner must coalesce the latest structural request,
cancel superseded preparation and publish only a complete ready replacement.

Each pass declares a finite sorted table of every potential sampled resource. Each
draw selects at most one source and one coverage entry; native admission resolves
and validates the complete table before creating an encoder. Relative mask scopes
are keyed by the actual leaf mask and an excluded **actual ancestor** at a group
boundary. A child group paints only the remaining mask chain; its completed surface
is masked once at composite into the parent. Ancestor fractional alpha is therefore
not multiplied again inside each child. Independent sibling surfaces retain their
canonical interval order. Binary rectangles additionally use balanced native stencil
push/pop; fractional rounded and texture coverage is applied once through the
coverage texture. CPU tracking regressions qualify these issued graph/workload
relationships only; native image comparison is still separately required.


The prepared sampler key preserves independent minification, magnification and
mipmap selection (twelve closed combinations prepared once). Current source
realizations have one mip level; native nearest/linear mip selection retains its
well-defined single-level behavior. It is not a reason to reject valid source
metadata. Multi-level image realization remains governed by the existing resource
admission boundary. Coverage surfaces exactly match the output extent and use an
explicit corresponding-texel load, independent of a source image's sampler. The
portable shader source revision advances to 3 for this sampling change; the
parameter ABI remains exactly 64 bytes and the two texture bindings are unchanged.


An accepted upload is bound to the exact retained geometry arena-slot issuance,
not merely its view and snapshot revision. Copies of that same immutable plan
remain valid; an independent plan from the same snapshot does not authorize the
accepted draw buffers. Each reserved candidate also copies bounded immutable
image/font realization evidence before upload. Submit requires the same graph
texture/native generation and source sharing evidence, and reruns the canonical
submission-retention validator before entering ComposeUi. For used fonts the
sealed atlas must own the accepted realization's actual PageLease and the same
frame/revision; equal numeric IDs from a foreign atlas cannot substitute ownership.
No new global registry, caller nonce or mutable source authority is introduced.
An atlas supplied with zero font bindings contributes no encoded texture or glyph
identity. Its ordinary canonical capture/lifetime rules still apply; no new atlas
observation API or ownership authority is needed for that unused input.


## Application source producer and retirement port

The non-installed `HoroRuntimeUiPaintHost` application target registers one fixed
participant before the editor consumer. Its `RuntimeUiPaintPort` pins revocable
service storage, never a host-wrapper pointer. Admission, reload and collection
run on that same owner thread. Shutdown revokes new calls and clears the borrowed
frontend pointer. The editor presentation composition retains its port while the
frontend polls native completion, then explicitly collects the stopped derived
owner before releasing the port and native renderer parents. A surviving external
port must be released on the owner thread after successful collection; shared
storage alone does not prove native retirement.

The source declaration carries only actual text/image/resource component inputs,
mask membership and potential isolation scopes keyed by admitted element IDs.
Structural element order, layout, visibility and fills come from the actual
catalog projections and the animation owner's computed style/layout/clipping
publication. `ProjectUiThemeSurface` projects that publication without another
cascade or theme runtime. Reload reserves the replacement declaration and arena
before the existing coordinated source/derived commit; failure keeps the original
source, client grants and prepared program. Structural program preparation occurs
at the next explicit owner safe point, after an actual new extraction exists.

`UiAnimationRuntimeParticipant::CanReclaim` is an additive owner-thread observation
of its existing stopped-owner fence. Callers first stop the participant, release
CPU observations, drain existing retirement work, and only destroy it after this
observation succeeds. It performs no hidden collection or reclamation.

The owning application and actual SDK-host regressions exercise this producer;
CPU program tests alone do not qualify its native pixel output. OpenGL/Vulkan
can explicitly deny this optional capability while ordinary GUI preview remains
available. Selected-Metal native effects and lifecycle still require native proof.


## Output evidence handoff

`RuntimeUiPaintPort::UpdateOutput` accepts committed physical output, independent
per-window display-scale evidence, and physical safe-area insets. The editor
samples its existing surface owner at the pacing boundary. SDL safe bounds are
converted from the same logical window to that committed physical extent; this
conversion is not used as a substitute for SDL's independent display scale.
NoSource does not query or require optional native paint resources.

The port validates the actual admitted canvas policy and coalesces one latest
observation. A change immediately fences old extraction/input. At the next owner
command cutoff, the existing canvas resolver supplies logical content/font scale
and the existing animation participant receives the new viewport revision. An
actual extraction then feeds bounded program refresh at the following owner safe
point. No new layout owner, per-frame graph compilation or GPU wait is introduced.
Only the exact new encoded frame's successful native Present receipt restores
input. Invalid evidence preserves the prior policy; a superseded receipt cannot
restore eligibility. Owning application regressions exercise resize, independent
DPI and inset changes, malformed evidence, coalescing and the old-receipt fence.

OpenGL/Vulkan may explicitly deny the optional UiPaint capability while retaining
ordinary editor GUI/preview startup. This is a declared fail-closed support matrix,
not another renderer fallback, ignored effect or native pixel qualification. The
selected Metal path still requires actual effect/readback and lifecycle evidence.

## Private native qualification inputs

The existing `HoroMetalGraphExecutionSmoke` target consumes a private offline
fixture; it does not create or qualify a production signed host catalog. The
producer uses the checkout's canonical `ui_paint.hlsl` and locked Linux tools to
validate SPIR-V and generate MSL. Its finite data-only manifest binds the same
workflow run/revision, exact source and lock bytes, effective options, entry/stage
identities, binary/archive digests, generated byte hashes and the actual reflected
64-byte uniform layout. Includes currently fail closed because this shader is
self-contained; adding includes requires binding their complete byte closure.

The Apple consumer checks the entire handoff before invoking installed offline
`metal`/`metallib`. It records those actual executable hashes and SDK evidence.
The fixture packages the resulting libraries with `PackageShaderArtifactStages`,
checks their hashes, and uses unchanged `UiPaintNativePayload` and native pipeline
reflection. Its pixel assertions read completed native texture copies, including
rounded edges, transformed masks, fractional isolated/nested opacity, intersecting
coverage, sibling independence and publication replacement. These tests do not
substitute a CPU image or claim an application Present from a readback completion.

Three scalar reserved fields retain offsets 20/24/28 under the production route's
strict OpenGL layout option. A `uint3` there would require 16-byte alignment under
that option and conflict with the fixed C++ ABI. Qualification requires actual
compiler member offsets; neither expected metadata nor a successful package parse
can stand in for that evidence. Missing tools or devices leave native qualification
incomplete. A private fixture never permits an unsigned production host catalog.
