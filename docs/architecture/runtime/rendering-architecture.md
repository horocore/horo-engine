# Rendering Architecture

## Purpose

This document defines the backend-neutral rendering model, render extraction,
frame and pass execution, GPU resource ownership, synchronization, resize,
device failure, and headless rendering behavior.

The equal first-class obligations of interactive backend modules are defined by
[Render Backend Parity Contract](render-backend-parity-contract.md).

## Core Decisions

- Scene and editor code submit backend-neutral render data. This includes
  imported meshes, generated primitive meshes, debug geometry, and GUI overlays.
- Backend-specific API types remain private to backend targets.
- The renderer consumes immutable frame snapshots, not mutable scene storage.
  Primitive meshes are generated on demand and cached; their render instances
  are indistinguishable from imported mesh instances in the render snapshot.
- GPU resources use typed generation-checked handles.
- Resource creation and destruction obey graphics affinity and GPU completion.
- Frame and pass ordering is explicit and validated.
- The null renderer is a supported backend for tests and headless workflows.
- OpenGL, Metal, Vulkan, and future interactive backends are equal sibling
  implementations. Implementation order does not grant architectural priority.
- Backend loss or unavailability returns typed errors rather than leaking API
  failures through engine interfaces.
- The active renderer backend is selected by configuration or command-line
  override at host startup. Runtime scene, editor, asset, gameplay, and MCP code
  do not branch on concrete backend types.
- Virtual Texturing owns logical page demand, selection and residency intent under
  [ADR-164](../../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md).
  Renderer owns admitted physical atlas/sparse resources, uploads, mappings, graph
  passes and GPU-safe retirement; it does not become logical page authority.
- Under [ADR-167](../../adr/167-vtx-feedback-readback-prediction-and-camera-data-ownership.md),
  Renderer also owns VTX feedback/compaction/copy passes, native resources and delayed
  readback lifetime. It publishes bounded immutable observations; it neither predicts
  page demand nor waits for same-frame CPU consumption.
- [ADR-168](../../adr/168-vtx-gpu-page-table-physical-cache-shader-and-material-ownership.md)
  gives Renderer complete ownership of VTX page-table/atlas/sparse resources, uploads,
  mappings, descriptors, graph synchronization and retirement. VTX supplies only
  finite logical revision intent, and each frame sees one immutable binding snapshot.

## Layer Model

```text
Scene Runtime / Editor Viewport
          |
          v
Render Extraction
          |
          v
Render Frontend
    frame graph, sorting, resources, uploads
          |
          v
Render API
    backend-neutral command and resource contracts
          |
          v
Registered Backend Module
    OpenGL / Vulkan / Metal / D3D12 / Null
```

The frontend owns engine rendering policy. Backends own API translation,
device/context state, synchronization primitives, and concrete GPU objects.

## Scene And Clip-Space Conventions

Horo scene and authoring space is right-handed, uses positive Y as up, and uses
negative Z as the default forward/view direction. Angles crossing typed math and
render contracts are radians. Matrices are column-major and transform column
vectors; local transforms compose as translation, then rotation, then scale
(`T * R * S`). Imported coordinate systems are normalized at the asset boundary
before scene or render extraction.

Scene transforms and camera values remain backend-neutral. Clip-depth range is
an explicit render/API adaptation: OpenGL uses `[-1, 1]`, while Metal and Vulkan
use `[0, 1]`. Concrete backends may apply this projection adaptation, but must
not independently redefine scene handedness, camera policy, transform order, or
authoring units.

### Camera-Relative Rendering And Universal Shader Precision

To maintain sub-millimeter visual precision across worlds spanning thousands of
kilometers while preserving universal GPU performance across Mobile (OpenGL ES /
Metal iOS), WebGL, Desktop, and Consoles:

- **Universal 32-bit GPU Shaders**: All vertex, geometry, and fragment shaders
  execute standard 32-bit single-precision (`fp32`) / half-precision (`fp16`)
  floating-point arithmetic. No GPU backend requires or relies on hardware `fp64`
  vertex attributes or double-single emulation.
- **CPU Camera-Relative Matrices**: The high-precision subtraction
  $P_{\text{inst\_cam\_rel}} = P_{\text{inst\_world}} - C_{\text{camera}}$ is
  evaluated on the CPU during Render Extraction. The Model-View matrix
  $M_{\text{view\_rel}} = V_{\text{rel}} \cdot M_{\text{inst\_rel}}$ is passed
  in 32-bit float uniform/constant buffers.
- **Vertex Transformation**: Vertex shaders compute
  $v_{\text{clip}} = P_{\text{projection}} \cdot M_{\text{view\_rel}} \cdot v_{\text{mesh\_local}}$,
  eliminating floating-point vertex jitter and Z-fighting at extreme world distances.
- **Shadow and Culling Precision**: Frustum culling and shadow cascade splits
  operate in camera-relative space, maximizing depth precision near the camera.

See [Coordinate Precision And Origin Rebasing](./coordinate-precision-and-origin-rebasing.md)
and [ADR-026](../../adr/026-large-world-precision-and-floating-origin-strategy.md).

## Backend Selection

Renderer backend selection is host-owned startup policy. The application
composition root resolves the requested backend through the verified renderer
component registry before creating a graphics window. It loads and negotiates
the exact selected module, adapts its provider into `RenderBackendRegistry`,
seals the registry, and passes the selection to `RenderFrontend::Create`. The
frontend constructs and initializes that inert instance with a backend-neutral
`RenderBackendConfig`, owns it for the frontend lifetime, and shuts it down
before releasing it.

Install, verification, probe, repair, and no-renderer behavior are defined by
[Renderer Distribution And Availability](renderer-distribution-and-availability.md).

Selection inputs, in priority order:

1. explicit command-line override
2. project or user configuration
3. host default for the current platform and build profile

Canonical backend identifiers:

Identifiers are lowercase ASCII slugs of at most 64 characters. They begin with
a letter, contain only letters, digits, or `-`, and do not end with `-`. Registry
registration rejects every non-canonical spelling before configuration lookup.

| Identifier | Backend | Status |
|---|---|---|
| `null` | `HoroEngine::RenderNull` | Required for tests and headless tools |
| `opengl` | `HoroEngine::RenderOpenGL` | Implemented; current editor migration path |
| `vulkan` | `HoroEngine::RenderVulkan` | Planned explicit-API desktop backend |
| `metal` | `HoroEngine::RenderMetal` | Implemented Apple parity peer |
| `d3d12` | `HoroEngine::RenderD3D12` | Planned Windows backend |

Configuration and CLI spelling use the same identifiers:

```text
render.backend = "opengl"
horo-engine run --renderer opengl
HoroEditor --renderer opengl
```

The default backend is a host policy, not a property of scene data or gameplay
code and not an architectural ranking. A host may temporarily default to the
only implementation that satisfies its parity gate; headless tools and CI
default to `null`. A platform-specific default may change only through an
architecture update and release note because it affects startup behavior,
driver requirements, and artifact validation.

Fallback is explicit. If a requested backend is unavailable, unsupported, or
fails initialization, the host returns a typed startup error unless the user or
profile opted into a fallback list:

```text
render.backend = "vulkan"
render.fallbacks = ["opengl", "null"]
```

Automatic silent fallback is forbidden. Diagnostics must include the requested
backend, attempted fallback backend if any, failure category, and relevant
capability or platform reason. A fallback to `null` is allowed only for tools,
tests, and explicitly headless workflows; interactive editor/game hosts must not
silently switch to `null`.

## Capabilities, Limits And Product Profiles

[ADR-174](../../adr/174-render-adapter-and-device-discovery-contract.md) owns the
backend-neutral adapter discovery and device-selection boundary. The selected
backend publishes a finite owned snapshot in canonical adapter-identity order;
host constraints select deterministically without native handles or silent
fallback. An explicit selection is paired with its discovery revision through
device creation, and unavailable, unsupported, stale, cancelled, shutdown, and
native creation failures remain typed and actionable. Discovery creates no
device or surface and closes admission before backend shutdown.

[ADR-032](../../adr/032-d3d12-baseline-and-agility-sdk-policy.md) owns the planned
`d3d12` component's native baseline: Windows 11 x86_64, feature level 12_0,
Shader Model 6.0, and host-owned Agility activation. The SDK package pin, runtime
selection, feature queries, and effective engine paths remain distinct. Legacy
barriers and root signature 1.0 provide the baseline; advanced paths are optional.
This policy does not implement the backend or change the Windows host default.

[ADR-031](../../adr/031-vulkan-loader-platform-and-version-baseline.md) owns the
planned `vulkan` component's native baseline: Vulkan 1.3 instance/device support,
explicit dynamic-rendering/synchronization2/timeline feature enablement, a single
system loader, and actual-surface admission on Windows/Linux desktop. It defers
portability-subset product support. These are downstream requirements, not a
claim that a Vulkan backend target exists or that optional engine features are
already implemented.

[ADR-171](../../adr/171-android-host-target-and-ownership.md) owns the separate
Android Vulkan baseline: API 29, production `arm64-v8a`, Vulkan 1.1 plus the
Android Baseline profile, and a replaceable PlatformAndroid-owned native-window
generation. Android Vulkan is a sibling backend specialization behind the same
Horo contracts; ADR-031's desktop qualification does not qualify it. OpenGL ES
is outside the initial Android product profile.

[ADR-030](../../adr/030-metal-platform-and-feature-baseline.md) owns the `metal`
component's native baseline: macOS 14.0+, Apple7 on native arm64 or Mac2 on native
x86_64, explicit MSL 2.4 shaders, and conventional command-buffer/encoder APIs.
Each shipped variant needs separate qualification. OS, GPU family, shader
language, implemented operations, and product profiles remain distinct checks;
neither a non-null device nor the `metal` identifier grants effective support.

[ADR-029](../../adr/029-opengl-core-profile-and-platform-policy.md) owns
the `opengl` component's native version, platform, and deprecation policy:
desktop OpenGL 4.1 Core minimum, actual-context validation, and qualified desktop
Windows/Linux/macOS support. “Compatibility renderer” does not permit the OpenGL
Compatibility Profile. OpenGL ES/WebGL references elsewhere in this architecture
describe possible future targets, not support provided by this component.
The macOS warning never changes backend selection or project settings by itself.
Context requirements must reach the selected private platform adapter before
window creation; completing that seam and native admission is downstream
implementation work, not a claim that today's bootstrap is fully qualified.

[ADR-028](../../adr/028-renderer-capability-limits-and-product-profiles.md) owns
the renderer capability and profile policy. Backend-reported device facts,
implemented backend operations, driver-adjusted effective support, and requested
product quality are distinct contracts. The frontend owns the immutable effective
snapshot; resource and plan admission use it, never a raw report or profile rank.

`Baseline`, `Standard`, `High`, and `Ultra` select rendering recipe preferences.
They are not graphics APIs, build profiles, hardware guarantees, or gameplay
budgets. Optional features use declared, implemented and cooked fallback recipes;
missing required features return typed failures. Limits have explicit units and
format checks validate the complete usage/sample/view combination. No resource
API silently substitutes formats or lowers quality.

Final support is established after device initialization, before resource work
is admitted; module metadata and probe results do not replace that validation.
Snapshots carry frontend/device/revision identity. Worker plans are revalidated
on admission, quality changes publish a new policy at a frame boundary, and
device recreation invalidates old snapshots and resolves policy again before
resource reconstruction. Profile changes never silently switch a backend.

The existing `RenderBackendCapabilities` booleans are a transitional implementation.
The richer value contracts, driver-policy provider and profile resolver are
downstream implementation work; this M0 decision does not claim them as available.

## Raster Render Path And Quality Policy

[ADR-036](../../adr/036-raster-render-path-and-quality-architecture.md) owns
production raster recipe selection. `RenderFrontend` resolves one immutable,
versioned `RasterRecipe` from the requested product profile, project/content
requirements, the effective capability/limit snapshot, cooked variants and
finite budgets. Backends report facts and execute the compiled graph; they do not
select, promote, demote or silently substitute a path. Scene extraction and
materials likewise provide typed requirements and compatibility, not global path
policy.

Horo has three production opaque raster families:

- **Forward** uses bounded CPU-prepared per-view/per-draw light lists and requires
  no compute, storage-buffer, indirect-draw or multiple-render-target support.
- **Clustered Forward+** uses a depth prepass and bounded 3D view-space cluster
  light lists. “Forward+” is the product-facing name for this clustered family;
  there is no separate 2D tiled production path.
- **Deferred** writes a versioned GBuffer schema and performs screen-space
  lighting. It is a hybrid frame recipe: incompatible opaque materials and all
  baseline transparency still use declared forward passes. Clustering is an
  optional light-preparation contract on that family, not a fourth path: a
  Deferred recipe must declare clustered preparation (same overflow as Clustered
  Forward+) or clustering-less preparation (Forward-equivalent bounded
  screen-space overflow). Unspecified Deferred light counts fail admission.

Scene-material classification is keyed by scene-conversion `MaterialId`
([ADR-027](../../adr/027-renderer-resource-identity-and-descriptors.md)): a
material-table key, not a resident GPU handle. Opaque, masked, forward-only
opaque, stable sorted-alpha and additive work are mutually exclusive cooked
categories. Combined alpha-test plus blend (dithered/stochastic transition) is
unsupported unless authored as exactly one of those categories or an optional
named stochastic/OIT recipe. Masked coverage is consistent between depth, shadow
and color/GBuffer passes. Sorted alpha is depth-tested, non-depth-writing and
ordered back-to-front with stable instance-identity ties; additive work uses its
own commutative pass. Order-independent transparency and refraction are separate
optional recipes with declared capability, memory and fallback contracts.

The concrete Standard PBR opaque/depth preparation boundary is
`Horo/Runtime/Render/StandardPbrPassPlan.h`. It consumes an already selected
raster family and exact resident material generations; it does not select a
recipe or inspect backend capabilities. Preparation is bounded, synchronous and
transactional, and publishes one owned canonical material table plus ordered
batches. Opaque and masked ranges are stable by `MaterialRuntimeId`; masked depth
and color/GBuffer batches reference the same range so coverage membership cannot
drift. Every plan declares scene color and depth products, plus motion vectors
when requested. Native backends realize those logical batches without changing
material classification, pass order or output policy.

[ADR-011](../../adr/011-vfx-effect-ownership-simulation-domain-and-renderer-boundary.md)
VFX batches keep their own pass kinds (`VfxParticlePass`, deferred/forward
`DecalRenderBatch`, `VolumetricVfxBatch`). The frontend maps those kinds onto the
resolved recipe; it does not re-encode them as the five scene-material
categories. A deferred-decal batch is remapped to the recipe's forward-decal
pass when Deferred is not selected; it is not dropped and is not left targeting
a missing GBuffer.

**MSAA can change the opaque family, not only the sample count.** Deferred MSAA
requires a declared multisample GBuffer, resolve and lighting contract;
otherwise a permitted Clustered Forward+ or Forward recipe that honors the
sample count may be selected. Diagnostics report that family change as a
first-class fallback reason. The resolver must not silently drop MSAA on
Deferred.

Cluster dimensions, light/reference counts, GBuffer formats, MSAA behavior,
semantic prepasses and overflow limits are typed recipe inputs bounded by the
effective snapshot. A profile name supplies no hidden numeric budget. Exceeding a
runtime bound follows the admitted deterministic policy and diagnostics; it never
changes the active path or accesses beyond allocated storage. Required complete
coverage fails admission when it cannot be represented.

Profile preferences resolve through complete-recipe checks:

| Profile | Preferred opaque family | Permitted ordered fallback |
|---|---|---|
| Baseline | Forward | None beyond failure of unsupported required baseline work. |
| Standard | Clustered Forward+ | Forward when project/content requirements and variants permit it. |
| High | Deferred | Clustered Forward+, then Forward. |
| Ultra | Deferred plus separately gated optional features | High, then its declared lower edges; Ultra is not a fourth opaque family. |

Each candidate must satisfy all required material, transparency, shadow,
scene-color, post-process-input, sample-count and budget predicates. The result
records requested and selected path/profile, failed predicates, fallback rule,
capability/content revisions and recipe generation. Explicit requirements may
remove fallback edges. Required content is never disabled merely to reach a lower
profile.

Every path publishes the same selected typed scene-color contract plus depth and
declared optional semantic resources.
[ADR-037](../../adr/037-scene-color-and-hdr-architecture.md) owns color encoding,
working space, exposure and output transformation. A versioned deferred GBuffer schema declares
its semantics, formats, encodings, sample behavior and producer/consumer stages;
material and lighting artifacts carry that identity. Forward paths may emit
normal, velocity or other semantic prepasses when a declared downstream input
requires them, so an effect does not select deferred rendering indirectly.

Recipe changes are explicit policy requests compiled and staged as new
generations. Publication occurs at
[ADR-018](../../adr/018-command-registration-permissions-threading-and-packaged-build-policy.md)
`CommandThreadPolicy::RenderSafePoint` after all required artifacts/resources
validate; that is the same graphics-affine frame-synchronization boundary as
[ADR-027](../../adr/027-renderer-resource-identity-and-descriptors.md). In-flight
frames retain their old generation.
Allocation failure, shader miss, slow frames, cancellation, stale inputs or device
failure never cause an unreported mid-frame downgrade. An adaptive-quality system
may request a new generation, but cannot mutate the active graph in place.

The editor's current bounded forward-preview lighting remains a migration
baseline. It is not production Forward until it consumes the common recipe,
material, graph, lifetime and qualification contracts. The Null backend can test
recipe resolution and graph structure without claiming native raster or image
parity.

## Scene Color, Exposure And Output

[ADR-037](../../adr/037-scene-color-and-hdr-architecture.md) owns scene color and
HDR policy. Every production raster recipe emits linear ACEScg/AP1 scene color in
canonical `RGBA16F`; wider-range/error-sensitive reductions use declared float32
intermediates. Packed unsigned, normalized LDR and native sRGB attachments are not
silent scene-color substitutes. Input color encodings are validated and converted
at asset/media boundaries, while non-color data textures bypass color transforms.

Exposure is a finite base-2 stop offset, `exposureEv`, with
`exposureScale = exp2(exposureEv)`: `+1 EV` doubles scene RGB and `-1 EV` halves
it. Automatic exposure is a GPU reduction whose `ExposureState` is consumed by
the next submitted view; same-frame histogram readback is forbidden. One
versioned `ExposureState` is published per view and shared by effects, histories,
tone mapping and diagnostics. Pre-exposure is a reversible internal
representation whose scale/generation follows every affected resource; it never
changes canonical scene values or becomes an effect-private estimate.

The frontend resolves a versioned `ColorPipelinePlan` using a pinned ACES 2 output
transform, cooked look policy, exposure and ADR-033's output snapshot. The plan
records working/output encodings, transform identities, gamut/white point,
transfer function, reference/paper white and peak/black luminance where known,
bit depth, dithering and all relevant generations. Scene color, float32
intermediates, LUTs, histories and output images are ADR-034 reservations
admitted before realize. Publication is
`CommandThreadPolicy::RenderSafePoint` on the host-declared render-capable
thread, the same class as `RasterRecipe` replacement. Backends translate this
plan; they do not choose tone curves, gamuts, brightness or fallback.

Baseline SDR output uses sRGB/Rec.709 primaries, D65 and the sRGB transfer
function. The first HDR contract uses a Rec.2020 container, D65, BT.2100 PQ, at
least 10-bit output and explicit finite paper-white/target-peak values in nits.
HDR display admission still requires the Platform/presentation facts and surface
contract in ADR-033. HLG, scRGB and platform EDR are separate future descriptors,
not aliases for HDR10.

Scene-referred work, including ADR-011 VFX additive/translucent batches, runs
before the output transform. Display-referred UI is composed afterward in
display-linear target space at the declared reference-white scale; the ADR-015
colorblind transform is ColorPipelinePlan step 6 and covers the complete
composed image, including HUD, before final gamut containment, dithering and
transfer encoding. Creative look/grading is step 3 and is not that pass.
Encoding occurs exactly once. Scene-linear captures and display screenshots are
separate typed products with their color-pipeline metadata.

Changing exposure/color/output conventions invalidates or explicitly rescales
dependent histories. SDR/HDR or display transitions stage a new plan and publish
at `CommandThreadPolicy::RenderSafePoint`; scene shading remains ACEScg. Missing transforms, invalid
metadata, hotplug, allocation failure or stale completion retain the last good
compatible plan or produce ADR-033's typed suspended/lost state, never a hidden
curve, precision or transfer-function substitution.

## Reconstruction, Generated Frames And Latency

[ADR-040](../../adr/040-reconstruction-frame-generation-and-latency-providers.md)
owns provider categories and timeline policy. Reconstruction, effect denoising,
frame generation and latency integration use separate sealed frontend registries,
effective capability/variant/budget checks, instances, histories and fallbacks.
A component may contribute more than one category but no category enables another.
Vendor SDK and backend types stay behind private negotiated adapters.

Real reconstruction consumes a canonical frame contract: linear ACEScg at its
declared exposure stage, positive linear view-depth meters, normalized
`previousUv - currentUv` motion excluding projection jitter, explicit jitter and
pre-exposure scales, typed finite masks, render/target extents and complete
generations. UV origin is top-left, `u` increases right, `v` increases down and
pixel centers use `((x + 0.5) / width, (y + 0.5) / height)`. Adapters convert to
provider conventions privately. Reconstruction runs before target output
transform and display-referred UI. Resolution control is separate product/frontend
policy and changes extent only through a new frame-boundary plan/history
generation.

Reconstruction consumes render-extent depth, motion and masks. Baseline frame
generation consumes target-extent guides. A frontend-owned `GuideResolvePass`
bridges unequal extents explicitly: nearest positive finite depth and its motion
win per render footprint, masks use a conservative maximum and missing qualified
samples become invalid. Equal extents alias qualified inputs. A provider adapter
may convert layout, orientation and units privately but cannot own an undeclared
guide upscale or change these semantics.

Temporal histories are scoped to view/provider/mode/extent/input/color/device
generations and advance only after a successful **real** frame. Camera cuts,
missing predecessors, incompatible scale/projection/color/exposure/motion/origin
or device/provider changes reset, rescale or migrate only through a declared
provider contract. Effect denoisers are the ADR-039/040 `DenoisingProvider`: they own
effect-specific signal/guide/history schemas and never become global
reconstruction history.

Simulation, real rendering and presentation have distinct IDs.
`ScenePresentationEpoch` groups all views of one scene frame;
`RealRenderFrameId` is per `RenderViewId`. Frame generation
may create `SyntheticPresentationFrameId` values bracketed by qualified real
frames, but cannot create a simulation/render frame or advance input, extraction,
animation, audio, exposure, temporal/material/VFX history, jitter, gameplay
callbacks or real-frame statistics. The baseline generated image contains
display-linear scene content after ADR-037 step 4; current qualified display UI
and the step-6 accessibility transform then run on every real and synthetic
presentation frame. Invalid/missing history, cuts, drops, resize/output/device
changes or backpressure suppress optional generation and present real frames.

Latency providers supply typed markers, measurements and bounded host-safe-point
wait hints. Input, Simulation, frontend and ADR-033 presentation keep their clocks
and ownership. A provider cannot pump input, alter fixed timestep/present mode,
busy-wait, violate frames-in-flight safety or report generated cadence as lower
simulation latency. Plans jointly admit queue depth, present policy, frame
generation and a hard latency budget, with explicit no-generation/default-
scheduling fallback.

Provider graph work and histories use finite ADR-027/034 resources and completion
retirement. Plan replacement is staged/atomic; provider failure/cancellation or
device loss keeps the last compatible generation or resolves an explicit fallback.
No runtime download, network access, opaque native cache or process-global SDK
state is part of baseline recovery or packaged execution.

## Backend Module Registry

Renderer backends are engine-internal modules with separate CMake targets,
private implementation directories, and independently packageable artifacts.
Installed product composition participates only through an exact verified
component record and negotiated first-party module ABI. Development and test
hosts may explicitly link/register a backend target, but that convenience path
does not define product discovery. Static constructors, linker-section discovery,
process-global registries, arbitrary filesystem scanning, and backend `switch`
statements inside `RenderFrontend` are forbidden.

```cpp
class IRenderBackendProvider {
public:
    virtual ~IRenderBackendProvider() = default;

    virtual Result<std::unique_ptr<IRenderBackend>>
    Create() const = 0;
};

struct RenderBackendDescriptor {
    RenderBackendId id;
    std::string displayName;
    std::unique_ptr<IRenderBackendProvider> provider;
};

class RenderBackendRegistry {
public:
    Result<void> Register(RenderBackendDescriptor descriptor);

    Result<void> Seal() noexcept;

    Result<std::unique_ptr<IRenderBackend>>
    Create(const RenderBackendId& id) const;
};

Result<void>
RegisterOpenGLRenderBackend(RenderBackendRegistry& registry,
                            IOpenGLPresentationPort& presentationPort);
Result<void>
RegisterNullRenderBackend(RenderBackendRegistry& registry);
```

The registry owns move-only descriptors and their providers. A concrete provider
may capture backend-specific platform services without exposing native handles
through the common Render API. It rejects duplicate or invalid IDs before renderer
selection. Registration and provider invocation do not create devices,
windows, swapchains, contexts, or worker threads; those side effects occur only
in the selected backend's `IRenderBackend::Initialize` path.

Provider registration and creation are serialized on the composition thread;
`Create() const` does not imply concurrent access. The registry may invoke a
provider zero or more times, and every invocation returns an independent inert
backend. Borrowed provider dependencies outlive the provider; any dependency
borrowed by a returned backend outlives that backend. `Register` consumes its
move-only descriptor on entry, so a rejected provider is destroyed before the
failed registration returns rather than being retained by the registry.

Providers return typed results, but allocation or module defects may still throw.
The registry is the exception boundary: it preserves returned failures, translates
thrown exceptions into `render.registry.provider_exception`, and rejects successful
results that contain a null backend pointer.

The host's build profile controls which renderer artifacts are produced, not
which components every installed editor must contain. Product runtime selection
uses only the installed, verified, ABI-compatible, successfully probed set. This
keeps the editor core and headless/CLI installations free of unused GPU
dependencies.

[ADR-052](../../adr/052-first-party-renderer-component-scope.md) assigns one
signed first-party component to one backend identity and keeps install,
verification, host support, runtime probe, selection and activation as orthogonal
application/product facts. `RenderFrontend` owns none of those facts. Components
cannot replace host renderer contracts or supply fallback policy; they implement
one selected backend behind the private module boundary.

```text
Resolve component record
    -> verify manifest/signature/ABI/probe state
    -> load exact verified module path
    -> negotiate private renderer module ABI
    -> adapt module function table into IRenderBackendProvider
    -> register provider in RenderBackendRegistry
    -> seal registry
    -> create RenderFrontend
```

The dynamic module loader and component registry are composition/application
services, not part of `RenderFrontend`. Development and unit-test builds may use
direct `RegisterOpenGLRenderBackend`/`RegisterNullRenderBackend` calls to avoid
packaging overhead while exercising the same in-process lifecycle contract.

First-party renderer modules cross a private, versioned C ABI with opaque
handles, host allocator/callback tables, explicit ownership, and strict unload
policy. The host adapter owns conversion into internal C++ renderer contracts.
This ABI is not the external extension/plugin ABI and does not establish an
unsupported third-party renderer marketplace. The package and ABI rules are
defined by
[Renderer Module Package Manifest](renderer-module-package-manifest.md).

Registry descriptors report identity and provider availability only. Product
component state is authoritative for installed/verified/probed status. Dynamic
GPU capabilities, driver versions, limits, and optional features are
authoritative only after the selected backend initializes. Selection UI may show
a known or installed module without claiming that device creation will succeed.

Interactive hosts additionally require side-effect-free module information and
window requirements before creating a presentation-capable window. That
pre-window contract and its required startup ordering are defined in
[Render Backend Parity Contract](render-backend-parity-contract.md); the current
OpenGL-first editor bootstrap is explicitly transitional.

## Backend Capabilities

Each backend exposes a value-type capability snapshot after initialization:

```cpp
struct RenderBackendCapabilities {
    RenderBackendId backend;
    bool presentsToWindow;
    bool supportsOffscreenTargets;
    bool supportsTimestampQueries;
    bool supportsCompute;
    bool supportsBindlessResources;
    bool supportsRayTracing;
};
```

Typed limits and an extensible feature set are future capability-contract
additions. They must not be consumed until their public render API types and
backend validation rules are implemented together.

Ray plans follow
[ADR-039](../../adr/039-ray-tracing-capability-and-abstraction.md). The transitional
`supportsRayTracing` field cannot admit work: acceleration-structure
build/update/compaction, inline ray query, dedicated ray pipeline and optional shader/dispatch
operations are independently effective and carry typed geometry, size, alignment,
payload, recursion, stage and dispatch limits. A backend advertises only routes it
both reports and implements; profiles and API/backend names grant none.

The frontend owns typed BLAS/TLAS handles and a `RayScene` projection over an
immutable ADR-038 GPU Scene generation. Hit groups derive from `MaterialId` /
`MaterialBindingId`; VFX is not in RayScene; Masked geometry requires `AnyHit`
or is omitted. AS size query, build, legal update/rebuild, compaction and ray
dispatch are explicit graph work under resource/residency budgets and
GPU-completion retirement, published at ADR-018 `RenderSafePoint`. Logical ray
shader groups and dispatch-table records are backend-neutral; native addresses,
identifiers and table packing stay private. Optional effects resolve declared
non-ray fallbacks, while required ray content fails admission. GPU ray results
never become gameplay query truth.

The frontend and feature systems query capabilities through render API values,
not by downcasting or including backend headers. Unsupported optional features
produce typed validation errors or disable declared optional passes before frame
execution. Required project features fail during project/runtime validation, not
mid-draw.

Capability reporting is enforceable behavior, not advisory metadata. A backend
whose snapshot reports a feature as unsupported must reject an execution plan
that requires that feature, even if earlier frontend validation was bypassed.

Backend capability snapshots are immutable for one initialized backend
instance. Device recreation may produce a new snapshot; users of capabilities
observe that through the frontend's typed recreation result.

## Renderer Diagnostics

[ADR-041](../../adr/041-backend-neutral-renderer-diagnostics-model.md) defines
the renderer-specific projection onto Foundation errors and process observability.
`Result<T, Error>` remains the operation contract. An immutable
`RendererDiagnosticEvent` records a correlated finding, decision, degradation,
failure or recovery; it never drives control flow and is not a metric or profiler
event.

The application composition supplies one bounded non-blocking diagnostic ingest
port to `RenderFrontend`. The frontend lends generation-bound emitters with
renderer, backend, safe device, product profile, capability revision and logical
work context to private backends/providers before device creation that can raise
native validation. Pre-init callbacks use a fixed adapter ring, then drain.
Producer threads perform no file I/O, UI calls, arbitrary allocation or sink
waits. Native callback adapters map private API severity/source/message identity
into registered Horo code, severity, subsystem and bounded fields without
exposing handles or branching on raw text. ADR-027/034/036/038/039/040
diagnostic payload lists populate this event; they are not a second API.
Required-content admission failure is `Error`; optional declared fallback is
`Warning`.

Accepted events project into the existing `ObservabilityRuntime` structured log
pipeline and retention. Renderer code owns no separate log store/file and publishes
no per-record data-bus events. Repetition follows bounded descriptor-declared
aggregation; per-frame/draw/resource activity uses metrics or an explicit profiler
capture. High-severity queue pressure uses the existing emergency path. Typed
fallback/lifecycle policy remains explicit even if diagnostic delivery fails.

Device/backend replacement closes old emitters and unregisters callbacks on their
owner thread before native state destruction. Accepted late evidence retains its
source generation; rejected stale submissions are counted safely. Shutdown never
waits for every normal sink on a renderer owner thread. Diagnostic export remains
allowlisted, redacted, local by default and under Observability/Application
ownership.

### CPU/GPU timestamps and pipeline statistics

[ADR-042](../../adr/042-cpu-gpu-timestamps-and-pipeline-statistics.md) owns
renderer measurement plans. CPU intervals use the process monotonic clock. Native
GPU ticks remain generation-scoped device/queue domains; cross-queue or CPU/GPU
ordering requires explicit calibration with a finite error bound. Per-queue spans
remain separate when that proof is unavailable, and overlapping work is never
summed into a fabricated frame total.

The frontend compiles registered logical measurement scopes and finite query,
readback, pending-result and sampling budgets into the render graph. Native query
heaps follow ADR-027/034; ring slots are plan-scoped indices retired after GPU
completion. Plan growth publishes at ADR-018 `RenderSafePoint`. ADR-039 AS and
dedicated compute/copy queues that participate in the real-frame graph obey the
same timestamp rule as graphics queues. Normal frames do not wait, map in-use
storage or call device idle for results. Delayed/out-of-order batches retain
source `RealRenderFrameId`, graph, plan, device, queue and clock generations.
Synthetic presentation owns no render query batch.

Always-on interval `frame.cpu` publishes as `engine.frame.cpu_time` (and GPU/present
counterparts) into fixed MetricsStore descriptors. Detailed pass
timings and canonical qualified pipeline statistics are profile-gated capture or
bounded snapshot data. Unsupported, not-ready, disjoint, partial and stale values
are explicit rather than zero. Per-frame/pass payloads do not become ADR-041 events
or unbounded metric dimensions, and backend code does not own telemetry export.

### GPU memory and resource inspection

[ADR-043](../../adr/043-gpu-memory-and-resource-inspection.md) projects the
ADR-027 resource registry and ADR-034 memory ledger into aggregate metrics and
explicit immutable inspection snapshots, including ADR-039 BLAS/TLAS identities
and their ADR-034-charged scratch/result allocations. The registry remains resident identity
and lifecycle authority; the ledger remains backing-accounting, admission and
pressure authority. An inspector owns neither model and cannot mutate them through
the read contract. Snapshot concurrency is per `RenderFrontendId`. Inspection
CPU-memory is a sibling of ADR-042 query/readback under the host diagnostics
envelope. Journal overflow during arming is capacity failure, not Partial.

Aggregate metrics preserve committed, reserved, live-payload, retiring and
reusable-slack meanings without adding overlapping values. Optional native usage,
budget and fragmentation observations retain sample age, availability and
measured/estimated provenance. Missing facts remain unavailable rather than zero.
Resource/allocation IDs, owner scopes, labels and native facts do not become
unbounded metric dimensions.

Detailed collection is an explicit finite operation and carries no per-resource
projection or mutation-journal cost until a profile-allowed session is admitted.
The frontend builds that session over bounded render-safe-point chunks, journals
only concurrent mutations while arming, then seals one consistent revision pair
and immutable pages. It never walks or copies the complete live registry in one
tool refresh. Page reads return owned immutable values, so snapshot expiry cannot
leave a borrowed view into recycled storage. Encoding and presentation work may
continue on cancellable workers over records charged to a diagnostics CPU-memory
allowance. Collection never waits for device idle, maps resource contents or
exposes native pointers, handles or GPU addresses. Record/byte/journal limits,
partial coverage, paging, retention, cancellation, stale generations and shutdown
are explicit. RND-017.9 queries these snapshots; it does not walk live backend or
frontend state.

### Render markers and debug labels

[ADR-044](../../adr/044-render-markers-and-debug-labels.md) owns stable logical
marker descriptors, typed frame/graph/pass/queue/resource/GPU-Scene correlation,
frontend graph placement and finite instrumentation plans. Resource class tokens
include `blas`/`tlas`. GPU-driven draws carry `GpuSceneInstanceId`. Backends
translate validated balanced streams into private native marker/object-label APIs;
they do not invent scope identity, expose native handles or let tool nesting alter
graph dependencies.

Marker modes resolve explicitly against effective capabilities and build/product
policy. Required unsupported modes fail before instrumentation admission; optional
fallback follows only a declared `NativeMarkersAndObjectLabels` → `NativeMarkers`
→ `LogicalCapture` → `Off` edge. Debug builds do not imply native support, and
validation-layer or external-capture activation remains separately owned.

Graph compilation places scopes after culling, merging and queue assignment and
validates same-context balance, nesting, native segmentation and pre-reserved
record/text capacity, including per-queue reserved floors plus an aggregate cap.
Runtime inserts reserve bounded queue capacity before command
mutation. Encoding uses registered strings and fixed storage with no frame-hot
interning/allocation. Marker emission never waits for GPU completion or performs
file/network/tool operations.

Resource debug labels are bounded privacy-validated metadata alongside ADR-027
identity, not descriptor/cache/handle identity. Relabel uses the exact generation
at ADR-018 `RenderSafePoint` on the render-capable owner thread. Device recreation resolves a new marker plan
and reapplies only admitted labels to new generations. Null validates logical
placement and budgets with synthetic records but claims no native-tool visibility.

### Backend validation and debug layers

[ADR-045](../../adr/045-backend-validation-and-debug-layer-integration.md) owns
native validation/debug-layer policy. The host resolves an immutable
`Disabled`, `Optional` or `Required` request before presentation-window and native
instance/context/factory/device creation. Debug builds do not imply activation,
Shipping defaults to disabled, and a live policy change requires backend or, when
the platform requires it, process restart.

The selected private backend resolves an inert plan, activates each facility at
its API-required creation stage and publishes generation-scoped realized state.
Request, reported availability and active features remain distinct. A missing
required facility fails with a typed result; optional policy may take only its
declared same-backend disabled retry after complete rollback. Validation policy
cannot lower the API contract, switch backend/device, use software rendering or
change product quality.

Native messages map through ADR-041's bounded generation-aware ingestion port and
registered codes. Callbacks perform no UI/file/network work, arbitrary allocation,
fallback or synchronous sink flush. Fixed filters, native mapping tables and
break policy are profile-validated before activation. Device loss/recreation and
shutdown unregister old sources on their required owner threads before native
state destruction; late messages cannot enter a new generation. Null validates
shared plan/fallback/mapping/lifecycle semantics but cannot qualify native layer
availability, callback behavior or overhead.

### GPU driver compatibility and workarounds

[ADR-046](../../adr/046-gpu-driver-compatibility-and-workaround-registry.md)
realizes ADR-028's restrictive driver-policy input. Delivery selects one signed,
versioned immutable policy; private backends normalize typed environment facts;
the frontend applies every matching rule before publishing effective capabilities.
Projects, users, plugins and remote services cannot override safety rules.

Rules may deny support, reduce upper bounds, strengthen alignment, deny routes or
select a registered semantically equivalent private route. They cannot grant
unreported/unimplemented support, weaken constraints, switch backend/device,
change profile/content or suppress typed failures. Matching is independent of file
order: restrictions compose conservatively and conflicts fail publication.

Applied snapshots retain policy/environment/reported/implementation revisions,
canonical matched rule IDs, before/after values and selected routes. Matching runs
only at initialization/recreation safe points, never per frame/resource/draw.
Device/backend/runtime change invalidates the generation and reapplies policy
before reconstruction. Null proves deterministic bounded resolution with synthetic
fixtures but cannot qualify a native driver or workaround.

### External graphics capture

[ADR-047](../../adr/047-renderdoc-pix-and-metal-capture-integration.md) defines
RenderDoc, PIX and Metal capture as explicit application operations. The caller
selects an advertised provider/mode, exact renderer/device/surface target, typed
frame or marker trigger, finite extent and restricted artifact policy. There is no
automatic tool fallback, focus-derived target or content/plugin-triggered capture.

The application service owns authorization, operation state, cancellation,
timeouts, staging, atomic publication, retention and export. Private backend/tool
adapters own native availability and begin/end translation on qualified threads.
Launch, attachment and in-process control remain distinct capabilities; a
creation-time requirement returns a user-approved restart/launch plan rather than
mutating the active process/device.

Capture consumes the active ADR-044 marker plan and records proven frame/graph/
surface correlation without enabling validation or changing render work. Native
payloads are restricted developer data because they may contain resource bytes,
shader source and rendered user content. Artifacts remain local/private, bounded
and excluded from automatic support bundles/upload. Device loss and shutdown stop
once at legal boundaries and quarantine incomplete staging without adding a
normal-path GPU-idle wait. Null proves shared request/state/artifact behavior but
claims no native capture support.

### GPU crash and device-loss incident evidence

[ADR-048](../../adr/048-gpu-crash-and-device-loss-diagnostic-bundles.md) separates
live device-loss collection from process-crash handling. Normal execution keeps
only finite generation-scoped event, graph, marker, capability, memory and surface
evidence rings. The first loss freezes the old generation, performs each admitted
native fault query once while its required parent remains alive, then allows
teardown/recovery; encoding and publication cannot gate recovery.

Process fault handlers never traverse renderer state or build bundles. They seal a
minimal preallocated renderer incident tombstone and notify Platform's collector;
rich association occurs out of process or on next launch from already durable/
shared bounded evidence. Missing, unsafe, timed-out and abruptly terminated
coverage remains explicit.

Native fault payloads are optional restricted data and never automatic capture,
telemetry or support-bundle content. The application/Observability owner controls
private staging, atomic manifests, retention and user-confirmed export. Incident
tasks cannot hold device/resources alive beyond one bounded native safe-point.
Null validates freeze/generation/budget/publication semantics synthetically but
cannot qualify native fault evidence or crash-handler safety.

### Render graph and resource inspector

[ADR-049](../../adr/049-render-graph-and-resource-inspector-ui.md) defines an
explicit immutable inspection bundle for one exact renderer/device/frame/graph
generation. The frontend projects post-compilation pass/resource/use/dependency/
lifetime/synchronization data into bounded owned pages. The editor never walks
live frontend/backend state or exposes native handles/enums.

ADR-043 memory/resource detail and ADR-042 delayed measurements join only through
proven generation, frame, graph, queue and scope identities. Pending, disjoint,
partial, nearest-aggregate and unavailable data remain explicit; same names or
latest values never establish correlation. Diagnostics and markers likewise join
by typed context, not text.

The application query service owns capture, paging, overlays, retention,
cancellation and safe export. `RenderInspectorTab` owns presentation state only
and cannot mutate barriers, queues, resources, residency, labels or graph policy.
Hidden tabs perform no polling/layout/formatting or instrumentation. Null supplies
deterministic logical fixtures but cannot qualify native synchronization, timing
or memory realization.

### Cross-backend reference images

[ADR-050](../../adr/050-cross-backend-reference-image-tests.md) defines native
reference-image qualification at named Horo render-graph capture points. Cases
own immutable scene/content revisions, fixed time/seeds/history, exact capability
predicates and canonical SDR/HDR image contracts. Backend-private readback
adapters normalize native format, swizzle, row pitch, orientation and resolve
semantics without exposing native objects.

Backend/platform golden gates and cross-backend semantic invariants are separate.
Every result preserves backend, environment, capability revision, real frame,
graph execution and baseline provenance. Unsupported, missing-baseline, timeout,
device-loss and infrastructure outcomes remain typed and cannot fall back to
another backend/reference or compare a blank image. Readback follows normal
submission/deferred-destruction ownership and never requires global GPU idle.

Null validates deterministic service, normalization, comparator, artifact,
budget, cancellation and shutdown fixtures. Only documented native hardware
lanes qualify rasterization, shader compilation, readback and driver behavior.

### Renderer benchmark and regression gates

[ADR-051](../../adr/051-renderer-benchmark-and-regression-gates.md) defines
immutable renderer workloads, exact environment cohorts, ADR-042 measurement
plans and robust protected-branch baselines. Warm-up, measured windows, process
iterations, capability path, cache state, frame/present policy and resource
budgets are descriptor inputs; ambient editor/project/power state cannot alter a
run silently.

CPU, GPU, present, throughput and memory meanings remain separate. A benchmark
cannot enable undeclared instrumentation, discard slow frames by value, continue
across device/profile generations, substitute hardware/backend or add a failed
candidate to its baseline. Unsupported, invalid environment/calibration,
insufficient samples, unstable/missing baseline, regression and infrastructure
failure are typed outcomes.

Null validates workload orchestration, sample/gate arithmetic, baseline and
lifecycle fixtures. Only matching documented native hardware cohorts establish
renderer performance qualification.

## Backend Implementation Boundary

Each concrete backend owns its API dependencies, context/device objects,
surface/swapchain objects, synchronization primitives, shader module objects,
pipeline caches, and backend-native diagnostics.

Rules:

- Backend source targets may include native API headers.
- Public render API headers may not include OpenGL, Vulkan, Metal, D3D12, GLAD,
  Volk, SDL3, Win32, Cocoa, X11, Wayland, or other native surface types.
- Backends implement `IRenderBackend` and backend-neutral resource contracts;
  they do not depend on `RenderFrontend`.
- Backend modules register providers explicitly with `RenderBackendRegistry`;
  registration performs no GPU or platform side effects.
- The frontend compiles render plans and owns render policy. Backends translate
  already-validated plans into API calls.
- A backend cannot mutate scene, editor, asset, gameplay, MCP, or application
  service state.
- External or plugin-provided renderer backends are a future extension point and
  are not part of the initial stable ABI.

## Backend Interface

```cpp
class IRenderBackend {
public:
    virtual Result<void> Initialize(const RenderBackendConfig&) = 0;
    virtual const RenderBackendCapabilities& Capabilities() const noexcept = 0;
    virtual Result<FrameToken> BeginFrame(const FrameDescriptor&) = 0;
    virtual Result<void> Execute(const RenderExecutionPlan&) = 0;
    virtual Result<void> Present(FrameToken) = 0;
    virtual void AbortFrame(FrameToken) noexcept = 0;
    virtual void AbortActiveFrame() noexcept = 0;
    virtual Result<void> Resize(FramebufferExtent) = 0;
    virtual void Shutdown() noexcept = 0;
};
```

`Shutdown` is explicit and idempotent for deterministic teardown. Backend
destructors must also release any remaining resources safely so an early-return
or failed composition path cannot leak native objects.

`RenderFrontend::BeginFrame` returns a move-only `RenderFrameScope` that owns the
matching token until successful presentation or abort. Its destructor aborts an
unpresented frame. Scope moves transfer abort ownership; moved-from scopes are
inert. Callers may explicitly `Cancel` a frame without relying on lexical scope
destruction. The frontend tracks its one outstanding scope, rejects a second begin,
and aborts plus invalidates that scope before backend shutdown if frontend
destruction occurs first. `RenderFrontend::SubmitFrame` is a convenience wrapper
over the same begin/execute/present path; it does not duplicate recovery logic.

Frontend methods, frame-scope methods, scope moves, and both destructors execute
serially on the same host-declared render-capable thread. Cross-thread scope
transfer and concurrent frontend/scope access are unsupported.

A begin failure or exception invokes token-independent
`AbortActiveFrame` because no token may have been returned. An expected
execute/present failure aborts the acquired token before the typed error is
returned. A successful begin result containing an invalid token is rejected as
`render.frontend.invalid_frame_token` and uses the same token-independent
cleanup. Backend exceptions are contained as `render.frontend.frame_exception`.
Initialization exceptions are contained as
`render.frontend.initialize_exception` and partial backend state is shut down.
Resize results also cross the frontend boundary unchanged; backend exceptions
are translated to `render.frontend.resize_exception`.

Backend interfaces use Horo value types. OpenGL names, Vulkan handles, GLAD,
Volk, SDL3, and native surface types do not appear in public render API headers.
Virtual dispatch occurs only at coarse frame, execution-plan, resource, and
lifecycle boundaries. Draw-item iteration and API command encoding remain inside
the selected backend, avoiding virtual dispatch per object or primitive.

All scene transforms, view/projection matrices, clip-depth conversion, bounds,
and viewport rays follow the shared [Scene Math](../foundation/scene-math.md)
contract. Backends may convert only the explicitly selected clip-depth range;
they must not introduce a second coordinate or matrix convention.

The typed pass contract binds a `PrimaryOutputAttachment` or a
`StaticMeshPassDescriptor` to a graphics pass. Static-mesh work contains only a
generation-safe target, extent, generic camera, immutable mesh views, transforms,
material bindings, and presentation tint. `RenderFrontend` validates target
generation and extent and dispatches the attached executor from
`RenderFrameScope::Execute`; editor code does not issue a separate viewport render
call. Primary output carries backend-neutral load/store operations and a finite
linear RGBA clear value. Copy and compute passes cannot bind the primary output. The implicit
primary output contains no native surface identity and is intentionally limited
to the first single-window slice; typed output handles replace it before
multi-window presentation.

`HoroEngine::RenderOpenGL` is an SDL- and ImGui-free module. Its provider borrows
an `IOpenGLPresentationPort`, remains inert until backend initialization, and the
backend controls context creation, make-current, present-mode configuration,
buffer swap, and context destruction through that port. The platform window
remains host-owned. The editor composition root registers the module, selects it
through `RenderFrontend`, and performs primary-output clear and presentation only
through the staged frame scope.

The OpenGL editor viewport owns generation-safe texture, texture-view, render-target,
buffer, and mesh handles through `RenderFrontend`. Creation is queued and published
only at the frontend resource safe point; replacement keeps the previous generation
usable until the new generation is ready, and release follows dependency order. An
editor-private OpenGL bridge resolves ready generic handles to backend instances for
frame-local command encoding. OpenGL names, framebuffer objects, and vertex-array
objects remain private to the OpenGL module and its matching editor integration.
The Metal editor viewport still uses its transitional frontend-owned logical target
identity and editor-private texture bridge until its corresponding resource-migration
ticket is complete.
Render extraction resolves versioned primitive descriptors through
`PrimitiveMeshCache`, emits a deduplicated table of immutable generic mesh
resource views, and emits instances containing only mesh resource identity,
transform, bounds, material, and presentation state. The OpenGL executor resolves
frontend-owned resident meshes and targets at a frame boundary; the transitional
Metal executor continues to own its native vertex/index buffer registry and
offscreen color/depth target. Both expose only a GUI-bridge texture identity to
`ViewportPanel`. The GUI identity remains app-private and is not a public render
texture handle. Render extraction and static-mesh submission are backend-neutral.
OpenGL and Metal editor integrations must
satisfy the same lifecycle and viewport parity suite defined by
[Render Backend Parity Contract](render-backend-parity-contract.md).
Selected editor instances carry backend-neutral tint and strength values; both
executors apply the same semantics without querying editor selection state directly.
The temporary typed `core.materials.default` resolution uses the shared neutral
viewport material and a bounded forward-preview lighting path. Render extraction
emits at most sixteen immutable world-space Directional, Point, or Spot light
values; both editor executors consume the same color, intensity, range, cone, and
normal-transform semantics. The first non-zero Directional light receives a
scene-bounds-fitted, texel-snapped 2048² orthographic shadow view with 3×3 PCF;
that selection and matrix are backend-neutral while native shadow resources remain
private to each executor. This is a viewport-quality baseline, not the general
material/PBR, clustered-lighting, cascaded-shadow, or punctual-shadow system.
The selected Light's influence geometry is also backend-neutral: Directional,
Point, and Spot line lists are built from the same immutable render snapshot,
then depth-tested without writing scene depth in each executor. Always-visible,
constant-pixel Light markers are composed and picked by the editor GUI layer.

Changing a project's renderer is a restart operation, not live migration of GPU
objects. The project setting records a canonical backend identifier; the host
closes project/runtime renderer state, destroys the frontend and matching GUI
adapter, recreates any backend-specific window attachment, and reopens the project
with a newly selected frontend. The current editor exposes the same selection
policy through `--renderer`; persistent project-setting and reopen orchestration
remain pending and must not be represented as implemented UI behavior.

## Render Snapshot

### Cinematic camera selection boundary

[ADR-119](../../adr/119-camera-authority-during-cinematics.md) keeps active-camera
selection outside the renderer. Runtime and PIE Camera services each resolve their
view-context proposals, while the editor viewport controller owns its authoring
camera. After `VariableUpdate` and before extraction, each owner publishes one
generation-checked immutable `CameraSelectionSnapshot`. All passes and outputs for
that rendered frame use the same selection; render execution cannot query sequence
state or replace the camera mid-frame.

Extraction projects the selection into frame-owned backend-neutral camera/view data.
The projection includes stable view/scene generations, camera identity, selection
epoch, transform and projection/lens values, transition state and generic
discontinuity evidence. It never retains a scene-component pointer or includes a
native graphics handle. Hard cuts publish one destination view. Camera-owned
single-view blends publish one interpolated view. An advanced two-view cross-fade is
accepted only through a declared frontend capability, qualified budget and explicit
fallback policy; no backend may silently change the selected transition mode.

Camera cuts and incompatible transitions change the generic selection epoch/
discontinuity evidence. Reconstruction, exposure, motion and other temporal
consumers apply their own history policy from that evidence. Cinematic Runtime never
calls backend/provider-specific reset hooks, and rendering never decides whether a
gameplay, cinematic or editor proposal wins.

[ADR-137](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
applies the same boundary to Terrain/Foliage. TerrainRuntime publishes a bounded,
immutable, generation/revision-tagged extraction snapshot and retains its lease through
frame extraction. RenderFrontend owns conversion to RenderApi resources/graph work;
the selected backend alone owns native buffers, culling commands, draws and deferred
GPU retirement. Rendering cannot mutate terrain residency/source/overlays, treat a
native handle as Terrain identity or infer a Terrain tier from the backend name.

[ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md)
refines that handoff. The Terrain adapter emits one bounded view-independent candidate
snapshot with neutral geometry/artifact references, material requirements, legal LOD/
seam data and finite costs. RenderFrontend derives `RenderObjectId` mappings and owns
per-view visibility, compatible LOD/transition selection, material/permutation admission,
resource realization, pass mapping and retirement. Residency, render visibility and
gameplay relevance remain independent facts.

Core 1.0 Terrain/Foliage uses deterministic bounded CPU frustum/distance/LOD/seam
planning and backend-neutral direct or instanced batches. It does not require compute,
GPU Scene, Hi-Z or indirect draws. A post-1.0 GPU-driven recipe may consume the same
semantic candidates only through a separately admitted, cooked, budgeted and qualified
renderer plan. Its GPU visibility/LOD results remain presentation-only and normal frames
perform no readback to drive streaming or gameplay.

`TerrainFeatureTier` and `RenderProductProfile` are distinct values even when labels
match. An aggregate plan records both axes, exact variants/limits/revisions and every
declared fallback. The renderer cannot use its profile to raise Terrain limits, discard
required layers/holes/instances or invent a missing shader representation.

[ADR-141](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
places renderer preparation inside the aggregate Terrain/Scene/cell transaction.
RenderFrontend returns typed request/incarnation/Terrain/resource revision receipts;
Ready resources remain private, Prepared publication is prevalidated for RenderSafePoint,
and Published resources remain activation-ticket-scoped until the aggregate root commits.
Failure/cancellation retires the candidate while old frames/resources remain valid. Render
alone acknowledges Retired after uploads, frames, GPU work and dependencies drain; visual
readiness cannot be inferred from decoded Terrain or native resource existence.

The scene runtime produces frame-owned render data:

```cpp
struct RenderWorldSnapshot {
    CameraData camera;
    std::span<const RenderInstance> opaque;
    std::span<const RenderInstance> transparent;
    std::span<const LightData> lights;
    DebugDrawSnapshot debug;
    SceneRevision sceneRevision;
};
```

Snapshots contain handles and immutable values. They do not contain pointers to
component pools, editor widgets, or backend objects.

Persistent GPU-driven projection follows
[ADR-038](../../adr/038-gpu-scene-and-instance-data-model.md). Extraction assigns
each logical renderable a generation-checked `RenderObjectId` that remains stable
across frames and distinguishes multiple subobjects emitted by one entity. It may
derive bounded ordered GPU Scene create/update/remove batches from acknowledged
snapshot revisions, but those batches retain no component-pool pointers and do
not replace the snapshot used by CPU-driven recipes.

`RenderFrontend` owns the render-object-to-GPU-slot mapping, bounded CPU shadow,
resource pins and immutable published GPU Scene generations. Current and previous
published transforms, bounds, mesh generations, `MaterialId` plus packed
`MaterialBindingId`, and the five ADR-036 classifications form the logical
instance record; an explicit target shader/reflection schema owns packed GPU
layout. Native addresses, descriptor indices and ECS indices are never persistent
render identity. `RenderClassification` keeps `TransparentSorted` and
`TransparentAdditive` distinct.

All views in one frontend-owned scene presentation epoch consume the same current
and effective-previous transform pair. A record's last-transform-change epoch
makes effective previous equal current in later unchanged epochs, preventing
repeated stale motion without rewriting every static record. Per-view execution
never advances shared transform history.

Delta application is a bounded transaction published at ADR-018
`CommandThreadPolicy::RenderSafePoint`. Queue pressure returns backpressure;
staged multi-frame uploads leave the prior generation coherent; failure or
cancellation rolls back the candidate. Removal and slot reuse wait for every
in-flight frame/culling/draw lease. Origin rebase projects a committed ADR-026
event; cell create/remove follows ADR-012 residency without GPU Scene evicting
cells. Resource replacement and device loss publish/rebuild complete generations
rather than mutating records visible to old frames. GPU visibility and LOD output
remain presentation data and cannot be queried as gameplay truth. ADR-011 VFX
batches stay off GPU Scene slots.

Multiple views, including game, scene viewport, thumbnails, and previews, use
separate `RenderView` descriptors over compatible snapshots.

## Runtime UI Projection

[ADR-073](../../adr/073-runtime-ui-ownership-scope-and-update-order.md) keeps
Runtime UI semantic state in `RuntimeUiService` and gives Renderer only immutable
per-view `UiRenderSnapshot` values. A snapshot names Horo logical UI/viewport/view/
layout/resource generations and contains bounded draw, text, clip and projection
data. It never contains mutable UI trees, ECS/component pointers, editor widgets,
ImGui IDs, native surfaces, swapchains, command buffers or backend handles.

[ADR-074](../../adr/074-runtime-ui-layout-units-and-measure-arrange.md) makes the
snapshot's boxes signed fixed-point logical DIP geometry produced by Runtime UI.
Renderer may derive physical pixel-snapped edges under the admitted output policy,
but snapping cannot feed back into measure/arrange, scroll extent or serialized
state. A policy that needs presented snapped hit geometry returns it as a new
generation-correlated interaction revision; the backend never patches the logical
snapshot or becomes layout authority.

[ADR-075](../../adr/075-runtime-ui-font-asset-family-and-fallback.md) keeps font
family/face matching, fallback order, coverage and logical metrics in Runtime UI.
Immutable positioned text runs name Horo `FontFaceInstanceId` and glyph identities.
Renderer may own rasterization strategy, atlas residency, upload and deferred GPU
retirement, but atlas miss/eviction/backend replacement cannot choose another face,
change metrics or line breaking, or expose parser/rasterizer/native font handles in
the UI snapshot.

[ADR-076](../../adr/076-runtime-ui-style-asset-token-and-inheritance.md) keeps token,
class, inheritance, visual-state and accessibility resolution in Runtime UI.
Extraction provides immutable Horo typed computed paint data and stable resource
identities. Renderer may convert linear colors, resolve resources and batch equal
paint state, but cannot read editor/ImGui styles, select state overrides, inherit a
property, replace a token or mutate a computed-style generation.

[ADR-080](../../adr/080-runtime-ui-presentation-scope-layer-and-route.md) gives
Renderer one immutable `UiPresentationPlan` per view. Its semantic World, HUD,
Screen, Overlay, Modal, Loading and Debug band order is fixed; a backend may batch
compatible draws within the plan but cannot reorder bands. Runtime UI and the host
own loading/debug availability, coverage and authority policy, never a backend.

World-space canvases project as ordinary view-dependent render instances under
declared depth/visibility policy. Screen-space canvases are frontend-owned passes
composed after world/display transform unless an explicit render plan declares an
earlier point. Runtime UI is part of `RenderExecution`; `RenderGui` remains the
separate host/editor/development GUI phase. A backend cannot reorder UI by native
convenience or make editor GUI composition the packaged-game path.

Viewport resize, DPI/safe-area/output change, backend replacement and resource
reload prepare complete new attachment/layout/render generations. In-flight frames
pin the old snapshot/resources until deferred retirement. Render completion returns
a typed presented/skipped/failed result correlated to the UI interaction revision;
RuntimeUiService, not Renderer, decides when that revision becomes eligible for
next-frame input. Failed/skipped presentation cannot silently adopt unpublished
hit-test geometry.

The concrete frontend admission boundary is
`Horo/Runtime/Render/UiRenderComposition.h`. It validates one bounded,
allocation-free, per-view pass span against a single graph owner and output extent.
Entries reuse `RenderTextureDescriptor` for color/depth structure, keep graph-local
resources inside Renderer, and borrow immutable `UiRenderSnapshot` values only for
the synchronous admission call. The frame owner retains accepted snapshot leases
until completion. Semantic composition point and presentation band ordering are
validated before graph execution; native backends cannot infer or reorder them.

Renderer produces `UiPresentationReceipt` evidence but does not apply it. Runtime UI
owns `UiPresentedInteractionState` and advances interaction eligibility only for a
strictly newer valid Presented receipt. Skipped and failed receipts preserve the
previous last-presented interaction revision.

The [VFX contract](./vfx-and-particles-architecture.md) extends the snapshot with
bounded immutable GPU simulation work, CPU/GPU particle sources, decals and volume
batches. VfxRenderExtractor never submits graph passes or writes mapped GPU buffers.
RenderFrontend admits/uploads CPU frame slices, schedules VFX Compute once per
scene/emitter step, then per-view Sort/Cull and dependent rendering passes. Multiple
views or reusing a snapshot cannot advance the same simulation step twice. Native
encoding, graph resource barriers and fence-based deferred retirement remain renderer
responsibilities. Retained snapshot and GPU leases may outlive logical scene teardown
but cannot reference destroyed scene storage or publish into a new incarnation.

[ADR-124](../../adr/124-vfx-gpu-simulation-readback-and-compute-fallback.md)
adds only cooked, bounded VFX readback intent. The frontend validates its normalized
schema and reservation, schedules an asynchronous post-compute copy and returns a
generation-tagged delayed observation. The backend privately owns staging, mapping,
cache maintenance and fences. No renderer path waits for same-frame VFX data, grants
it gameplay authority, chooses an effect fallback or changes the selected backend.

[ADR-125](../../adr/125-vfx-transparency-sorting-and-pass-placement.md) fixes the
VFX mapping inside every raster recipe: opaque/masked outputs join standard depth and
opaque work; sorted translucent forward reads but does not write completed depth; and
the scene-linear additive band follows it without per-particle distance sorting. The
frontend owns per-view stable CPU/GPU sort plans, aggregate work admission and overrun
evidence. Assets/backends cannot name/reorder these semantic passes or depth policies.

[ADR-127](../../adr/127-vfx-decal-projection-lifetime-and-rendering-path-policy.md)
keeps logical decal placement/lifetime in VfxWorld while the frontend resolves a
deferred-preferred or explicitly compatible forward path with the raster recipe.
Renderer owns physical atlas storage and native passes only. `RequireDeferred` never
silently remaps; forward-only tiers need an admitted forward variant, and neither path
writes scene depth or renders the same decal twice in one view.

## XR Views And External Presentation Targets

[ADR-160](../../adr/160-xr-rendering-openxr-compositor-and-renderer-ownership.md)
is normative for the XR/Renderer handoff. XROpenXR owns native frame, swapchain, image
and composition calls; Renderer owns Horo extraction, graph/GPU work, external-resource
registration and completion evidence. The private bridge translates native resources
without becoming a second owner.

XR supplies a bounded runtime-driven set of view descriptors and
generation-scoped runtime-owned image identities. Renderer does not assume that
an XR frame contains exactly two eyes, and public render contracts do not expose
fixed two-element matrix or target arrays.

The first production implementation may explicitly admit primary stereo only.
The contract still preserves view role, ordering, extent, projection, target,
and configuration identity so later quad-view or foveated-inset execution does
not require a public API migration. An unsupported view configuration is
rejected before resource acquisition and is never silently truncated.

For `XRProjection1_0`, that first production admission is exactly one primary opaque
stereo configuration with exactly two runtime views. One-view input is limited to an
explicit simulator/test profile. Any greater-than-two configuration is retained as
discovery evidence but fails admission before swapchain-image acquisition until the
complete N-view path is implemented and qualified.

Runtime-owned images enter the graph as typed external resources with declared:

- format, extent, array/view index, and permitted uses;
- color/depth role and composition requirements;
- acquire/wait/release ownership;
- synchronization and frames-in-flight lifetime;
- session, swapchain, image, and configuration generations.

An XR image and imported `RenderResourceId` are correlated, non-interchangeable
identities. Renderer borrows the allocation for one external lease and never destroys
it. XROpenXR releases/reuses it only after Renderer supplies completion evidence for all
covered queue references; CPU command recording alone is insufficient. The descriptor
also carries physical allocation extent versus active render rectangle, typed color
representation and initial/final external synchronization state.

Dynamic resolution, fixed or gaze-driven foveation, variable-rate shading,
density maps, depth submission, and motion inputs are renderer capabilities.
They are negotiated before frame execution. Foveation is not modeled as a
generic post-process, and Horo does not implement runtime-owned asynchronous
reprojection. Ordinary projection-layer rendering is the explicit fallback
when optional features are unavailable.

One immutable XR render-quality plan separates swapchain extent, render rectangle,
dynamic scale/controller, fixed or gaze-driven foveation, Renderer VRS versus runtime
density-map mechanism, privacy revision and fallback. Scale/plan changes occur at a safe
point and invalidate/migrate histories by explicit policy; they do not recreate a
swapchain every frame. Space-warp depth/motion/timing inputs require their own declared
semantics and cannot be substituted with generic motion-blur data.

XRRuntime supplies one complete frame and backend-neutral layer intent. Renderer returns
typed submission/completion evidence but never calls native image release or end-frame.
XROpenXR validates and encodes native layers after required targets complete. Runtime
asynchronous reprojection, distortion, scan-out and guardian/chaperone remain outside
the Horo graph. A desktop mirror is an independent ADR-033 output, not proof that the XR
compositor presented successfully.

[ADR-162](../../adr/162-mixed-reality-ownership-privacy-and-capability-tier.md)
keeps mixed-reality providers outside Renderer. Passthrough arrives only as an admitted
backend-neutral compositor-layer plan; Renderer owns virtual pixels/depth/occlusion but
receives no raw camera stream or permission handle. Bounded light estimates are optional
inputs with provider/spatial/access generations and cannot rewrite authored lights,
exposure or color authority. Observed room geometry reaches GPU resources only through
an explicit destination-owned derived adapter, never a provider callback.

See [XR Architecture](./vr-ar-architecture.md) for session, view, capability,
privacy, and qualification ownership.

## Frame Contract

[ADR-173](../../adr/173-render-queue-submission-and-fence-contract.md) owns the
backend-neutral queue, submission-order, completion-timeline and CPU wait policy.
Queue roles resolve through an effective frontend topology: a single-queue
backend may map graphics, compute and transfer roles to one logical queue, while
multi-queue mappings require admitted capabilities. Every admitted submission
retains one frontend-issued total order, explicit GPU timeline waits and one
signal on its own queue. Backends preserve that order without exposing native
queue or synchronization handles.

Normal frames use GPU-side dependencies and never CPU-block to order work.
Completion polling is non-blocking; bounded readback, deterministic tests,
teardown and documented recovery are the only CPU-blocking purposes, and each
requires a positive finite timeout. Resource retirement consumes exact queue
timeline evidence rather than frame indices, presentation, or CPU owner release.
Sequence and timeline exhaustion fail before wraparound.

One frame:

1. acquires a backend frame token
2. processes completed resource work
3. uploads bounded pending data
4. builds or validates the execution plan
5. executes ordered passes
6. resolves viewport and GUI targets
7. permits explicitly declared composition work while the frame scope remains active
8. presents or returns an offscreen result
9. retires deferred resources

Failure before presentation has a defined recovery result. A failed frame does
not leave the frontend believing resources were successfully committed.

## Render Graph

The frontend represents passes and resources as a directed acyclic graph:

```cpp
struct RenderPassDescriptor {
    RenderPassId id;
    RenderPassKind kind;
};
```

The render API defines backend-neutral authoring records for graph-local pass and
resource references, semantic resource uses, explicit dependencies, queue roles,
and finite graph limits. Pass references reuse canonical `RenderPassId` and
`RenderPassKind` values rather than creating a competing pass identity model.
Finalized records reside in one move-only owning `RenderGraph` exposed through
immutable deterministic-order views. `RenderGraphBuilder` owns pre-reserved finite
CPU storage under a process-local non-reusable identity. Creation fixes
owner-thread affinity; explicit idempotent shutdown releases storage without
backend or GPU work. Pass authoring assigns canonical identities and rejects
unknown or incompatible queue roles without fallback. Resource, use, and
dependency authoring validate builder ownership, record existence, and semantic
compatibility before preserving deterministic authoring order. Owner-thread
cancellation releases retained CPU storage, while successful finalization moves
all records into the immutable graph.

Resource declarations classify each graph-local buffer or texture as external,
persistent, transient, or history data. Transient declarations carry no resident
identity. The other classes explicitly import a generation-safe Horo buffer or
texture handle; the graph borrows that identity and does not own, prolong, resolve,
or translate its resident generation. An explicit export record names an observable
graph output. Imports, exports, and semantic read/write, attachment, storage, and copy
uses are finite, owner-thread-authored metadata with no backend/native handles or
ambient registry effects. Dependency DAG validation, read-before-write validation,
cycle detection, pass culling, lifetime compilation, barrier synthesis, and backend
translation remain separate render-graph delivery stages.

The validation stage is a synchronous, backend-neutral compile over immutable graph
metadata and is bounded by the graph's admitted capacities. It rejects exact duplicate
dependency records while preserving distinct dependency reasons for the same endpoints,
cycles (including disconnected cycles), and transient reads without an ordered producer.
Imported resources are treated as initialized only for this coarse graph stage; exact
initial-state and generation evidence is deliberately deferred to ADR-175 state/hazard
synthesis and is not claimed by this schedule.
Stable topological ordering uses authoring order to break dependency ties, producing a
total schedule even when resource users have no authored dependency path. That total
order is the input to ADR-175 hazard synthesis: RAW, WAR, and WAW transitions are derived
there, so authors do not duplicate resource hazards merely to make ordering deterministic.
Culling is opt-in per pass. The default conservative policy retains the pass regardless of
its declared writes; only a pass explicitly marked cullable may be removed when its
transient outputs cannot affect an export, imported-resource mutation, external
synchronization, or another retained pass. Producer and explicit predecessor closure is
retained. The owning schedule records retained passes in dependency order and a typed
authoring-order disposition/reason for every pass without borrowing graph storage.

The graph:

- validates read-before-write and cycles
- determines pass order
- identifies transient resource lifetimes
- provides synchronization requirements to explicit APIs
- remains backend-neutral

[ADR-175](../../adr/175-render-resource-state-and-barrier-model.md) is the
normative resource-state and barrier policy. Graph uses carry Horo-owned access,
operation, stage, range, layout-intent, and effective-queue state; they never
carry native barrier values. Imported generations declare exact initial and final
states, while transient resources begin undefined and must be written before
read. Deterministic compilation detects RAW, WAR, and WAW hazards over checked
texture subresources or buffer byte ranges and emits normalized transitions.

Queue-role changes become ownership transfers only when ADR-173 resolves them to
different effective queue identities. Those transfers use matched GPU-side
release/acquire operations tied to exact queue timeline values, never normal-frame
CPU waits. Explicit backends translate the complete normalized plan; implicit
backends realize equivalent ordering and visibility. Unsupported or malformed
state, range, ownership, generation, or translation evidence rejects the plan
before execution rather than selecting a hidden fallback.

Simple backends may execute the compiled plan serially. Scene systems do not
manually order backend commands around hidden global state.

## Resource Model

The canonical identity, descriptor, validation, and lifetime policy is
[ADR-027: Renderer Resource Identity and Descriptors](../../adr/027-renderer-resource-identity-and-descriptors.md).
ADR-027 is the sole normative owner of that policy; this section is an
orientation summary and must not be extended into a parallel resource model.
Supported resident resource classes are buffers, textures and texture views,
samplers, shader modules and pipelines, render targets, and meshes. Leaf,
derived, and composite classes share that model. Material bindings are typed
render data over those resources; scene `MaterialId` is a material-table key,
not a resident GPU handle. Backend framebuffer and binding-allocation objects
remain private.

Creation is described by immutable Horo-owned descriptors. Every public resource
class has a distinct typed handle whose identity contains the creating frontend
owner, registry slot, and non-wrapping generation. Handles are process-local
references rather than ownership, native values, or persistent identities.
Creation that reserves a slot returns a pending handle plus a
`ResourceOperationId`; only `Ready` generations may be submitted.

```cpp
ResourceCreation<TextureHandle> CreateTexture(const TextureDescriptor&, InitialData);
Result<void> DestroyTexture(TextureHandle);
```

The frontend validates structure, owner, generation, registry state, referenced
resources, and effective capabilities before native execution. A resident
generation retains the exact dependency generations named by its descriptor.
Release prevents new direct use of that handle immediately. Native destruction
waits until dependents drop their pins and prior GPU work completes on the
host-declared render-capable thread. Replacement never retargets existing
dependents. Handle state is validated when the frontend accepts a queued
request, not when a producer constructs it.

Asset IDs and render handles remain distinct. The asset system owns persistent
logical asset identity; the renderer owns one resident realization. Reload,
resize, replacement, and backend recreation publish new generations instead of
mutating an existing descriptor or preserving a process-local handle. Automatic
recovery requires a reconstruction source on the residency record
(`RecreateFromAsset`, `RecreateFromRetainedCpuData`, `RebuildByOwner`, or
`NonRecoverable`).

## Upload And Streaming

The canonical allocator, budget, residency and pressure policy is
[ADR-034: GPU Memory and Residency Ownership](../../adr/034-gpu-memory-and-residency-ownership.md).
The host supplies finite envelopes; the frontend owns reservation accounting and
readiness, while the backend owns native requirements, heaps and release.
Charge backing capacity, copies and replacement overlap once, including retired
resources until release acknowledgement. Heap slack is already charged capacity,
not memory returned merely by destroying a suballocated resource.

Streamed resources consume the World Streaming aggregate reservation through an
explicit adapter; its GPU claim and the renderer record project the same charge.
The renderer never selects cells to evict or discards activation-critical resources
under an Active cell. Non-streamed, GUI, viewport and presentation resources have
explicit owner scopes within the same host envelope. Native budget observations
remain estimates, not guaranteed allocation capacity or permission to exceed caps.

CPU asset preparation occurs on workers. GPU upload is queued to the
render-capable thread with:

- a memory budget
- per-frame upload budget
- cancellation before submission
- source asset and generation identity
- completion result

A late upload for an evicted, reloaded, or closed-project asset is discarded by
generation check.

## Shader And Pipeline Contract

The source language, compiler routes, normalized reflection and diagnostics policy
is [ADR-035](../../adr/035-shader-source-and-intermediate-representation.md).
ADR-035 is the sole normative owner of that policy; the renderer consumes its
validated target artifacts and must not select an alternate source or IR route.
Portable HLSL source feeds validated SPIR-V for Vulkan/GLSL/MSL routes and direct
DXIL for D3D12. Target-specific binding/packing maps preserve logical material
interfaces; native compiler defaults and raw reflection structs are not public
contracts. Packaged builds consume cooked variants, with controlled native
realization such as GL compile/link of cooked GLSL. Missing variants cannot trigger
an implicit authoring-source compiler or a blocking frame-hot pipeline build.

Shaders are cooked by the asset pipeline. Runtime loading validates:

- format and version
- backend and feature compatibility
- reflected resource layout
- material binding compatibility
- required vertex attributes

Compilation errors preserve source mapping and structured diagnostics. Runtime
fallback shaders are explicit product policy, not a silent default.

## Material Binding

Material data is backend-neutral and refers to semantic parameters and asset
handles. Binding layouts are derived from validated shader reflection.

Missing required parameters fail validation. Optional parameters use declared
defaults; arbitrary string lookup in the draw loop is avoided.

## Threading And Synchronization

The host declares the render-capable thread. That thread is the graphics-affine
thread for registry mutation and native create/destroy. ADR-018
`CommandThreadPolicy::RenderSafePoint` runs on it at the frame-synchronization
boundary; it is not a second render thread. Backend calls occur only there
unless a backend method explicitly documents thread safety.

Worker threads may:

- decode images
- build mesh data
- compile backend-independent render plans
- prepare upload payloads

They do not create or delete graphics objects directly.

Deferred destruction waits for the relevant frame fence or backend-equivalent
completion. The renderer owns that queue.

## Resize And Surface Changes

[ADR-033](../../adr/033-presentation-and-display-ownership.md) owns presentation
and display boundaries. Platform owns native windows/display snapshots; the
matching private adapter owns the surface attachment; the backend owns graphics
presentation resources. Host policy supplies intent, and the frontend publishes
the resolved output contract after checking platform, surface, and effective
device support. Requested settings and active output are separate values.

`RenderDisplaySnapshot` is the narrow RenderApi handoff for Platform-owned display
facts. A snapshot owns no native handles and carries a non-zero, strictly increasing
platform revision. Display records are bounded and canonically ordered by stable
machine-local `RenderDisplayId`; each record owns a sorted unique mode set, an exact
current mode, explicit color-space/transfer/dynamic-range facts, tri-state HDR support,
and optional finite luminance values in nits. Unknown HDR support and missing
luminance remain explicit rather than being fabricated as unsupported or zero.
Platform enumeration and monitor/window association remain outside RenderApi.

The platform owner feeds complete snapshots through `RenderDisplaySnapshotFeed` at
its owner-thread safe point. Publication validates the whole candidate before replacing
the last committed value and returns a bounded immutable delta with typed Added,
Removed, CurrentModeChanged, and CapabilitiesChanged reasons. Equal or stale revisions,
unknown color/HDR values, contradictory modes, malformed bounds, and non-owner
publication fail with typed results and leave the previous snapshot unchanged. There
are no callbacks, worker jobs, or hidden polling threads; `Stop` idempotently closes
admission and releases retained facts before Platform shutdown. Headless hosts simply
omit this feed rather than inventing a display.

`PresentMode` remains the single native-free pacing-mode vocabulary used by backend
configuration and negotiation. Host intent is either one explicitly required mode or
a bounded unique Auto preference order. Backend capabilities are a bounded, canonical
sorted set. `NegotiatePresentMode` is pure and deterministic: an explicit request only
succeeds on an exact match, while Auto examines only the host-provided order and records
the requested mode, resolved mode, selected preference index, and degraded fallback
status. Empty, duplicate, unordered, unknown, unsupported, and no-match inputs return
typed failures; negotiation never invents FIFO, tearing, latency, or frames-in-flight
policy.

The negotiation value owns no surface, callback, worker, or native resource, so it has
no independent thread, cancellation, or shutdown lifecycle. The application host owns
the immutable request and invokes negotiation before a renderer-owned surface lifecycle
applies the resolved value. Surface reconfiguration, pacing, HDR, and backend-native
translation remain in their owning downstream contracts.

Publish revisioned logical output candidates with owner-thread commands before
layout/extraction; commit active output only after realizing the same candidate
before native frame acquisition. Output-dependent extraction retains that
revision. A failed, pending, or mismatched realization skips its output instead
of executing a new-size plan on the previous surface.

Logical window size, framebuffer extent, DPI scale, and render target extent are
distinct values.

Resize is coalesced and committed at a frame boundary. The frontend rejects
resize while a frame scope is active without aborting that frame. Zero-sized
minimized surfaces suspend presentation without creating invalid resources.

Viewport render targets are recreated transactionally. Consumers observe either
the previous valid target or the new valid target.

Native surface reconfiguration has a stricter physical limitation: an OS-invalid
old surface cannot be promised as a rollback target. Publish a new configuration
only after success; retain the old one only if still usable, otherwise report
Suspended/Lost output. Coalesce revisioned platform events, commit native changes
before backend-frame acquisition, and retire prior surface generations safely.
Native window size, offscreen editor viewport resolution, and scene dynamic
resolution must not share a resize authority.

`RenderSurfaceLifecycle` is the backend-neutral owner-thread state machine for the
current single primary surface. It retains at most one pending command, reports an
explicit superseded sequence when coalescing, and publishes that complete native-free
candidate and a new snapshot revision before layout/extraction. It freezes exactly one
immutable transition at a render safe point while keeping any later pending candidate
separately observable. The private native owner completes that exact
owner/generation/revision/sequence with `Ready`, `Suspended`, `Lost`,
`PreserveActive`, or `Unattached`; mismatched and late completions cannot publish.
Ready replacement advances the surface generation, while a preserved usable output
keeps its prior generation and configuration. Close/loss commands cannot be replaced
by later ordinary resize work. If an earlier native outcome makes the one queued
candidate incompatible with the newly realized state, the next safe point reports
that candidate as explicitly invalidated and frees the bounded queue. The contract
owns no native handles, worker jobs, or waits; native retirement remains deferred in
the concrete backend.

## Device Or Context Failure

Backends classify failure as:

- recoverable frame failure
- surface recreation required
- device/context recreation required
- fatal unsupported or corrupted state

Recovery tears down the old registry and recreates resources from each
residency record's reconstruction source (asset identity, retained CPU data, or
owner rebuild). Resources without a source are `NonRecoverable` and their
owners are notified. Backend handles are never assumed stable across
recreation.

## Null Renderer

The null backend:

- validates frame, pass, and resource contracts
- returns deterministic handles
- performs no GPU or window work
- supports headless application and scene tests
- records bounded command summaries when requested

It must not silently skip validation that a real backend relies on.

## Editor Integration

Editor viewports request render views and targets through renderer frontend
capabilities. Tabs and modals do not own backend resources.

The GUI backend consumes a declared final target or submits its own pass through
the render plan. A transitional editor-private graphics bridge may render after
scope execution and before presentation while a backend migration is in progress;
native GUI or graphics types do not enter RenderApi. Modal dimming and UI
composition do not mutate world rendering state.

## Metrics

The renderer exposes bounded metrics:

- CPU render extraction and submission time
- GPU frame and pass time when supported
- draw and dispatch counts
- resident and pending resource bytes
- upload bytes and queue depth
- shader/pipeline cache hit rate
- deferred deletion depth

Metrics follow [Observability Metrics And Profiling](../observability/observability-performance.md).

## Testing

Required tests cover:

- backend API header isolation
- duplicate/invalid backend registration rejection
- sealed-registry mutation rejection and deterministic enumeration
- unknown, not-installed, incompatible, and unavailable backend selection
  returning distinct typed errors before window creation
- verified-path module loading and private ABI negotiation
- probe failure/crash isolation and no-renderer repair routing
- provider invocation remaining inert until backend initialization
- provider-returned failures, thrown exceptions, and null successful values being
  contained at the registry boundary
- frontend initialization ownership, deterministic backend shutdown, and
  initialization exception containment
- frontend frame submission aborting owned frame state after expected failures
  or backend exceptions
- frontend rejection and cleanup of invalid successful frame tokens
- frontend resize propagation and exception containment
- capability-incompatible and malformed execution plans being rejected
- reported/implemented/driver-adjusted support separation and profile resolution
  following ADR-028, including stale snapshots and missing required fallback variants
- installed editor composition loading only the selected verified component while
  development/headless profiles may register explicit static/null modules
- render graph cycle and invalid-resource rejection
- deterministic pass ordering
- stale resource handle rejection
- upload cancellation and generation replacement
- terrain/foliage extraction generation, tier/capability rejection and GPU-resource
  retirement without native identity leaking back to Terrain
- Terrain view-independent candidate capacity/multi-view tests, CPU 1.0 per-view LOD/
  seam selection and explicit post-1.0 GPU-recipe admission/fallback
- Terrain material classification/permutation failure and Terrain-tier/render-profile
  independence without silent layer, hole, instance or required-quality loss
- resize, minimize, and target recreation
- deferred destruction after frame completion
- shader reflection/material validation
- null backend contract equivalence
- backend initialization and device-loss error mapping
- XR external-image generation, acquire/import/release ordering, and loss
- one-view, primary-stereo, and explicitly unsupported greater-than-two view
  configurations
- XR dynamic-resolution/foveation capability fallback and history invalidation

## Related Documents

- [Coordinate Precision And Origin Rebasing](./coordinate-precision-and-origin-rebasing.md)
- [ADR-026: Large-World Precision and Floating Origin Strategy](../../adr/026-large-world-precision-and-floating-origin-strategy.md)
- [Render Settings UI Reference](./render-settings.html): quality presets, render feature toggles, resolution, and GPU profiler panel.
- [Render Backend Parity Contract](./render-backend-parity-contract.md)
- [Renderer Distribution And Availability](./renderer-distribution-and-availability.md)
- [Renderer Module Package Manifest](./renderer-module-package-manifest.md)
- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Scene Runtime](./scene-runtime.md)
- [Asset Pipeline](./asset-pipeline.md)
- [Built-In Scene Primitives](./built-in-scene-primitives.md)
- [Ownership And Resource Lifetime](../foundation/ownership-and-resource-lifetime.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [XR Architecture](./vr-ar-architecture.md)
- [ADR-137: Terrain and Foliage Ownership, Data, Tier and Lifecycle](../../adr/137-terrain-foliage-ownership-data-tier-and-lifecycle.md)
- [ADR-139: Terrain Render Extraction, Material, LOD and Tier Boundary](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md)
- [ADR-141: Terrain/Foliage Cross-System Ownership and Readiness](../../adr/141-terrain-foliage-cross-system-ownership-and-readiness.md)
