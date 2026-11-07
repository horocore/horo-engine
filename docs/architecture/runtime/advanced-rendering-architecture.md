# Advanced Rendering Architecture

## Purpose

This document defines Horo Engine's high-end rendering feature stack: lighting,
shadows, global illumination, reflections, post-processing, temporal
anti-aliasing, upscaling, and capability tiers. It sits on top of the
[Rendering Architecture](./rendering-architecture.md) and the
[Material And Shader Model](./material-and-shader-model.md), filling in the
features required for modern visual fidelity without prescribing every backend
implementation detail.

The goal is to give the renderer a stable contract for producing AAA-quality
images while keeping fallback paths explicit and testable.

## Scope

Covered:

- deferred and forward+ lighting architecture
- shadow systems
- global illumination strategies and probes
- reflections and planar reflections
- HDR post-processing pipeline
- temporal anti-aliasing (TAA) and TAA upscaling (TAAU)
- vendor-neutral upscaling interface
- ray tracing boundary
- GPU-driven rendering and meshlets
- bindless resources
- virtual texturing
- occlusion culling
- terrain and foliage rendering boundary
- feature tiers and fallback policy

Not covered:

- RHI backend details (see [Rendering Architecture](./rendering-architecture.md))
- material parameter model (see [Material And Shader Model](./material-and-shader-model.md))
- asset import/cook (see [Asset Pipeline](./asset-pipeline.md))

## Core Decisions

- The renderer supports deferred, forward+ and baseline forward paths. The
  frontend selects a recipe from product preferences, scene requirements and
  effective feature/format/limit predicates under
  [ADR-028](../../adr/028-renderer-capability-limits-and-product-profiles.md).
- Lighting is clustered or tiled by default to scale with many punctual lights.
- Shadows use a configurable atlas with cascaded directional shadows and
  punctual shadow maps.
- Global illumination is provided by a combination of baked lightmaps,
  reflection probes, irradiance volumes, and optional real-time techniques.
- Reflections combine SSR, reflection probes, and optional ray-traced
  reflections behind a capability check.
- Post-processing is a explicit render graph pass chain, not hidden in
  individual materials.
- Reconstruction, denoising, frame generation and latency are separate provider
  categories under
  [ADR-040](../../adr/040-reconstruction-frame-generation-and-latency-providers.md).
- Ray tracing, mesh shaders and bindless resources require implemented backend
  paths, effective device support and product permission independently.
- Optional high-end features use declared, implemented and cooked raster or
  CPU-driven fallbacks. Missing required features fail admission; a profile
  cannot silently disable content requirements.
- The render graph owns resource allocation and barrier scheduling; individual
  features declare inputs and outputs.

## Lighting

### Deferred Lighting

Preferred path for High and Ultra when the complete GBuffer attachment, format,
sample and resource-limit requirements pass. Otherwise the frontend evaluates
the explicitly allowed fallback recipes before admitting resource work.

Pass flow:

```text
GBuffer Pass
  -> albedo, normal, material properties, emissive, motion vectors
Lighting Pass
  -> tiled/clustered light evaluation using GBuffer
Transparency Pass
  -> forward-lit translucent objects
```

GBuffer layout is declared as part of the active material/shader model contract
(see [Material And Shader Model](./material-and-shader-model.md)). The layout
must be stable enough for post-processing, lighting, and decal passes to consume
it without per-material negotiation.

### Forward+ Lighting

Preferred path for Standard when compute preparation and its resource requirements
are effective. Baseline uses forward raster with bounded CPU light preparation;
this is also the explicit fallback when compute is unavailable.

Forward+ performs a light culling prepass to produce a per-tile light list,
then renders opaque and translucent meshes in a forward pass.

### Light Types

| Type        | Notes                                                    |
| ----------- | -------------------------------------------------------- |
| Directional | Sun/moon; cascaded shadow maps; no position attenuation. |
| Point       | Omni-directional; shadow cubemap or dual-paraboloid.     |
| Spot        | Cone-shaped; shadow map.                                 |
| Rect        | Optional; area light approximation.                      |
| Sky         | Ambient contribution and environment cubemap.            |

Rect and Sky light recipes declare their shader/format/limit requirements.
Baseline may use basic approximations; richer area-light and dynamic-sky paths
are selected only when their requirements and product budgets pass admission.

### Clustered / Tiled Culling

Lights are assigned to screen-space tiles or world-space clusters. Each tile
stores a compact index list. Shaders iterate only over relevant lights.

Culling is performed on CPU or GPU according to effective support, the selected
recipe and the product's bounded light/work budget.

## Shadows

### Cascaded Directional Shadows

Directional lights use cascaded shadow maps (CSM).

- cascades are split by logarithmic or mixed partitioning
- cascade count is a validated product budget bounded by effective resource limits
- cascade blending reduces seams
- stable cascades reduce shimmer during camera rotation

The editor viewport implements the shared bounded fitting/profile contract and
native GL/Metal cascade consumers, with a Vulkan depth/lit batch implementation.
Its current source-level integration and platform qualification limits are
recorded in [Directional Shadows](../../guides/directional-shadows.md); viewport
coverage does not qualify the complete production lighting pipeline.

### Punctual Shadows

Point and spot lights use shadow maps or cubemaps. Shadow allocation is managed
by a shadow atlas.

### Shadow Atlas

A global atlas packs dynamic shadow maps. Allocation is frame-local and
prioritized by:

- light importance
- screen size
- distance from camera

### Contact Shadows

Optional screen-space contact shadows add fine detail near the camera. They are
a separate optional pass. High may prefer contact shadows; Ultra may prefer a
virtual-shadow-map recipe. Both require independent resource/format predicates
and an explicit fallback to the admitted shadow-atlas recipe.

## Global Illumination

### Baked GI

Lightmaps store precomputed indirect lighting for static geometry.

- UV2-based lightmap atlases
- directional lightmaps for normal modulation
- stored as textures in the cook output

### Irradiance Volumes

Sparse volumes store irradiance probes for dynamic objects.

- placed by artists or generated procedurally
- trilinear interpolation for blending
- fallback to ambient term when outside volumes

### Reflection Probes

Cubemap probes capture local specular reflection.

- blended by influence volume
- parallax correction for box/sphere volumes
- updated dynamically or baked

### Screen-Space Global Illumination

Optional real-time technique using the depth buffer and GBuffer.

- lower accuracy than baked GI
- useful for dynamic objects and contact color bleeding
- effective-capability and product-policy gated, with a baked-GI/probe fallback

## Reflections

### Screen-Space Reflections (SSR)

Trace rays in screen-space using the GBuffer and depth buffer.

- works for glossy and mirror-like surfaces
- fails at screen edges and occlusions
- combined with reflection probes to hide missing data

SSR and SSGI both consume the GBuffer and depth buffer. They are independent
passes and may run in either order; the render graph schedules them before any
pass that reads their output.

### Planar Reflections

Render the scene into a reflection texture for flat surfaces like water or
mirrors.

- expensive; used sparingly
- clipped to the reflection plane

### Ray-Traced Reflections

Available only when effective support includes the required ray operations and
resources, the effect implementation is linked, and product policy enables it.

- more accurate than SSR
- denoising required
- fallback to SSR + probes when unavailable

## Post-Processing

Post-processing is a chain of render graph passes operating on the HDR image.

### Standard Passes

| Pass                  | Purpose                             |
| --------------------- | ----------------------------------- |
| `MotionBlur`          | Camera and object motion blur.      |
| `DepthOfField`        | Bokeh blur based on depth.          |
| `Bloom`               | Bright highlight glow.              |
| `LensFlare`           | Artifact glow around bright lights. |
| `ToneMapping`         | HDR to display mapping.             |
| `ColorGrading`        | LUT-based color correction.         |
| `Vignette`            | Edge darkening.                     |
| `FilmGrain`           | Optional grain.                     |
| `ChromaticAberration` | Optional lens artifact.             |

### HDR Pipeline

Scene-linear HDR and HDR/EDR display output are separate contracts.
[ADR-033](../../adr/033-presentation-and-display-ownership.md) assigns OS/display
facts to Platform and resolved surface output to the render frontend. The graph
owns final conversion/composition against that contract; native attachment
encoding and metadata must agree so conversion occurs exactly once. A float
scene target alone does not prove HDR display support. Scene color policy stays
with RND-013; output capability modeling belongs to RND-008.2.

- scene is rendered to a high-precision floating-point target
- exposure is applied before tone mapping
- tone mapping uses a configurable curve (ACES, Reinhard, etc.)

### Exposure

Exposure may be:

- fixed manual value
- auto-exposure based on scene luminance histogram
- artistic key-value override

## Temporal Anti-Aliasing

Temporal reconstruction combines qualified real-frame samples to reduce aliasing
and optionally reconstruct a larger target extent. It is selected through ADR-040's
`ReconstructionProvider` category rather than an unqualified upscaler.

Requirements:

- per-pixel velocity vectors
- jittered projection matrix
- history buffer
- motion vector reprojection

The canonical provider input uses exposed/pre-exposed linear ACEScg, positive
linear view-space depth in meters, normalized `previousUv - currentUv` motion that
excludes projection jitter, explicit current/previous jitter, exposure scales and
typed optional masks. UV origin is top-left, `u` increases right, `v` increases
down and pixel centers use `((x + 0.5) / width, (y + 0.5) / height)`. Provider
adapters convert privately; they cannot infer sign, orientation, units or jitter
inclusion from native resources.

TAAU (TAA Upscaling) renders at a lower resolution and reconstructs a higher
output resolution using temporal accumulation. Horo's `taau` route is the built-in
fallback provider; `native` produces one real frame at target extent without
reconstruction-owned history. Histories are scoped to view, provider, mode,
extent, input, color and device generations and advance only after a successful
real frame.
Camera cuts, missing predecessors and incompatible generations reset explicitly.

## Upscaling

[ADR-040](../../adr/040-reconstruction-frame-generation-and-latency-providers.md)
replaces the legacy single upscaler interface with four sealed frontend-owned
registries:

- reconstruction converts one real rendered frame to one real reconstructed
  frame; spatial and temporal modes are distinct;
- denoising filters one named noisy effect (the ADR-039 effect-owned denoiser)
  and owns only that effect's history;
- frame generation may insert synthetic presentation frames between qualified
  real frames; and
- latency integration supplies markers/validated scheduling hints without owning
  images, input or simulation.

Verified components contribute inert backend-neutral descriptors. The frontend
selects providers from product intent, effective support, cooked variants, exact
input/color/extent contracts and finite budgets. Vendor/SDK/backend names grant no
capability and native SDK types stay private. `native`/`taau`, effect-owned
fallback, no frame generation and default scheduling are independent baseline
routes; selection never downloads a runtime, switches renderer or enables another
category implicitly.

Scene reconstruction runs before ADR-037's target output transform and
display-referred UI. Dynamic resolution is separate product/frontend policy that
publishes a valid render extent at a frame boundary; a provider consumes it and
cannot change scale or effect placement itself. Effects declare whether they run
at render or target extent.

Frame generation consumes qualified bracketing real reconstructed frames and
produces display-linear **scene content** after ADR-037 step 4. The frontend then
composes current display-referred UI (step 5) and applies the ADR-015/037 step-6
accessibility transform on every real and synthetic presentation image. A
synthetic frame has a separate ID and never advances simulation, input, extraction,
animation, audio, exposure, TAA/denoising/material/VFX history, jitter,
`ScenePresentationEpoch`, gameplay callbacks or real-frame statistics.
`RealRenderFrameId` is per `RenderViewId`; stereo pairs share the ADR-038 epoch
and matched synthetic counts. Missing history, cuts, drops, resize, output
or device changes or backpressure suppress generation and present real frames only
under optional policy. Denoising is the ADR-039 effect-owned `DenoisingProvider`.

Reconstruction consumes canonical depth/motion/masks at render extent. Frame
generation consumes target-extent guides. If the extents differ, the frontend's
explicit `GuideResolvePass` chooses the nearest positive finite depth and its
motion from each render footprint, conservatively maximizes masks and marks an
unqualified footprint invalid. Equal extents alias qualified inputs. Providers
may convert layout/units privately but cannot silently own a different guide
upscale or filtering policy.

Latency is resolved jointly with frames-in-flight and presentation policy but is
not frame generation. Providers may emit typed markers and bounded safe-point wait
hints; they cannot pump input, alter fixed timestep/present mode, busy-wait or claim
synthetic cadence as lower end-to-end latency. Measurements identify real versus
synthetic presentation and explicit unknown segments.

## Ray Tracing

[ADR-039](../../adr/039-ray-tracing-capability-and-abstraction.md) owns the ray
capability and execution boundary. Ray support is a vector of independently
reported, implemented and effective operations: acceleration-structure build,
update, compaction, inline ray query, dedicated ray pipeline, custom intersection,
any-hit, callable shaders, indirect dispatch, motion instances and serialization.
`InlineRayQuery` does not imply `RayPipeline`; neither update nor compaction is
inferred from a build path. The transitional `supportsRayTracing` boolean is not
an admission authority.

The effective snapshot carries typed limits for geometry/primitive/instance
counts, build sizes and alignments, recursion/payload/attribute bytes, supported
formats/stages/flags, shader-group/dispatch dimensions and native table packing.
Vulkan, D3D12 and Metal map only operations their actual native feature set and
Horo backend implementation can execute. Equal backend standing means equivalent
Horo semantics for an advertised operation, not a requirement to expose identical
native pipeline or shader-table models. OpenGL 4.1 has no initial ray route.

`RenderFrontend` owns a `RayScene` projection over one immutable GPU Scene/device
generation. It maps generation-checked mesh geometry to typed BLAS handles and
GPU Scene instances to TLAS generations. Logical triangle/AABB descriptors contain
explicit formats, ranges, bounds and update policy; they never expose native
addresses. Hit shading uses `MaterialId` → `MaterialBindingId` → derived
`RayHitGroupId`; that last ID is a pipeline group, not a third material identity.
AS handles follow ADR-027
`Pending`/`Ready`/`Retiring`/`Retired`/`Failed` state, dependency pins and
GPU-completion retirement under ADR-027/034. ADR-011 VFX batches are not in
RayScene. Masked geometry requires effective `AnyHit` (or equivalent coverage
query); otherwise it is omitted, never treated as opaque.

Build, legal update/rebuild, asynchronous compaction and TLAS publication are
typed render-graph passes with explicit input, scratch, result, queue/access,
budget and cancellation dependencies. A dedicated compute/copy queue is used only
when independently effective; otherwise the graphics queue runs the same passes.
A compacted or rebuilt candidate publishes at ADR-018 `RenderSafePoint` while old
frames retain old structures. No feature code issues native
build/barrier commands, blocks for GPU idle, performs frame-hot post-build readback
or mutates a structure consumed by an earlier generation.

Inline queries are declared variants in supported existing shader stages. A
dedicated ray pipeline separately declares ray-generation, miss, hit, optional
any-hit/intersection/callable groups, payload/attribute schemas and recursion.
Logical `RayDispatchTable` records use stable group IDs and typed local arguments;
backends privately pack native shader identifiers, addresses, sections and
alignment. Metal intersection function tables are private realizations of a
qualified operation, not proof of a DXR/Vulkan-shaped SBT.

Reflections, shadows, ambient occlusion and global illumination own their effect
recipes, geometry/material coverage, denoiser and finite budgets. Built-in product
profiles require no ray operation; Ultra may prefer individually enabled effects.
Each optional recipe declares a qualified inline/pipeline route and explicit
raster, screen-space, probe or baked fallback. Required ray content fails
admission when any operation, limit, shader, memory, geometry or denoiser predicate
is missing. No route changes silently in response to timing or pressure.

Ray traversal and visibility are presentation data. Physics, AI, audio, networking
and saves do not use hardware traversal as authoritative truth or synchronously
read it back. Device loss invalidates every native AS address, pipeline identifier
and dispatch table; recovery rebuilds from retained GPU Scene/mesh/cooked shader
descriptors after effective capabilities are revalidated.

## GPU-Driven Rendering

[ADR-038](../../adr/038-gpu-scene-and-instance-data-model.md) owns the persistent
GPU Scene projection. `RenderFrontend` owns one scene per admitted scene
runtime/incarnation and frontend/device generation. It maps stable, process-local
`RenderObjectId` values to generation-checked GPU slots, retains a bounded CPU
shadow and publishes immutable GPU Scene generations. Backends realize buffers
and copies; they do not own instance identity or inspect ECS storage.

The logical record contains current/previous-published transform, local and
origin-relative bounds, resident mesh generations, scene `MaterialId` plus packed
`MaterialBindingId`, and the five mutually exclusive ADR-036 classifications
(`Opaque`, `Masked`, `ForwardOnlyOpaque`, `TransparentSorted`,
`TransparentAdditive`). Visibility/shadow flags, LOD policy and
record/motion/origin generations complete the record. Sorted-alpha and additive
transparency remain separate; they are not one "transparent" class. A target's shader/reflection schema defines actual packing and table
indices. The logical C++ carrier is never copied as an assumed native GPU layout,
and descriptor/bindless indices are derived device-generation values rather than
asset or scene identity.

Render extraction emits bounded ordered create/update/remove delta batches over
immutable state. Admission validates source/scene/revision generations, queue and
capacity envelopes, dependencies and cancellation. Worker preparation owns its
inputs and cannot map native memory or publish slots. The render-capable owner
stages slots, pins and uploads, then atomically publishes a complete generation at
ADR-018 `CommandThreadPolicy::RenderSafePoint`. Queue pressure applies typed backpressure; multi-frame staging keeps
the prior coherent generation active and never drops deltas or exposes partial
updates.

Removed slots leave new visibility plans immediately but cannot be reused until
every prior frame, culling result, generated draw and debug lease completes. Slot
reuse increments a non-wrapping generation. Resource replacement, origin rebase
and device recovery likewise publish new complete generations while old consumers
retain old leases. Device-native addresses and compact table indices never survive
device recreation.

GPU culling, LOD, meshlet expansion and generated draws consume a published scene
generation plus independent per-view work buffers. Visibility/selected LOD never
mutates the base record or becomes gameplay truth. Generated counters/arguments
are bounded and carry scene/view generations; mismatches are rejected before
native execution. Multiple views share instance data without advancing simulation
or previous-transform state more than once.

The CPU-driven path remains a separately admitted raster recipe over immutable
snapshots. Missing compute/indirect support may select it only through the complete
fallback policy in ADR-028/036; required GPU-driven content returns a typed
failure. Allocating an instance buffer or issuing one indirect call is not evidence
that a backend implements this lifecycle or may advertise GPU-driven support.

## Bindless Resources

Bindless descriptor indexing reduces descriptor set pressure.

- textures referenced by index
- materials pass texture indices
- descriptor heap/array managed by the renderer

Fallback to a declared bound-resource variant when effective bindless support is
absent or product policy disables it; missing required variants fail admission.

## Mesh Shaders And Meshlets

Optional high-end path for geometry submission.

- meshes are preprocessed into meshlets
- task shaders perform culling
- mesh shaders output triangles

Fallback to traditional vertex/index pipeline when mesh shaders are unavailable.

## Virtual Texturing

Virtual texturing allows scenes to use more texture data than GPU memory.

- texture data is split into tiles
- runtime requests visible tiles
- sparse texture arrays or page tables manage residency

Virtual texturing is optional and admitted through its effective feature/format
requirements and product residency budgets, not through a profile rank.
Logical page policy and physical realization remain separate under
[ADR-164](../../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md).
The feature is Post-1.0; an API/backend name or sparse-resource bit does not advertise
support without complete artifact, material, budget, synchronization and qualification
evidence.

## Occlusion Culling

Occlusion culling reduces draw calls for hidden objects.

Techniques:

- CPU frustum culling
- GPU Hi-Z occlusion culling
- portal culling for indoor scenes
- software occlusion rasterization (optional)

## Terrain And Foliage Rendering

Terrain and foliage are not core primitives; they require dedicated subsystems.
This document defines the boundary:

- terrain produces render instances through a virtualized geometry system
- foliage uses instanced rendering with wind and LOD
- both integrate with the material system and shadow passes
- both participate in culling and occlusion

Detailed terrain/foliage architecture is covered in a separate document when
implemented.

## Product Profile Policy

[ADR-028](../../adr/028-renderer-capability-limits-and-product-profiles.md) owns
the Baseline through Ultra recipe preferences and the legacy API-tier migration.
This document owns effect algorithms and their concrete requirements. A profile
is not a bundle of guaranteed hardware features: every selected pass declares
required operations, formats, limits, inputs, variants and finite budgets.
The frontend records optional fallback decisions and rejects missing required
paths before resource creation. No feature or upscaler selects another renderer
backend, and no frame-hot path queries the native driver for capability policy.

## Render Graph Integration

Each feature declares its inputs, outputs, and resource dependencies to the
render graph. The render graph schedules passes, allocates transient resources,
and inserts barriers.

Features do not allocate render targets directly.

## Diagnostics And Profiling

Debug views:

- GBuffer channels
- shadow cascades
- light count heatmap
- cluster visualization
- motion vectors
- reflection probe influence
- overdraw
- GPU timing per pass

## Testing Requirements

- Visual regression tests for representative scenes.
- Profile/feature fallback tests verifying declared degradation, rejection of
  missing required paths and preservation of the active plan after failed updates.
- Shader permutation tests for each lighting path.
- Performance tests measuring draw call scaling.
- Determinism tests for TAA and motion vector generation.

## Related Documents

- [Rendering Architecture](./rendering-architecture.md): render graph, backend
  abstraction, pass extraction.
- [Material And Shader Model](./material-and-shader-model.md): PBR parameters,
  shader variants, feature tiers.
- [Asset Pipeline](./asset-pipeline.md): texture, mesh, and shader cook.
- [VFX And Particles Architecture](./vfx-and-particles-architecture.md):
  translucent effects, decals, volumetrics.
- Terrain And Foliage Architecture — not yet written; boundary is described in
  this document.
