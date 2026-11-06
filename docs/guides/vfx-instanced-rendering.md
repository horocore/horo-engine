# VFX indexed instanced drawing

Delivery identity: HORO-1735 / #1778 [VFX-004.3].

This document records the implementation and migration boundary for native
billboard and mesh drawing. Metal and desktop OpenGL have native functional
evidence below; software Vulkan functional qualification has also passed. Packing tests, shader source,
or headless graph tests do not qualify native execution.

The source was reconciled onto merged main `6b7830dd`, which includes the
OpenXR application-host (#3544) and runtime-UI deliveries. Their canonical
allocation-free material parameter packing overload is reused unchanged. A later
OpenXR follow-up remains a coordinated stack dependency for shared native
lifetime hunks; its unpublished source is not imported here.

## Ownership and preparation

`VfxWorld` remains the sole simulation and extraction owner. The renderer consumes
its immutable `RenderWorldSnapshot` and existing `VfxDrawOrder`; it never borrows
mutable particle streams, runs simulation, or changes alpha ordering. Existing
`MaterialBindingTable`, resident material leases, mesh handles, resource registry,
and graph completion authority remain canonical. There is no VFX shader registry,
backend discovery in VFX code, or independent retirement authority.

`HoroRenderApi` owns `VfxDrawData.h`, `VfxShaderParameters.h` and
`VfxFrameUpload.h`. Each is assigned to that target in
`cmake/HoroPublicHeaderOwnership.cmake`, and its generated public-header consumer
and explicit VFX consumer exercise the installed boundary. No repository-wide
include path is required.

`VfxDrawView` captures the actual view projection and orthonormal camera right/up
vectors in the same render origin as the snapshot. The host supplies the selected
clip-depth convention through its projection. A vertical camera does not trigger
an invented world-up fallback.

The installed particle SoA contains `sizeX`, `sizeY` and scalar `rotation`;
extraction preserves these as `Vec2 size` and one angle. There is no installed
particle size-Z, quaternion, pivot or emitter transform being discarded. A
billboard uses a centered unit quad, full width/height and rotation about the view
normal. A mesh preserves its authored vertex origin and Z coordinates; the
available scale components affect local X/Y and rotation is about local Z. This
makes the first native interpretation explicit; full three-dimensional particle
orientation would require a deliberate descriptor/SoA/extraction migration.

## Reflected instance data

`PackVfxDrawInstance` produces canonical mathematical clip transforms and linear
RGBA values. Packed simulation color is RGBA8, most significant byte first. It is
already linear: packing does not apply sRGB decoding, premultiplication, exposure
or output encoding.

The 80-byte logical record is not a native uniform ABI. The canonical material
packer consumes array-major **row-major logical components** and applies the final
target's reflected offsets, array/matrix strides and matrix major order. Callers
must convert `Math::Mat4`'s column-major storage into that logical input order.
`PackVfxShaderParameters` performs that conversion in fixed local staging and
calls the existing destination-span material writer; it does not add another ABI
packer. Unused instance lanes are zero and the required prefix is failure-atomic.
`vfx_instanced.hlsl` deliberately pairs its explicit `row_major` declaration with
the vector-first HLSL `mul` expression. Locked DXC/Cross diagnostics proved that
this pair lowers to column-stored matrices and matrix-vector evaluation on both
MSL and GLSL. The previous `column_major` declaration produced a different MSL
operation; accepting it with canonical column-major packing transposed a
non-symmetric off-axis transform. The selected logical-to-native map therefore
remains canonical column-major packing, without a backend-specific CPU transpose.
The native off-axis fixtures below exercise this mapping; mathematical translation
evidence alone does not qualify pixels or device execution.
The shared HLSL qualifier also changes Vulkan and DXIL lowering. Neither route
is qualified by the GL/MSL diagnostic: Vulkan needs its own layout, native draw
and lifetime evidence. D3D12 graphics remains unsupported.

`MakeVfxShaderManifest` in the existing `HoroRenderApi`-owned
`VfxShaderParameters.h` is the canonical logical cook interface: Instances,
Material, BaseColor and BaseColorSampler retain stable IDs, as do the four
parameters ClipFromLocal, InstanceColor, Tint and AlphaCutoff. Cook callers and
fixtures migrate from handwritten manifests to this factory. The existing
RenderApi generated public-header consumer and `VfxDrawPublicHeaderConsumer`
cover the addition; no new header owner or usage dependency is introduced.
OpenGL and Metal use cook-contract revision 3; Vulkan remains revision 2 pending
its native qualification. Recook preliminary revision-1 artifacts for the corrected
matrix lowering, and revision-2 GL/Metal artifacts for corrected final-target
evidence. GL now uses deterministic cross-stage varying names. Metal records
Material as vertex-only in the ordinary permutation and vertex/fragment in the
masked permutation, following actual final evidence. The HLSL bytes and logical
locations are unchanged. The existing compilation key includes the manifest
revision, preventing reuse of old GL code or Metal metadata; the cooked key also
includes the actual reflected visibility. Old records are recooked, not repaired
or accepted through a native optional-binding fallback.

This deliberately corrects the public reflection contract: an active binding's
`stages` is observed visibility within the manifest's declared maximum. The
canonical normalizer enforces nonempty known-bit subsets. Globally inactive
records retain declaration visibility with `active=false` for schema continuity.
The manifest and its compatibility hash remain the logical interface authority;
no public field, binary layout, serialization schema or ownership registry changes.
Cook normalizers, metadata encode/decode, frontend material preparation, and native
Metal/Vulkan material consumers use the observed mask. Existing producers whose
maximum is observed keep identical behavior. Custom producers must supply actual
visibility and rebuild against the documented header; RenderApi and ShaderBuild
public-header consumers cover the boundary. Native exact-argument validation is
unchanged, and the owning reflection regression target covers subset admission
and foreign/unknown/zero-stage rejection.

`ShaderIntermediateEnvironment::Vulkan12SpirV15` is an explicit appended RenderApi
enum value for OpenGL/Glsl410. Its descriptor value participates in artifact
identity and selects matching DXC, validator and debug-output environments. VFX
GL callers must request it explicitly. Existing UI, Vulkan and Metal requests
retain `Vulkan13SpirV16`; no descriptor is silently reinterpreted. The legacy
environment rejects other backend/payload combinations. Custom cook adapters and
SDK consumers must rebuild and explicitly support the new value before using it;
the `ShaderBuild` and RenderApi public-header consumers cover this extension.

The Metal cooker selects this exact interface identity before capturing the same
locked translator operation's SPIR-V reflection and generated MSL. A sibling VFX
normalizer validates complete native uniform structures, fixed arrays, stage
interfaces and actual resource argument attributes. It never supplies assumed
default native slots. The UI normalizer keeps its existing contract and shares
only its pure bounded source tokenizer. Missing optimized fragment material
arguments do not renumber logical IDs. Unproved matrix-major lowering or native
packing rejects the cook; scripted parser tests are not native compiler evidence.
VFX alone uses the documented MSL decoration-binding option for its closed
flat-set-zero, separate image/sampler interface, preserving the same material
buffer index across stages. UI and unrelated routes keep their existing options.
The MSL normalizer validates the complete generated matrix-vector helper and its
actual position call, rejecting changed semantics or competing overloads.

The same-operation OpenGL handoff validates its own generated GLSL410 std140
blocks and the unique combined sampler, retaining the actual link names. It
assigns finite Horo uniform/texture slots that the existing candidate-program
realizer installs explicitly; these are not assumed shader-default bindings.
The existing native gate queries each linked member's type, count, offset,
array/matrix stride and major order, and actual UBO/texture-unit limits before
publication. No Vulkan or Metal target map substitutes for that validation.
The VFX GL translation alone disables nonzero-base-instance support: its existing
native consumer uses only `glDrawElements`/`glDrawElementsInstanced`, with no base
instance field or API. This removes an otherwise unaccounted fallback uniform.
Supporting a future nonzero-base draw requires a deliberate profile/contract
change, not silently continuing this assumption. Actual final-stage I/O names
are read from GLSL declarations; SPIR-V names cannot replace GL fragment-output
location lookup names.
GLSL410 monolithic linking also requires matching names at the two VFX varying
locations. The VFX-only translator uses the locked tool's documented
`--rename-interface-variable` option to name vertex outputs and fragment inputs
`horo_vfx_uv` and `horo_vfx_color` at locations 0 and 1. The normalizer reads these
names from actual GLSL and requires cross-stage agreement; the separate Cross
JSON may retain intermediate names, which are reconciled by location and type.

Native GL attribute enumeration may also return `gl_InstanceID`; the actual Apple
GL4.1 driver reports it as a scalar integer with numeric location 1. It is a native
instance builtin, never the MeshVertex normal stream at that location. The private
preparer admits at most one exact scalar-int instance builtin for the canonical
typed VFX schema, then checks all user inputs separately against their exact cooked
names, shapes, locations and count. Unknown/malformed/duplicate builtins and missing
or aliased user inputs fail. A driver that omits the builtin from enumeration does
not change the user-input contract. This does not alter shader bytes, logical
locations, the cook revision, or any vertex-array stream binding.

Production adapter qualification on a macOS cook host still requires its admitted
executable and signed host-catalog route. The built-in Linux tool catalog does not
authorize executing a macOS cook tool. Native smoke may consume the exact captured
cross-host GLSL or compile captured MSL through the existing native fixture
authority; that qualifies device consumption, separately from the production
adapter. No self-signed catalog, authored reflection or handwritten replacement
native shader is an acceptable substitute.

`tests/fixtures/shaders/vfx_locked` now retains the authentic final combined
Cross JSON, MSL and GLSL text for ordinary and masked stages. Its compact
provenance records source/tool hashes and the two explicit intermediate profiles;
the shared test loader verifies the captured bytes before normalization. CPU
cases also package the actual GLSL through the canonical reflection cooker.
These text regressions do not run the production compiler adapter, create a Metal
library, or supply GPU evidence. The skipped Linux adapter integration remains
separate. The existing native Metal preparation target compiles the captured MSL
through its established `xcrun` fixture dependencies; it does not invent a signed
production tool catalog.

Metal's narrow instance-array admission requires the canonical VFX interface,
actual reflected `float4x4[128]` and `float4[128]` element types, exact native
offsets/array strides, column-matrix packing and a 10240-byte block. Ordinary
material arrays/matrices remain unsupported. The native preparation regression
target exercises malformed layout and block extent admission. Immutable Material constants retain
their logical 20-byte member extent: the existing frontend queries native block
size and pads the allocation before publication, including Metal's native struct
extent. Callers must not substitute a 20-byte native buffer for that query.

The GL oracle extends the existing SDL material smoke composition with captured
cooked shaders and the complete frontend graph path. One off-axis scene includes
two distinct billboard instances, a mesh with authored nonzero Z, masked discard,
foreground alpha, depth-occluded alpha and additive color. Pixel-center ray/plane
depth is computed independently from the VFX packer. Actual GPU completion and
ordinary frame admission precede publication reuse; test-only readback/teardown
barriers do not change the production frame path. The opt-in desktop OpenGL target has now run with the result recorded below.
The authored-Z mesh selects a deliberate quadrant of a nonuniform 2x2 texture;
normal-field reads, swapped UV components and a flipped V coordinate produce
different expected texels. Its UV remains the proven canonical MeshVertex input
at location 2. The host preserves an original assertion exception while explicitly
completing and draining the frontend before destroying its still-live context.

Canonical VFX scheduling requires scene-linear floating-point color. The GL
resource owner now admits `Rgba16Float` sampled/render attachments using
`GL_RGBA16F`/`GL_HALF_FLOAT`, the same typed material pipeline and cold graph FBO.
This supplies the existing scheduler contract; it does not add tone mapping,
exposure, a look policy, or a fallback to normalized color. `Rgba32Float` remains
unsupported on this GL path. Storage mapping/attachment regression sources and
the native VFX color oracle exercise this addition, as recorded below.

Each uniform draw is bounded by 128 instances. That bound is an admission ceiling,
not a claim that a device supports the reflected block: actual native block size,
layout, ranges, alignment and selected limits must be checked before activation.
Prepared frame slots cannot be overwritten while CPU readers or native commands
retain their previous generation. A retained slot reports typed backpressure;
normal frames never wait for GPU completion or allocate replacement backing.

`PrepareVfxFrameSlot` issues a proof from one already resident canonical material.
Its instance binding must be a dedicated host-visible uniform generation at
offset zero, cover exactly the actual native block size, and appear in no other
binding of that material. The frontend verifies the ready pipeline, reflection,
owner incarnation and buffer generation. It allocates only fixed CPU staging at
this cold preparation point. The slot is not an alternate resource registry and
does not create native backing. Host configuration and the existing retained
material/resource budgets bound the pool; 128 is the shader's instance limit,
not an imposed draw-count limit.

The existing frontend buffer record owns the CPU slot claim for its exact
generation. Repeated preparation for the same retained material returns the same
live shared control block, so it cannot bypass an unsubmitted graph borrower.
Another material on that buffer is rejected. An expired claim remains consumed:
the host must prepare a fresh buffer generation instead of restarting native
revision numbering. Only canonical buffer-record replacement resets the claim.
Same-buffer recursive preparation is blocked before entering an adapter query;
query reentry that resizes, releases resources or starts shutdown is revalidated
before any proof is issued.

Updates accept typed instance records and a newer revision, never arbitrary
caller bytes or a caller-selected range stride. All matrix and color components
must be finite before the canonical writer changes staging. The native adapter
then revalidates the exact material/pipeline/range and all actual prior uniform
uses. CPU admission is deliberately conservative: the updating host must hold the
unique shared slot handle. Any extra facade copy, including a graph revision
borrower, returns pending even if no GPU work exists. Hosts retain one pool handle,
release all additional CPU copies before updating, and create no new borrowers
during publication. This count is never GPU completion evidence; native command
completion independently protects other material aliases. A native write failure
invalidates publication rather than reusing possibly changed bytes. Cancellation
or pending admission preserves the previous accepted publication.

An owner-thread mutation guard rejects nested updates before staging or native
writes. The outer update temporarily retains the slot independently of the
caller's handle, so a callback cannot free borrowed bytes by resetting that
handle. After the adapter returns, owner revocation and exact buffer/pipeline
readiness are checked again before publication; shutdown cannot recreate an
accepted revision. Required exception containment remains explicit without
adding analysis-rule suppressions.

The GL adapter does not equate a numeric native name with a generation. The
retained canonical material owns existing submission pins for its exact buffer
and pipeline; frontend release removes discovery but cannot delete either native
object while those pins remain. A callback can create other generations, but
cannot recycle the pinned object's native name through this ownership path.
The issued slot, native material and uniform metadata also carry the existing
selected-device incarnation. Fresh admission follows presentation, reflection
query, completion polling and state callbacks before upload. Shutdown closes
admission immediately; the backend retains its context attachment during drain
and rejects initialization of that same attached owner. Directly destroying an
object through a borrowed backend while its frontend owns it violates the
existing resource ownership contract and is not an alternate supported lifecycle.
The fault-dispatch release/recreate and shutdown regressions exercise this
boundary; they are unit evidence, not GPU qualification.

Shutdown requested inside a synchronous native borrow closes admission at once.
It defers the existing abort/closing-fence step until that borrow returns; polling
while borrowed reports pending. Otherwise a fence inserted by a callback could
precede commands still encoded after that callback. The ordinary completion and
shutdown fences remain the only GPU retirement authority.

## Preparing a graph revision

Hosts reserve native draw capacity through `VfxSchedulingBudget::maximumDraws`
before activation. This bounds records and scratch for the admitted retained
views; the same budget charges VFX captures on the existing eight completion
leases. It creates no new resident or completion registry. Zero keeps the existing
plan-only scheduling contract. A contiguous material run splits at the shader's
128-instance block, and every resulting chunk consumes the scene draw allowance.

After `ScheduleVfx`, the host supplies exact vertex/index graph imports, canonical
mesh and material slots, a prepared floating-color/depth target, and captured
scene/frame/origin/view identity to `PrepareVfxDraws`, before opening a frame.
Billboards use authored centered unit quads; mesh batches must name their exact
extracted mesh. The complete binding set is checked for finite transforms and
ready resource identities before the first upload. Records use preallocated
storage, and callbacks cannot replace borrowed binding spans. Only a successful
complete transaction seals the order's native revisions. A partial upload or a
callback replacing the recipe cannot publish a partial graph program.

The baseline admits one explicit sampled color texture and immutable canonical
material constants in addition to the dedicated instance buffer. Additional
mutable uniform bindings and undeclared texture aliases are rejected. The graph
declares exact vertex, index, uniform, sampled, color and depth roles. Frontend
resolution and native validation compare those identities to the actual VAO,
material, framebuffer attachments and accepted publication count/revision.

The existing graph completion lease pins targets and meshes and retains the
immutable snapshot/order/slot program until proven completion, including partial
encoding and abort. The scheduling pool itself always owns its reusable order
storage, so weak-pointer expiry is not completion evidence. The canonical owner
may clear old draw records only when that slot has no external order aliases;
ordinary facade copies still prevent CPU publication. Native uniform frame-use
tracking independently checks every actual alias through the existing fences.

`VfxForwardInstancing` is a new narrow capability bit. The GL source route uses
the canonical material pipeline and indexed instanced consumer, and checks
opaque/masked depth-writing, translucent alpha/read-only-depth and additive
policies. Shadow, depth-prepass and GBuffer VFX encoders remain unsupported in
this phase. Generic GL depth-clear graph support now has source coverage for
whole depth/depth-stencil attachments, preserving stencil, the depth write mask
and the caller's clear value. CPU dispatch and an existing native smoke fixture
have depth/abort/readback assertions with native results below.
Metal and Vulkan now have source routes
through their canonical prepared material/mesh/target owners, exact native
publication checks and indexed instance counts. Metal uses the existing native
command-use retirement records; Vulkan retains the existing graph submission
lease and resource-state/timeline authority. Both publish the capability only
after their actual native resource/command prerequisites are initialized; the
bit does not itself prove platform qualification. D3D12 remains an explicit graphics
foundation dependency. None of these source changes constitutes platform
qualification.

Slots store an inert weak frontend-incarnation marker and no frontend pointer.
Shutdown revokes that marker before teardown. The host must release slots and
graph borrowers before destroying the frontend: their canonical material leases
still obey the existing registry/native retirement contract. Staging destruction
does not allocate; it does not create another retirement or fence authority.
Shutdown drops the frontend's scheduling inventory after aborting an active frame.
It does not clear a borrowed order: an accepted graph retains its slot until actual
command completion, then releases the slot's material backing while the device is
still alive. Host-owned material and scheduling aliases must still be released in
the order required by `MaterialBindingLease`; shutdown cannot revoke those leases.

The new `IRenderBackend::UpdateVfxFrame` virtual is an intentional source/ABI
change for custom backend providers. Existing source implementations inherit a
typed unsupported default, but every provider/module and consumer must rebuild
against the updated public header. This change does not promise compatibility
with an old compiled backend vtable or qualify its VFX support. The public-header
consumer covers the declarations; native-capable providers need their own
publication and draw regressions before reporting support.

## Depth, color and lifetime evidence

Opaque/masked draws obey the existing depth-writing geometry policy. Translucent
draws preserve canonical back-to-front instance order and use read-only completed
depth; additive follows translucent and does not introduce distance sorting.
Masked depth/color/shadow coverage must agree. Material and pipeline state remain
responsible for blend/depth policy; shader output stays unexposed scene-linear.

Qualification must render billboard and mesh geometry with actual native shaders,
verify pixel/color and depth interaction, exercise failure/abort and retained-slot
reuse, and measure the declared reference scene on its stated backend/device/build.
Neither fixed packing layout tests nor unsupported-return tests replace these
results. A performance report must name instance/mesh complexity, resolution,
blend/depth workload, CPU/GPU timing availability, and the measured budget.

## Manual Metal measurement

The long measurement is deliberately separate from ordinary tests. The
`HORO_BUILD_METAL_VFX_BENCHMARK` option defaults to `OFF`. When enabled,
`HoroMetalVfxInstancedBenchmark` is still excluded from the default build and has
no CTest/discovery registration. Existing GPU smoke selectors do not run it.
Build only this target in a separate Release tree; a single-config Debug tree
cannot become Release by passing `--config Release`.

```sh
cmake -S . -B build/vfx-metal-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DHORO_BUILD_METAL_VFX_BENCHMARK=ON \
  -DHORO_BUILD_EDITOR_GUI=ON -DHORO_EDITOR_LIVE_UI=OFF \
  -DHORO_BUILD_RENDER_METAL=ON -DHORO_BUILD_RENDER_OPENGL=OFF \
  -DHORO_BUILD_RENDER_VULKAN=OFF -DHORO_BUILD_XR_OPENXR=OFF \
  -DHORO_ENABLE_GPU_SMOKE_TESTS=OFF
cmake --build build/vfx-metal-release --target HoroMetalVfxInstancedBenchmark --parallel 1
```

The GUI configuration enables the existing SDL/Metal presentation composition;
this target does not build or run the Editor application. Supply an absolute
path to the authentic cooked ordinary/masked Metal package, a count of exactly
`1024`, `4096` or `16384`, and a new absolute JSONL destination whose parent
already exists. The receipt refuses overwrite.

```sh
HORO_VFX_INSTANCED_ASSET_ROOT=/absolute/path/to/cooked-metal-package \
HORO_VFX_BENCHMARK_COUNT=1024 \
HORO_VFX_BENCHMARK_RECEIPT=/absolute/path/to/new-iteration.jsonl \
MTL_DEBUG_LAYER=0 \
  build/vfx-metal-release/tests/HoroMetalVfxInstancedBenchmark
```

Each invocation measures one count in one process. For a measurement set, run
seven independent processes per count, sequentially, retaining every receipt and
failure. Each iteration prepares a static 1920x1080 RGBA16F/Depth32F scene with
five deterministic blend/depth families: translucent/additive billboards and
three opaque/masked indexed-mesh families, one with authored mesh-local Z. Each
geometry uses four vertices and six indices. Canonical 128-instance chunks
produce 10, 35 or 130 indexed draws. Resources and graph preparation occur before
the measured window. The explicit Fifo cohort uses serial native-completion
observation; it introduces no hidden pacing sleeps to satisfy the sample count.

There are 300 completed warmup frames, then at least 1,200 measured frames and
20 measured seconds. Both minimums must hold within 10,000 samples and the
300-second iteration deadline; a limit/error preserves partial evidence and
fails the invocation. Raw observations are buffered within a fixed bound and
written after the window. The receipt reports CPU encode and Present-call time
separately from the entire native command-buffer GPU interval, retaining
unavailable/invalid GPU timestamps explicitly. It does not measure whole-frame
CPU time, simulation/update throughput or product FPS. Catch instrumentation,
serial completion polling, presentation policy and requested validation state
remain part of this diagnostic cohort.

This is an opt-in measurement candidate, not an automatic performance gate or
approved baseline. Numeric targets remain unset until the measured reference
workload/cohort is reviewed. Run it when renderer/VFX performance changes or at
milestone qualification, rather than on every build or PR. Keep the existing
functional pixel/depth/lifetime checks separate from timing evidence.

## Current availability and remaining acceptance

Implementation proceeds against available OpenGL, Metal and Vulkan graphics
paths. Every native route requires its own actual qualification. D3D12 currently
has resource qualification but lacks the shared graphics/material foundation;
that dependency remains pending and is not implemented by this track. WARP
resource success and a typed unsupported result are not D3D12 VFX drawing proof.
The complete ticket remains pending while a required available graphics backend
or native acceptance criterion lacks evidence.

Existing plan-only `ScheduleVfx` consumers remain source-compatible. Native
execution must reject absent, stale, foreign or unready preparation explicitly;
it cannot turn an unencoded graph into successful rendering. No backend fallback
or reduced assertion policy is part of this migration.

## Registered graph texture uploads

`RenderGraphTextureUpload` adds one closed Vulkan upload operation to the existing
render graph. The host creates a registered `HostVisible | CopySource` buffer with
the actual pixels and an ordinary registered `CopyDestination` texture, then submits
the upload through the normal graph and frontend submission owner.

The workload contains the graph-local source/destination IDs, exact extent and
byte count. It accepts only a complete tightly packed RGBA8 2D base level: one mip,
one layer, one sample, depth one, and exactly `width * height * 4` bytes. The source
buffer must have that exact size. Offsets, row padding, partial updates and other
formats are rejected rather than reinterpreted or clamped.

The pass declares exactly two uses: source `Read/CopySource` and destination
`Write/CopyDestination`. Compiled synchronization supplies the image transitions;
subsequent graph uses supply the sampling layout. Existing resource resolution and
submission leases retain both registered resources. Destination contents are
published only by actual accepted submission; an unsent abort discards candidate
state. This adds no private staging allocation, queue, timeline or normal-frame wait.

Vulkan records the operation with its existing `vkCmdCopyBufferToImage2` entry
point. OpenGL, Metal and Null reject this new workload explicitly. Vulkan's
`CreateTexture(initialData)` remains unsupported; callers use the declared graph
upload instead of inventing ready state or issuing an untracked native write.

The workload alternative is appended to `RenderGraphWorkload`, preserving existing
alternative indices. Providers and consumers must rebuild for the changed C++
variant type; preserved indices are not a binary compatibility promise.

### Depth-copy descriptor admission

The frontend also admits exactly a single-sample, single-mip/layer 2D Depth32Float
attachment with `RenderAttachment | CopySource`, only when the selected backend's
valid support snapshot reports those roles. Initial bytes, extra roles and other
shapes remain unsupported by this narrow extension.

Metal reports the copy-source role after its bounded native private render-target
allocation probe succeeds, and probes sampled attachment support separately.
Vulkan derives the role from actual native format properties. Existing allocation,
resource retirement and submission ownership remain authoritative.

The native VFX tests use terminal diagnostic copies to inspect actual color/depth.
Those test observations do not introduce a general public texture-to-buffer graph
workload or a normal-frame readback policy.

## Native functional evidence (2026-11-06)

The Debug macOS build has GUI/live UI and opt-in GPU smoke enabled. Physical
Apple M3 Metal ran four instanced cases with 1,875 assertions, zero failures and
zero skips, with the Metal validation layer enabled. The cases exercise off-axis
billboards, authored-Z mesh/UV canaries, floating-point blend/depth, masked discard,
actual in-flight retention, and unsent abort/retry. Desktop OpenGL ran its named
billboard/authored-Z mesh case with 669 assertions, zero failures and zero skips.
These are selected native functional cases, not full-platform qualification.

The ordinary and masked Metal libraries and Vulkan SPIR-V packages were produced
by the admitted host tools through the authenticated cook route. The offline
`HoroVfxInstancedShaderCook` tool publishes decoded, validated packages atomically;
it does not compile shaders in the runtime or supply handwritten reflection.

Authentic Linux ARM64 GCC 15 compilation succeeded for the owning Vulkan, graph,
frontend, reflection and native smoke targets. The latest Vulkan unit run had 137
cases, zero failures and three explicit existing unsupported shader/stereo skips.
The actual software Vulkan native run passed both cases with 1,122 assertions,
zero failures, zero skips and no Vulkan validation errors. This covers the main
pixel/depth scene and abandoned-first/committed-retry execution. The fresh frontend
suite also passed all 311 cases without skips. Graph (84) and reflection (108)
results are carried from byte-identical executables, not rerun results.
This is software Vulkan evidence, not physical Vulkan hardware qualification.

The earlier unenabled shader demotion, duplicate graph texture identities,
frontend-owned shutdown inventory and stale per-pass material alias cache were
corrected; raw failing runs remain separate from current evidence. Material alias
scratch is reset for every pass and abandoned attempt; accepted resource states
still publish only through successful submission.

Shader demotion is optional, queried from the actual Vulkan 1.3 device and enabled
only when supported. A module declaring the demotion capability is rejected before
driver creation if the feature is unavailable. Ordinary non-demoting shaders and
baseline adapter admission do not acquire a new mandatory feature requirement.

The Metal diagnostic used seven particles, five indexed draws, two instanced
batches and a 64x32 floating-point color/depth target. One warm-up and eight frames
with validation enabled are diagnostic timing only. No reviewed reference scene,
performance cohort or numeric throughput budget has been supplied. This does not
satisfy ADR-051 measurement windows or the ticket's throughput acceptance.
The ticket remains In Progress until the remaining acceptance is resolved.

## Manual Metal measurement baseline (2026-11-06)

The isolated Release functional executable also passed the original four native
cases with 1,843 assertions, zero failures and zero skips, with Metal validation
enabled. This is fresh functional evidence, separate from timing windows.

The opt-in Release benchmark completed seven independent processes at each count
on an Apple M3 with a 10-core GPU, at 1920x1080. Each process completed 300 warm-up
frames followed by at least 1,200 measured frames and 20 seconds. All 21 processes
completed, producing 50,337 measured frames with valid GPU timestamps. The table
reports the median of the seven per-process nearest-rank p99 values; samples from
different processes are not pooled or filtered.

| Instances | Indexed draws | CPU encode p99 (ms) | GPU command-buffer p99 (ms) |
| ---: | ---: | ---: | ---: |
| 1,024 | 10 | 0.547 | 2.247 |
| 4,096 | 35 | 0.852 | 2.284 |
| 16,384 | 130 | 2.099 | 2.467 |

At 16,384 instances, the seven CPU p99 values ranged from 2.007 to 2.249 ms;
GPU p99 values ranged from 2.397 to 3.555 ms. CPU encode excludes drawable
acquisition and completion waiting. GPU time covers the entire native command
buffer, rather than an isolated VFX pass. The prepared static graph is replayed
with serial completion; these windows exclude per-frame simulation, extraction,
camera updates, readback and resource creation. They therefore do not establish
whole-engine frame time or the complete ADR-051 performance cohort.

The performance target is comparable behavior against mainstream engines such as
Unity and Unreal under matched workloads. No numeric acceptance threshold is
derived from Horo's own results. A comparison must match hardware, resolution,
particle simulation mode, geometry, material behavior, screen coverage, depth and
blend work, and measurement boundaries. Simulation and rendering costs must be
reported separately. Equivalent reference-engine runs have not yet been recorded;
performance parity and HORO-1735 throughput acceptance remain open.

The benchmark stays manual, disabled by default and outside normal build and
CTest execution. These measurements are a baseline for comparison, not a CI gate.

### Published engine workloads informing the next measurements

The [Procedural Pixels Unity case study](https://www.proceduralpixels.com/blog/rendering-90000-particles-in-unity)
reports 90,000 moving cube particles in Unity 6000.3.11f1/URP, using a Windows
IL2CPP Master build with DX12 at 1600x900 on an i5-10400F and RTX 3060. Its
procedural rendering route reports a 3.40 ms CPU frame median and approximately
3.02 ms GPU frame time. Those are the author's observations, not independently
reproduced Horo reference results. They include simulation, uploads and a rendered
URP frame; Horo's static five-family quad workload and p99 values have different
boundaries. Use the published 5,000/90,000 count range to inform scalability
coverage, rather than copying its milliseconds into an Apple M3 acceptance gate.

[GPU Instancer's published asteroid test](https://gurbu.com/performance) uses
10,000 through 500,000 instances at 1080p, with three prefabs, two LODs and much
heavier geometry than the Horo quads. It measures a commercial Unity extension
as well as Unity's ordinary instancing, so its extension results do not establish
baseline Unity performance. Its methodology reinforces recording actual geometry,
LOD, visibility, draw count and residual scene work alongside instance count.

[Puget's Unreal Niagara scalability workload](https://www.pugetsystems.com/pugetbench/creators/unreal-engine/)
maintains approximately 750,000 GPU-simulated particles, with 148,000 spawns per
second and a five-second lifetime, measured over a 30-second 1080p editor window.
It is a useful separate GPU simulation stress model. Neither its frame-rate
metric nor a fluid simulation is interchangeable with Horo's CPU-world static
instancing microbenchmark.

Published results guide workload selection without requiring a reference engine
installation here. Comparable performance remains a target; an online number on
different hardware cannot establish an engine speed ratio. Keep static rendering
scalability, per-frame simulation/extraction/upload and whole-engine frame timing
as distinct measurements, with every omitted stage stated in the result.

### Separate opaque cube scalability profile

The manual executable also accepts the explicitly selected
`opaque-cube-scalability-v1` profile, with exactly 5,000 or 90,000 instances at
1600x900. It uses one opaque emitter and actual indexed cubes: 24 vertices and
36 indices per mesh, with uniform world size 0.04. A deterministic three-dimensional
lattice provides bounded placement. Canonical 128-instance chunks require 40 or
704 indexed draws. The shader, renderer, preparation and resource admission remain
the real production paths; no production limit is raised for the measurement.

The cube profile explicitly selects the existing host retirement configuration
with 8,192 submission pins. At 704 chunks, resident material generations hold
3,520 pins and one graph needs 713 more, exceeding the default 4,096. Two in-flight
graphs require 4,946 pins. This changes only this fixture's requested envelope;
production, legacy mixed and functional-test defaults remain unchanged. The
original revision-1 90,000-instance attempt failed this admission before any
warm-up frame or measured sample; its raw failure is retained separately.
Cube cohort revision 2 records the selected pin envelope explicitly.

```sh
HORO_VFX_BENCHMARK_PROFILE=opaque-cube-scalability-v1 \
HORO_VFX_INSTANCED_ASSET_ROOT=/absolute/path/to/cooked-metal-package \
HORO_VFX_BENCHMARK_COUNT=90000 \
HORO_VFX_BENCHMARK_RECEIPT=/absolute/path/to/new-cube-iteration.jsonl \
MTL_DEBUG_LAYER=0 \
  build/vfx-metal-release/tests/HoroMetalVfxInstancedBenchmark
```

An absent profile variable selects the original mixed profile and its original
1,024/4,096/16,384 counts. An explicitly empty or unknown profile is rejected.
The cube profile shares the bounded warm-up/window and raw receipt protocol but
has a separate workload identity. Keep its results separate from the 21 mixed
profile measurements above.

The hidden manual case `[.manual-vfx-pin-budget-rejection]` exercises intentional
resource-budget rejection and cleanup without a long timing window. It runs only
when explicitly selected and is outside ordinary build/CTest execution.

Revision 2 completed seven independent processes per count on the same Apple M3,
with AC power observed, for 29,210 measured frames and no invalid GPU timestamps.
Each process used the declared 300-frame warm-up and at least 1,200 frames/20
seconds. These are medians of seven per-process nearest-rank quantiles, without
pooling or filtering samples:

| Cubes | Draws | CPU encode median (ms) | CPU encode p99 (ms) | GPU command-buffer median (ms) | GPU command-buffer p99 (ms) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 5,000 | 40 | 0.427 | 0.834 | 0.559 | 2.948 |
| 90,000 | 704 | 3.015 | 4.771 | 5.068 | 9.083 |

Cold schedule/packing medians were 1.146 and 20.648 ms respectively, outside the
measured replay loop. This startup measurement is not sustained moving-particle
throughput. The reference Unity CPU number includes different work and comes from
different hardware; neither the table nor a ratio of these times establishes
engine parity. No numerical pass/fail threshold was applied.

The corrected executable also passed all four original functional cases with
1,891 assertions and no skips, plus the hidden budget-rejection case with 1,244
assertions. A fresh legacy mixed-profile run completed 2,397 measured frames;
its invocation and cohort metadata exactly matched the original mixed workload.
These compatibility observations are separate from both measurement aggregates.

This extends count and geometry scalability coverage inspired by the published
Unity workload. It remains static, unlit, offscreen rendering: no per-frame
particle simulation, extraction, sorting, packing, uploads, shadows or final image
copy to the drawable is timed. It is not a reproduction of the moving, shadowed
Unity scene, a whole-engine FPS result, or evidence of matching Unity's speed.
The original mixed profile's conservative transparency admission also remains
authoritative; this opaque profile does not qualify 90,000 mixed particles.

### Dynamic CPU World measurement profile

`cpu-world-additive-v1` is a separate opt-in manual cohort for 1,024, 4,096 or
16,384 additive camera-facing billboards at 1920x1080. It advances the actual
`VfxWorld::PrepareSpawns` simulation, extracts a fresh immutable snapshot, schedules
and packs/uploads the canonical indexed chunks, compiles a new graph and submits
real Metal work on every frame. It does not mutate raw particle buffers to imitate
simulation. The existing spawn contract supports this bounded additive billboard
profile; it does not establish support for 90,000 dynamically simulated opaque cubes.

The deterministic initial burst has no continuous spawning, seed 1735, a unit-sphere
volume, radial speed 0.002, size 0.01 and opacity 0.05. Both particle lifetime and
effect duration are 60 seconds. The fixed simulation delta is `1.F / 60.F`:
one cold activation, 300 warm-up frames and at most 3,000 measured frames remain
below 55.02 simulated seconds. Simulation time and wall time are distinct. The
measurement still requires at least 1,200 frames and 20 wall seconds; exceeding
the frame/time envelope is an incomplete run, never a filtered success.

```sh
HORO_VFX_BENCHMARK_PROFILE=cpu-world-additive-v1 \
HORO_VFX_INSTANCED_ASSET_ROOT=/absolute/path/to/cooked-metal-package \
HORO_VFX_BENCHMARK_COUNT=16384 \
HORO_VFX_BENCHMARK_RECEIPT=/absolute/path/to/new-dynamic-iteration.jsonl \
MTL_DEBUG_LAYER=0 \
  build/vfx-metal-release/tests/HoroMetalVfxInstancedBenchmark
```

Raw observations separate simulation, extraction, scheduling, packing/upload,
graph compilation, encoding, submission and reclamation. The owner-phase total
adds the actual same-frame non-overlapping phases; extraction/scheduling/upload
are already inside graph preparation and are not counted twice. These are wall
clock durations around owner CPU work, not thread CPU time or whole-engine FPS.
Drawable acquisition, native completion, completion pumping and autorelease-pool
cleanup are outside that total. GPU time still covers the entire command buffer.
Graph allocation, compilation and destruction are included; no allocation-free
preparation claim is made. Native resources and upload slots are prepared cold.

The hidden manual case `[.manual-vfx-dynamic-reuse]` checks multi-chunk reuse,
client/native retention, completion and abort recovery. Untimed end checks compare
copied particle identities/positions, real native uniform bytes and the native
resource ledger. Snapshots and slots are not retained merely for diagnostics.
This profile remains outside ordinary CTest/build execution and does not define
an automatic performance gate or establish Unity/Unreal parity.

#### Dynamic pilot observations (2026-11-06)

One successful independent process per count completed 6,150 measured frames on
the Apple M3/Metal Release executable. Each passed the complete protocol, actual
motion/native-byte-change checks and native resource ledger checks, with no invalid
GPU timestamps. These are single-process pilot quantiles, not medians of seven
runs or a stable cross-engine comparison:

| Particles | Draws | Frames | Owner phase p50 (ms) | Owner phase p99 (ms) | GPU command-buffer p50 (ms) | GPU command-buffer p99 (ms) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,024 | 8 | 2,300 | 0.460 | 2.058 | 1.639 | 4.452 |
| 4,096 | 32 | 2,398 | 2.455 | 3.714 | 0.217 | 1.079 |
| 16,384 | 128 | 1,452 | 6.296 | 7.065 | 0.582 | 1.209 |

At 16,384 particles, the individually measured simulation and packing/upload
medians were 1.977 ms and 3.548 ms. The total column uses actual same-frame totals;
it was not obtained by adding these medians. Bracketed phases include their Catch
assertion/check machinery. The differing GPU ordering across these single-process
pilots is not enough evidence to rank scaling or claim a repeatable speed ratio.
The earlier 1,024-particle attempt was interrupted by the continuous free-disk
reserve guard; its partial artifacts remain separate and are not in this table.

The new hidden native reuse case passed with 26,420 assertions. All four original
functional cases and the prior budget-rejection case also passed, with zero skips.
Six deliberately invalid profile/count inputs were rejected before native receipt
creation; they are expected rejection controls, not successful measurement runs.
No new coverage, hosted gate, engine FPS or throughput-parity clearance is implied.


#### Dynamic packing optimization observations (2026-11-06)

The frontend now serializes each admitted canonical chunk once into its existing
cold-owned slot staging, before any native upload callback. Publication retains
its exact transaction claim, owner/revision admission, resident checks and native
write evidence. A later preflight rejection performs no native writes; a later
native upload failure releases graph/transaction claims but does not undo a
previous successful native write. No new per-frame staging allocation is added.

The VFX serializer uses a direct writer only for fully admitted, disjoint,
contiguous 128-lane matrix/color arrays with the required reflected strides and
bounds. Both matrix orders, reversed arrays and gaps are supported. Irregular,
interleaved or padded layouts and descriptor/output overlap retain the canonical
generic path. All inputs are copied and finite-checked before destination mutation;
unused lanes and padding remain zero and the suffix remains untouched.

On the same Apple M3/Metal Release profile, three independent processes per
implementation ran in interleaved AB/BA/AB order, with no profiler. All 79 cohort
fields matched. Each process completed its 300-frame warm-up and 1,200 measured
frames over at least 20 seconds, for 7,200 measured frames total. All frames had
valid GPU timestamps; motion, stable identities, native uniform changes and the
final resource ledger passed. These values are medians of three per-process
nearest-rank quantiles; samples were not pooled or filtered:

| Metric | Before p50 (ms) | After p50 (ms) | Before p99 (ms) | After p99 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Packing/upload | 3.733 | 1.306 | 4.613 | 2.352 |
| Owner phase total | 6.558 | 5.285 | 8.237 | 7.310 |
| GPU command buffer | 0.579 | 0.578 | 0.596 | 0.595 |

Within the three matched pairs, owner-phase p50 reductions ranged from 16.3% to
26.0%, and packing/upload reductions from 64.4% to 67.1%. Acquisition, native
completion, completion pumping and autorelease cleanup remain outside the owner
phase. Power/thermal state was observed at batch boundaries, without formal
thermal or background-workload control. This is a local observation, not an
engine FPS result, a general stability guarantee or Unity/Unreal parity proof.

An earlier baseline attempt stopped at warm-up attempt 50, after 49 completed
warm-up frames and before any measured sample. Its raw failure is preserved and
its underlying cause remains unknown; successful later runs do not explain it.
The manual harness now emits bounded native command-buffer status and NSError
details only on terminal failure, plus one untimed measurement-start marker.
No timeout, completion rule, assertion or sample admission was weakened.

Fresh validation passed 29 VFX extraction/serialization cases, 34 injected OpenGL
protocol cases and all six actual Metal functional/reuse/budget cases, with zero
failures or skips. Serializer tests compare full output against the independent
generic writer, including both matrix orders, inactive lanes, aliasing, malformed
layouts, typed failure ordering and zero allocations. The affected public frontend
header consumer also compiled. Local Sonar remains waived; no new coverage,
Codacy, hosted-platform or whole-ticket acceptance clearance is claimed. Manual
benchmarks remain opt-in and outside normal build/CTest execution.


#### Stable particle payload self-moves

Stable CPU compaction now leaves a particle's payload untouched when its source
and destination dense indices are identical. Handle admission, stable ordering,
slot mappings, generation retirement, tail clearing and canonical free-slot
rebuilding still run. In particular, keeping every particle does not bypass the
free-slot rebuild: that would change subsequent spawn identities after prior kills.
The private copy helper adds one branch and no storage, allocation or public API.

A separate matched Apple M3/Metal Release comparison retained the preceding
frontend/serializer optimization in both implementations. Three processes per
implementation ran in AB/BA/AB order, with the same 79 cohort fields and no
profiler, completing 7,201 measured frames. Medians of per-process quantiles were:

| Metric | Before p50 (ms) | After p50 (ms) | Before p99 (ms) | After p99 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Simulation | 3.041 | 1.667 | 4.015 | 2.306 |
| Packing/upload | 1.332 | 1.758 | 2.315 | 2.741 |
| Owner phase total | 5.480 | 5.101 | 6.871 | 6.501 |
| GPU command buffer | 0.580 | 0.580 | 0.597 | 0.601 |

Owner-phase p50 improved in all three pairs, by 4.0% to 8.0%. Packing/upload
worsened in this batch despite its unchanged source path; the cause is unproven.
The independently measured phase quantiles must not be added or subtracted to
infer the total. The preceding power/thermal, workload, timing-scope and parity
limitations apply; this does not set a reference-scene acceptance budget.

All 157 owning VFX API/simulator/buffer cases passed separately on macOS and Linux
ARM64 GCC 15, with no failures or skips. Two regressions check exact fixed/custom
payload bits, preserved and moved particles, canonical free-slot reuse, stale
handles, zeroed recycled payloads and allocation-free full-survivor compaction.
All six native Metal functional/reuse/budget cases also passed. OpenGL and software
Vulkan native results above belong to their recorded preceding source, rather
than a fresh run of this CPU-only change. No new hosted, coverage or analyzer
clearance is claimed; HORO-1735 throughput acceptance remains open.


#### Bulk committed CPU identity view

`CpuParticleExtractView::handles` borrows the simulator's existing committed
handle prefix in the same dense order and generation as its position/payload
streams. It allocates no identity table and adds no ownership authority. All
borrowed spans expire at the next owner-thread mutation; callers needing a longer
lifetime must copy values or consume the existing immutable World snapshot.
Candidate handles remain private until the same successful atomic commit. The
individual `HandleAtDenseIndex` API remains available for isolated checked reads.

World spawn extraction consumes this one admitted bulk view, preserving finite
geometry checks and immutable snapshot copies. It also computes the unchanged
placement scale once per emitter rather than once per particle; quaternion
rotation and large-world coordinate arithmetic keep their existing operations.

This appends a span to a public C++ result structure. C++ consumers/providers must
rebuild against the updated `HoroVfxApi` header; compatibility with an older binary
structure layout is not promised. No serialized asset schema or external C ABI is
changed. The owning public-header consumer checks the exact read-only handle type.


For this change, both macOS and Linux ARM64 GCC 15 passed 161 API JUnit entries
(including the new rollback sections), plus 29 extraction entries and the actual
public-header consumer build. All six native Metal functional/reuse/budget cases
passed. The bulk view allocated nothing in its direct extraction checks; successful
compaction/reuse and rejected/cancelled candidates retained the expected identities.

A separate AB/BA/AB comparison on the same Apple M3/Metal Release workload completed
14,354 measured frames across six independent processes with all 79 cohort fields
matched, no filtered samples and valid GPU timestamps throughout. Medians of the
three per-process nearest-rank quantiles were:

| Metric | Before p50 (ms) | After p50 (ms) | Before p99 (ms) | After p99 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Extraction | 0.394 | 0.277 | 0.991 | 0.725 |
| Packing/upload | 1.231 | 1.281 | 1.807 | 1.906 |
| Owner phase total | 3.456 | 3.413 | 4.554 | 4.440 |
| GPU command buffer | 0.582 | 0.582 | 1.126 | 0.855 |

Extraction p50 improved in all three pairs. Total owner-phase p50 changed by
27.3%, 1.2% and 0.8% respectively; the large first-pair variation is not a general
speedup guarantee. The total benefit in the median table is small, and packing
again worsened slightly despite unchanged packing code. Background/thermal effects
and per-phase costs are not fully isolated. These wall observations retain the
same assertion-inclusive scope and do not establish normal-engine FPS or a
reference-engine performance acceptance budget.
