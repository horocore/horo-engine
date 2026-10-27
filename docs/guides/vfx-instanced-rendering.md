# VFX indexed instanced drawing

Delivery identity: HORO-1735 / #1778 [VFX-004.3].

This document records the implementation and migration boundary for native
billboard and mesh drawing. Source authoring is in progress. Packing tests,
shader source, or headless graph tests do not qualify native execution.

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
The actual native off-axis fixture is still required; mathematical translation
evidence does not qualify pixels or device execution.
The shared HLSL qualifier also changes Vulkan and DXIL lowering. Neither route
is qualified by the GL/MSL diagnostic: Vulkan and D3D12 VFX admission remains
disabled or unsupported pending its own layout, native draw and lifetime evidence.

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
material arrays/matrices remain unsupported. Native malformed-layout and block
extent cases are authored but have not run. Immutable Material constants retain
their logical 20-byte member extent: the existing frontend queries native block
size and pads the allocation before publication, including Metal's native struct
extent. Callers must not substitute a 20-byte native buffer for that query.

The GL oracle extends the existing SDL material smoke composition with captured
cooked shaders and the complete frontend graph path. One off-axis scene includes
two distinct billboard instances, a mesh with authored nonzero Z, masked discard,
foreground alpha, depth-occluded alpha and additive color. Pixel-center ray/plane
depth is computed independently from the VFX packer. Actual GPU completion and
ordinary frame admission precede publication reuse; test-only readback/teardown
barriers do not change the production frame path. These new native assertions are
source-only until the opt-in GPU target is configured, built and run.
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
the native VFX color oracle must qualify this addition before delivery.

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
have new depth/abort/readback assertions; none has run for the current source.
Full VFX pixel/depth fixtures remain pending. Metal and Vulkan now have source routes
through their canonical prepared material/mesh/target owners, exact native
publication checks and indexed instance counts. Metal uses the existing native
command-use retirement records; Vulkan retains the existing graph submission
lease and resource-state/timeline authority. Both still keep this bit disabled
pending compile and native qualification. D3D12 remains an explicit graphics
foundation dependency. None of these source changes constitutes platform
qualification.

Slots store an inert weak frontend-incarnation marker and no frontend pointer.
Shutdown revokes that marker before teardown. The host must release slots and
graph borrowers before destroying the frontend: their canonical material leases
still obey the existing registry/native retirement contract. Staging destruction
does not allocate; it does not create another retirement or fence authority.

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
