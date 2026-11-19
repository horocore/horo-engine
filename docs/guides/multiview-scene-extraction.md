# Multiview scene extraction

`MultiviewSceneSnapshot` owns renderer presentation values, with one instance table
shared by every view. It never owns or mutates a runtime scene. Each instance names
an exact resident mesh and material generation; those identities do not retain GPU
resources. The frontend must resolve and pin them using its existing resource and
material completion lease before native submission.

The public headers belong to `HoroEngine::RenderApi`. Existing mono scene and
material submission callers retain their contracts. This additive surface exists
because `RenderSceneView` describes one borrowed camera and CPU mesh spans, while
multiview needs immutable frame-owned transforms and generation-safe resident
identities shared by a complete runtime-ordered view set. No public native handle
or XR session type crosses into Renderer.

The caller supplies the canonical scene/frame/origin/view and selected-device
publication fences. A retained snapshot remains memory-safe after scene teardown;
`Validate` rejects use against shutdown or replacement. The XR adapter must retain
and revalidate the exact `XRViewRenderPlan`, session/configuration generations and
current acquired-image evidence. A matching numeric view revision alone does not
replace XR identity validation. Presentation poses must be explicitly transformed
from their declared reference space into the canonical scene origin; local/stage
coordinates cannot be silently treated as world coordinates.

## Work and shader indexing

Extraction has fixed storage ceilings of 32 views and 128 instances. Caller limits
independently bound view count, instance count, instance/view cull tests and visible
draw expansion. Admission checks counts before traversing payloads. Each cull test
transforms eight box corners and conservatively checks six homogeneous clip planes;
touching/straddling geometry stays visible. Both clip-depth conventions are explicit.
Finite input whose matrix product or corner transform overflows is rejected.
No normal successful extraction or invocation lookup allocates, waits, calls native
code, changes producer state, or repeats simulation. Stack/value storage is bounded
by the public fixed ceilings. Errors use the standard owned diagnostic mechanism.

Every visible draw references one instance plus a view mask. `Invocation` maps a
packed visible-view ordinal to the same canonical instance and dense shader index
for multipass and instanced execution. It rejects invalid ordinals rather than
wrapping them into another view. Runtime semantic roles remain with the XR producer;
dense shader index zero is not an assertion that a view is the left eye.

`RequireInstanced` requires both indexed-shader support and instanced target routing.
`PreferInstanced` may select explicitly supported multipass; an unsupported complete
view count is never truncated. Capabilities are effective backend facts for the
captured device/capability generations, not feature claims made by this extractor.

## XR conversion and real native consumption

`XRMultiviewScene` belongs to `HoroEngine::XRApi`. It retains the exact view plan,
validates session/configuration/origin/acquisition identities, composes each
presentation pose with an explicit reference-to-world sample, and builds finite
asymmetric right-handed projections. `ExtractInstances` lets a host prepare view
uniforms before extracting ready canonical mesh/material generations once. These
are presentation copies; neither API acquires simulation or tracking authority.

`RenderGraphMultiview` adds one bounded complete graphics workload to RenderApi.
Targets name exact graph images, independent color/depth layers, rectangles and
prepared single-layer views. Geometry names the same canonical instance index,
resident material generation and explicit vertex/index/uniform graph dependencies.
Every target and draw must pass admission before any native recording. The
frontend resolves actual mesh/view generations and pins them and material leases
through its existing graph completion lease. No parallel resource registry or
completion ledger is introduced.

The non-installed `OpenXRMultiviewGraph` is host composition in
`HoroXROpenXRVulkanHostInterface`/`HoroXROpenXRVulkan`. The host header carries API
values and forward-declared frontend/frame borrows; the existing private implementation
dependency supplies frontend execution. No frontend dependency is published through
the host interface. Create runs before an active
frame, receives ready resources and the host's actual graphics queue, and resolves
`OpenXRSwapchain::Current` rather than treating graph contents policy as a native
import. It copies actual external-source acquired/final states and compiles their
mandatory releases. Execute rechecks actual owners and the frontend capability
publication and forwards the host scheduler's supplied order/waits/signal to
`ExecuteSubmittedGraph`. Host code still owns Present, ObserveSubmission,
PollRelease, frame lifecycle, and orderly shutdown. CPU graph destruction never
releases a runtime image. Owners/frontends outlive the prepared graph.

The Vulkan recorder shares the canonical graph command buffer, submission
validator, mesh capture, native retention and drain authority. Multipass renders
each ordered view through its exact single-layer attachment view and supplies the
dense view index through firstInstance. Instanced routing uses one compatible
complete layered tuple and contiguous visible-mask runs. Each draw retains its
own material/object transform; view index never substitutes for object identity.
Both paths enforce primitive-work budgets in addition to extraction bounds.
Single-sample color and optional depth-only D16/D32 attachments, MeshVertex
triangles and zero-to-one clip depth are the currently admitted native envelope.
Unsupported tuples fail before recording without silently changing device/backend.

The shipped `assets/shaders/runtime/multiview_mesh.hlsl` uses SV_InstanceID to
select WorldToClip and a per-object LocalToWorld. Separate cooked
HORO_MULTIVIEW_LAYER_ROUTING=0/1 variants use single-layer views or
SV_RenderTargetArrayIndex. The HLSL uses vector-first `mul` because its compiler
representation reverses matrix operands: final-ABI mathematical values must be
consumed with matrix-times-column-vector operations in SPIR-V. Native qualification
checks that operation direction as well as the final reflected layout.
`PackMultiviewShaderParameters` only adapts typed
mathematical values; `PackMaterialParameterBinding` remains the sole reflected
matrix/array offset/order/stride/overlap/padding authority. Prepare/upload outside
an active frame. Its returned generic byte extent can need native block-tail
padding: allocate/bind the actual selected pipeline's queried parameter block
size and zero the remaining bytes. Do not infer an ABI from a C++ struct layout.

Vertex-layer routing is optional and does not raise Vulkan baseline admission.
The device explicitly enables supported shaderOutputLayer; effective draw
capabilities also require complete dispatch. Native shader admission verifies
actual SPIR-V Layer decorations against enabled device support and the declared
pipeline routing. Routing contributes to the pipeline state digest; the existing
SingleView digest retains its previous reserved-zero encoding. OpenGL and Metal
reject this new workload/routing until they implement and advertise it explicitly.

## Projection history for runtime frame synthesis

The selected Vulkan Play graph stages its actual immutable view/geometry
extraction when the optional native frame-synthesis route needs a producer.
Object correspondence comes from the prepared Scene's stable authored
identities, independently of instance or draw order, and must match the
retained mesh generation. Each eye receives its own previous unjittered
world-to-clip transform. A predecessor is absent when either frame culled
the object in that eye or when sequence, prediction, origin, view, or renderer
evidence is incompatible. Bounded storage retains only one candidate and one
successful projection. The app commits the candidate only after native
compositor projection succeeds.

The Vulkan motion cook and multiview graph admit a separate multipass shader
with one signed floating-point color attachment and sampled hardware depth.
Uniform packing requires a genuine predecessor for each eye that draws the
object. The shader projects the same local geometry through each eye's current
and previous transforms and computes signed XYZ displacement in target-image
NDC. Current position drives fixed-function depth in the same pass. Invalid
previous samples keep current depth and write alpha zero so foreground geometry
still occludes farther geometry. This alpha is an engine validity marker.

The OpenXR Vulkan image bridge can import sampled floating motion and depth
allocations for an enabled native route. It checks both observed eye rectangles
against the exact retained attempt and packs separate array layers within the
smallest combined allocation. Imported registrations retire through the same
queue and final attachment layouts as projection images. They expose a typed
frame-registration input only for the current live native acquisition.

The OpenXR frame coordinator borrows those two owners and attempts both
acquisitions with zero-time waits against the same canonical frame. It
registers them atomically after the same Renderer submission accepts both
auxiliary targets and projection color. A timeout or omitted pair retains
native cleanup on those swapchains without extending the ordinary projection
image set. The next optional attempt waits for this native debt. When the
Renderer accepted a pair but the compositor uses color only, both exact
auxiliary releases still precede native frame end.

The native provider registry admits the actual selected EXT or FB route only
with an enabled renderer feature plan and the exact active native attempt.
Its compositor adapter borrows both released owners, checks the preceding
projection and exact per-eye evidence, and attaches the corresponding OpenXR
extension structure to every view. Missing optional evidence uses the same
single color projection. A native end failure retains the original frame fault.

The selected host loads the optional offline pair from
`shaders/runtime/multiview-motion-vulkan.horoshdr` and
`shaders/runtime/multiview-motion-vulkan.reflection.json` under its packaged
asset root. Cook this prefix with `HoroEditorVulkanShaderCook` and
`--multiview-motion`. During opted-in Play preparation, the renderer builds
the resident pipeline using the intersection of advertised OpenXR formats and
the selected Vulkan device's sampled attachment support. Missing or failed
motion packages leave the ordinary projection available.

Before optional submission, XRRuntime now requires completed renderer
classification covering the full motion and depth rectangle of both eyes
with zero unresolved samples. The report must name the exact current and
predecessor projections, image generations, and eye layers. Missing, stale,
or incomplete evidence leaves the projection in color-only mode.

The selected optional path can retain two owned sampled eye-depth
attachments in alternating projection graphs. It publishes the newest
depth only after the renderer has observed completion and the same native
projection ends successfully. The next compatible frame may borrow that
texture and its actual depth-attachment state for a graph-synchronized
sampled read. Missing preparation, a skipped or aborted projection, or
incompatible history leaves no previous-depth borrow.

The optional coverage program is a separate offline pair under
`shaders/runtime/multiview-coverage-vulkan` with the same
`.horoshdr` and `.reflection.json` extensions. Cook it with
`HoroEditorVulkanShaderCook` and `--multiview-coverage`. The viewport
records coverage only when compatible previous depth exists for both eyes.
The owner can consume one queue-observed result for the exact current
frame and its predecessor. Play sizes coverage bindings from the render
instances present during scene preparation. Missing coverage assets or
scene growth beyond that capacity preserve color and internal depth
projection.

Before the native cutoff, optional composition still requires classification
of the actual coverage pixels and the provider-specific invalid-pixel
encoding. A completed graphics pass reports execution of the coverage
program. EXT ignores the engine validity alpha, and FB consumes the RGB
vector.
[OpenXR frame synthesis contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameSynthesisInfoEXT.html)

The Linux native coverage regression uses the regular Vulkan backend and
finite readback after graphics completion. Cook `multiview_mesh.hlsl` with
`--multiview` and `multiview_motion.hlsl` with `--multiview-coverage`.
Point `HORO_NATIVE_MULTIVIEW_PAYLOAD` and `HORO_NATIVE_MULTIVIEW_REFLECTION`
at the first cook's stage package and reflection. Set
`HORO_NATIVE_COVERAGE_PAYLOAD` and `HORO_NATIVE_COVERAGE_REFLECTION` to the
second cook's equivalent files. In the Vulkan and OpenXR Editor test build, run
`HoroRenderVulkanTests '[vulkan][multiview][coverage][native-smoke]'`.
The regression checks packed pixels from two eyes at known depths and rejects
a stale completion. The same native qualification exercises the full-eye
graph when the separately cooked motion and reduction programs are supplied.

The closed reduction stage is available through the same offline cooker. Cook
`assets/shaders/runtime/multiview_coverage_reduce.hlsl` with
`--multiview-coverage-reduction`. Its two input views and two exact active
rectangles map to one pair of visited and unresolved counters per eye.
Every covered pixel contributes once. Only the depth-consistent code
resolves a sample. Place the matching pair under
`shaders/runtime/multiview-coverage-reduction-vulkan` in the installed asset
root. Play resets the counter storage per submission and retains the exact
paired mask through graphics completion. For native qualification also set
`HORO_NATIVE_MOTION_PAYLOAD`, `HORO_NATIVE_MOTION_REFLECTION`,
`HORO_NATIVE_REDUCTION_PAYLOAD`, and `HORO_NATIVE_REDUCTION_REFLECTION` to the
matching output files. The paired graph test checks asymmetric active areas,
unresolved samples, reset across successive submissions, and exact completion.
Renderer completion supplies coverage evidence. Native provider admission still
requires the matching runtime encoding and auxiliary image lifecycle.

## Public contract migration and evidence

The additive headers are owned by RenderApi or XRApi in the ownership registry.
MaterialBindingGenerationId moves to the narrow MaterialBindingIdentity header;
existing MaterialBinding includes continue to expose the same type and identity.
RenderGraphWorkload appends its alternative, preserving existing alternative
indices. Mesh/view mappings are trailing execution-request fields; legacy callers
remain valid. Graphics input usages map to the existing shared GraphicsInputBuffer
synchronization state rather than creating competing native state authorities.

Extraction/XR conversion success remains bounded and allocation-free. Prepared
CPU graphs, shared presentation/material storage and packed upload bytes allocate
before frame admission. Native recording is bounded, with no queue idle or
command-completion wait. Canonical leases retain submitted resources independently
of logical handle release, and BeginShutdown/PollDrain are the sole drain protocol.

Regression sources cover asymmetric origins/projections, unchanged source
snapshots, reflected row/column packing, ordered indexed multipass/instanced calls,
late invalid inputs and bounded work, completion lifetime, actual acquired-image
host graph composition, final-state release and stale reacquisition. The focused
Vulkan shader qualification workflow builds XR/OpenXR/Vulkan and the affected
public-header consumers on Linux/Windows. Source presence is not an executed test
receipt. Record current-head hosted results separately; no headset, visual parity
or performance claim follows from injected dispatch tests.
