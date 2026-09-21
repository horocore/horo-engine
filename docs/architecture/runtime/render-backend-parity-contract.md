# Render Backend Parity Contract

## Purpose

This document defines the common obligations of every interactive Horo renderer
backend. OpenGL, Metal, Vulkan, and future native backends are equal first-class
implementations of the same engine contracts. No backend is the architectural
base class, compatibility layer, fallback implementation, or privileged path for
another backend.

Implementation status does not change this rule. OpenGL may exist before Metal,
but code shared by the engine and editor must not encode OpenGL lifecycle,
resource, frame, presentation, or GUI assumptions.

Renderer components are independently installable. Discovery, verification,
probing, and no-renderer recovery are defined by
[Renderer Distribution And Availability](./renderer-distribution-and-availability.md).
Device loss, frame abortion, resource invalidation, host-owned restart and
terminal failure are defined by
[ADR-179](../../adr/179-device-loss-and-renderer-restart-lifecycle.md).
The current evidence matrix and objective qualification thresholds are owned by
[Cross-Backend RHI Qualification](./render-backend-qualification.md).

## Non-Negotiable Invariants

- Every interactive backend implements the same required lifecycle and baseline
  rendering behavior.
- A backend target cannot link another concrete backend target.
- The frontend, scene, editor screens, project model, settings, CLI, and MCP code
  cannot branch on concrete backend C++ types.
- Native API and window-system types remain private to concrete backend or
  platform-adapter targets.
- Backend selection occurs before creation of the presentation-capable host
  window.
- Selection uses a stable `RenderBackendId`; it does not use compile-time
  `#if` branches in editor or runtime feature code.
- Optional capabilities may differ, but required baseline behavior may not.
- Capability differences are reported through typed values and never inferred
  from the backend identifier.
- Unsupported or unavailable backends return typed errors. Silent fallback is
  forbidden.
- Installed state does not imply availability, and availability does not imply
  activation.
- Project renderer changes are restart operations until an explicit live-device
  migration contract is approved.

## Equal Target Topology

Concrete backends are sibling targets:

```text
HoroEngine::RenderApi
        |
        +-- HoroEngine::RenderOpenGL
        |
        +-- HoroEngine::RenderMetal
        |
        +-- HoroEngine::RenderVulkan
        |
        +-- HoroEngine::RenderNull
```

These sibling build targets produce independently packageable renderer
components. An installed editor may contain any supported subset; runtime loading
does not change the dependency direction.

[ADR-052](../../adr/052-first-party-renderer-component-scope.md) restricts each
first-party component to one backend identity and its matching private module/
presentation/editor artifacts. Host-neutral contracts, component management,
trust, selection/fallback policy and Null remain outside every interactive
component. Installed, verified, host-supported, runtime-available, selected and
active are separate snapshot dimensions and cannot be collapsed to claim parity.

Editor integrations follow the same rule:

```text
HoroEngine::EditorRenderApi
        |
        +-- HoroEngine::EditorRenderOpenGL
        |
        +-- HoroEngine::EditorRenderMetal
```

Allowed dependency direction:

```text
Editor composition -> selected editor integration -> matching render backend
Render frontend    -> RenderApi
Concrete backend   -> RenderApi + private native dependencies
```

Forbidden dependency direction:

```text
RenderOpenGL -> RenderMetal
RenderMetal  -> RenderOpenGL
RenderApi    -> any concrete backend
EditorRenderApi -> any concrete backend
```

Shared implementation belongs in a backend-neutral target only when its
semantics are genuinely API-independent. OpenGL code is not moved into a shared
target merely so Metal can call it.

The selected editor viewport adapter reports its projection clip-depth range
through the backend-neutral renderer contract. OpenGL reports `[-1, 1]`; Metal
and Vulkan report `[0, 1]`. Viewport presentation, gizmo projection, pointer-ray
construction, and CPU picking consume that value directly. Editor feature code
must not branch on `RenderBackendId` to recover this convention.

Editor viewport adapters also consume the same backend-neutral world-grid line
geometry and render it inside the offscreen viewport target. Grid visibility,
adaptive spacing, depth-test behavior, and mesh occlusion are parity obligations;
an adapter must not replace the world grid with a backend-specific screen-space
effect.

Selected Light influence visualizers follow the same rule. Directional arrows,
Point range geometry, and Spot cone geometry are built once from the typed
world-space Light snapshot and rendered as depth-tested, non-depth-writing lines
by every interactive editor backend. Constant-size Light location markers and
marker hit testing remain in the shared GUI presentation layer because they are
screen-space editor affordances rather than scene geometry.

## Pre-Window Backend Description

The host must know presentation requirements before it creates a window. Every
installed renderer package therefore provides signed immutable manifest metadata
that can be read without loading native code or creating a device, context,
layer, surface, swapchain, queue, or worker thread.

The common value model is:

```cpp
enum class RenderPresentationKind : std::uint8_t {
    None,
    OpenGL,
    Metal,
    Vulkan,
};

struct RenderHostWindowRequirements {
    RenderPresentationKind presentation{RenderPresentationKind::None};
    bool resizable{true};
    bool highPixelDensity{true};
};

struct RenderBackendModuleInfo {
    RenderBackendId id;
    std::string displayName;
    RenderHostWindowRequirements windowRequirements;
    bool supportsInteractivePresentation{false};
};
```

These are backend-neutral policy values. They must not contain `SDL_Window`,
`SDL_GLContext`, `CAMetalLayer`, `MTLDevice`, `VkSurfaceKHR`, `NSWindow`, Win32,
X11, or Wayland types.

`RenderPresentationKind` describes the host surface family required by the
selected module. It does not expose a native surface handle and is not a proxy
for runtime capabilities.

The component service validates and converts package manifest metadata into this
common value model. The Render API does not parse package files or discover
native libraries.

## Startup Sequence

Every interactive host follows one sequence:

```text
Resolve requested RenderBackendId
        |
        v
Resolve installed component record
        |
        v
Verify manifest, signatures, variant, and ABI
        |
        v
Require a current successful availability probe
        |
        v
Create host window from RenderHostWindowRequirements
        |
        v
Load and negotiate the exact verified renderer module
        |
        v
Attach the selected backend's private presentation adapter
        |
        v
Register and seal the selected backend provider
        |
        v
Create RenderFrontend
        |
        v
Initialize matching editor GUI and viewport integration
```

The editor must not create an OpenGL, Metal, or Vulkan window before resolving
its backend selection and availability. Failure at any stage unwinds only
resources acquired by completed stages, in reverse order. Missing, incompatible,
or unhealthy components route to the HoroEditor Welcome/component-manager repair
surface rather than creating a fallback RenderApi graphics window.

## Required Interactive Lifecycle

[ADR-033](../../adr/033-presentation-and-display-ownership.md) defines the separate
surface state and ownership model around this backend lifecycle. Platform/window
events enqueue bounded revisioned requests; they do not call resize or present
directly. Surface generation, display revision, and device identity are distinct.
The existing single-primary-output API must be deliberately extended before
multi-window presentation is advertised; offscreen viewport targets are not
additional native surfaces.

Every interactive backend must obey the existing `IRenderBackend` lifecycle:

```text
Uninitialized
    -> Initialize
Ready
    -> Resize
    -> BeginFrame
FrameActive
    -> Execute zero or more validated plans
    -> Present or Abort
Ready
    -> Shutdown
Uninitialized
```

Required rules:

- `Initialize` is single-owner and rejects overlapping use of one presentation
  attachment.
- Failed initialization leaves no retained device, context, surface, layer,
  queue, or ownership lease.
- `Shutdown` is idempotent and safe after partial initialization.
- Backend destruction performs defensive shutdown without throwing.
- `Resize` rejects zero extents at the backend boundary; the host suspends frame
  submission while its drawable extent is zero.
- `Resize` during an active frame is rejected.
- `BeginFrame` issues a non-zero frame token unique within the backend instance.
- `Execute`, `Present`, and `Abort` reject stale or foreign frame tokens.
- `Present` completes the active frame exactly once.
- `Abort` returns the backend to a reusable ready state.
- Exceptions from native boundaries are translated to typed Horo errors.

OpenGL context behavior and Metal command-buffer behavior are implementation
details beneath these rules.

[ADR-029](../../adr/029-opengl-core-profile-and-platform-policy.md)
defines OpenGL's desktop 4.1 Core admission policy. The selected private adapter
must apply complete context requirements before window creation, and the backend
must validate the actual context and required entry points before publishing
readiness. Its compatibility product role does not allow legacy GL contexts,
weaken parity, or make it another backend's implicit fallback.

## Required Editor Rendering Lifecycle

The editor uses one backend-neutral ordering regardless of the selected API:

```text
Begin backend frame
Begin editor GUI frame
Compile and execute static-mesh viewport and primary-output passes
Encode editor GUI draw data
Present backend frame
```

The editor integration may coordinate private native state required to encode
GUI work, but it cannot expose that state to editor screens, panels, settings,
scene code, or the public Render API.

All interactive backends implement the resource identity, immutable descriptor,
validation, readiness, replacement, and deferred-retirement rules from
[ADR-027](../../adr/027-renderer-resource-identity-and-descriptors.md). A backend
may reject unsupported descriptor values through its typed capability/error
contract; it may not reinterpret them, silently choose a fallback, or weaken
owner and generation validation.

All backends also obey
[ADR-034's memory and residency policy](../../adr/034-gpu-memory-and-residency-ownership.md):
admitted backing-capacity charges, bounded upload/readback work, safe retirement
and typed pressure/failure results. Native budget telemetry and allocator strategy
may differ or be unavailable; logical lifetime and owner allowances cannot.
GUI/private helper allocations are included, not exempted from host accounting.
Null tests inject costs and completion schedules; native budget and fragmentation
qualification still requires each actual backend.

[ADR-124 VFX readback parity](../../adr/124-vfx-gpu-simulation-readback-and-compute-fallback.md)
is a backend-neutral normalized schema/copy/result contract. Each backend reports
effective asynchronous readback formats, alignments, byte/work and pending-result
limits; it privately owns staging, map/cache rules and fences. Equivalent requests
retain logical source-step/schema/generation identity and typed unavailable, degraded,
lost or stale outcomes. Null/fakes validate bounds and delayed publication but cannot
qualify native copy, mapping, synchronization or driver behavior.

[ADR-125 VFX sorting parity](../../adr/125-vfx-transparency-sorting-and-pass-placement.md)
requires the same semantic pass/depth mapping and canonical view-depth/batch/particle
ordering. Backends execute frontend-selected stable bitonic/radix plans with declared
count, scratch, key-layout and synchronization limits; they do not replace the
algorithm, accept native order or submit unsorted alpha. Null/fakes validate plans,
ties, ceilings and delayed timing records, while native ordering/time qualification
still requires each shipped backend.

All backends additionally obey
[ADR-041's renderer diagnostics model](../../adr/041-backend-neutral-renderer-diagnostics-model.md).
Equivalent unsupported, invalid, degraded, lost and recovered conditions use the
same registered Horo codes, severities, subsystem identity and semantic fields.
Native message IDs/text may provide bounded private-adapter evidence, but backend
parity never requires consumers to understand native enums or parse strings.
Backends submit through the host's bounded generation-aware port and cannot own
sinks, retention, UI, fallback policy or per-record data-bus publication. Null
validates schema, ordering, saturation and lifecycle behavior; actual native
callback and emergency-path qualification still requires each backend.

[ADR-042 measurement parity](../../adr/042-cpu-gpu-timestamps-and-pipeline-statistics.md)
is operation-specific and optional unless a product/capture request requires it.
A backend advertises timestamp or pipeline-statistic support only with exact
queue/stage/scope limits, clock description, counter widths and canonical semantic
mapping. It owns native queries through GPU completion and returns delayed
generation-tagged results without normal-frame waits. Unsupported/unqualified
values are unavailable, never zero or approximated.

Equivalent instrumentation plans preserve logical scope and result semantics;
native placement and calibration adapters may differ. Null validates graph/query
budgets, delayed schedules, ordering, invalidation and typed failure with synthetic
fixtures, but cannot establish native clock accuracy, statistic semantics,
instrumentation overhead or performance parity.

[ADR-044 marker parity](../../adr/044-render-markers-and-debug-labels.md) requires
the same registered marker identity, typed correlation, graph placement, balanced
scope semantics, finite plan and object-label metadata contract. Native marker and
label operations are optional effective capabilities with exact queue/context,
nesting, object-class and encoded-text limits; a backend cannot infer them from a
Debug build or silently discard a required mode.

Private adapters may segment a logical scope only when the effective capability
and plan declare equivalent correlation across native command boundaries. They do
not expose pointers/addresses, allocate or intern frame-hot strings, launch capture
tools or turn markers into diagnostics/timestamps. Null validates logical streams,
budgets, generation invalidation and encoding fixtures synthetically but cannot
qualify native-tool visibility or overhead.

[ADR-045 validation parity](../../adr/045-backend-validation-and-debug-layer-integration.md)
requires one backend-neutral startup request, immutable resolved plan,
generation-scoped realized state, typed failure set and ADR-041 message projection.
Activation order remains API-specific and private: OpenGL context intent, Vulkan
instance/device facilities, Metal host launch/environment evidence and D3D12
pre-device debug configuration are not exposed through public native flags.

A backend must distinguish requested, realizable and active validation features.
It may take a disabled retry only for an `Optional` plan with that exact declared
edge and complete rollback; `Required` never silently disables. Validation cannot
select another backend/device, lower baseline requirements or alter render work.
Callbacks and polled sources obey bounded admission, fixed filters, generation and
owner-thread teardown. Null proves shared resolution, mapping, saturation and
lifecycle fixtures but cannot certify a native facility, driver message mapping,
callback thread or validation overhead.

[ADR-046 compatibility parity](../../adr/046-gpu-driver-compatibility-and-workaround-registry.md)
requires every backend to report canonical typed environment identity and apply
the same restrictive-only policy semantics through the frontend. Native driver
version namespaces and private workaround implementations may differ, but generic
code never compares driver strings or branches on vendor/device names.

All matching restrictions compose conservatively and retain stable provenance.
A backend cannot use a rule to grant support, increase limits, weaken alignment,
switch adapter/backend or hide failure. Private routes must preserve public
semantics and be registered/qualified before policy references them. Null validates
matching, conflict, bounds and generation behavior synthetically; affected and
unaffected native hardware lanes qualify each actual rule and route.

[ADR-047 graphics-capture parity](../../adr/047-renderdoc-pix-and-metal-capture-integration.md)
requires one provider capability, request, state, typed failure and restricted
artifact contract across supported backends. RenderDoc, PIX and Metal may expose
different launch, attach and in-process modes; unsupported target/trigger semantics
remain unavailable and never select another tool or approximate a frame.

Adapters invoke native begin/end only on qualified owner contexts and preserve
exact renderer/device/surface/marker generations. Capture cannot change backend,
device, quality, validation or graph policy. Null validates authorization, bounds,
state transitions, stale generations, manifests and failures synthetically but
cannot qualify native artifact readability, frame accuracy, marker visibility or
overhead.

[ADR-048 incident parity](../../adr/048-gpu-crash-and-device-loss-diagnostic-bundles.md)
requires common incident identity, generation, freeze, manifest, coverage, privacy
and bounded lifecycle semantics. Backend-native fault evidence may differ or be
unavailable; absence is explicit and another backend's data is never synthesized.

The old generation freezes before recovery mutation. Each native query executes
once at its qualified owner safe point while required parent objects remain alive,
then releases diagnostics ownership so teardown can proceed. Process crash handlers
perform no backend traversal/query. Null proves shared rings, partial coverage,
interrupted staging and stale-generation behavior but cannot qualify native fault
APIs, dumps, driver accuracy or signal safety.

[ADR-049 inspector parity](../../adr/049-render-graph-and-resource-inspector-ui.md)
requires the same logical pass/resource/use/dependency/lifetime/synchronization
schema, stable Horo identities, immutable pages and typed coverage. Backends may
provide optional normalized realization facts but never expose native handles,
addresses or enums to the application/editor.

Memory, timing, statistics, diagnostics and markers join only through exact source
generation/frame/graph/queue/scope identities. Unavailable or delayed facts remain
explicit; a backend cannot fabricate parity with names, zeros or latest values.
Null validates logical fixtures and all service/UI states but cannot qualify native
barriers, memory observations or measurement accuracy.

[ADR-050 reference-image parity](../../adr/050-cross-backend-reference-image-tests.md)
requires the same case identity, scene/capture/image contract, capability outcome,
typed result and bounded artifact schema across backends. Backend-private adapters
may normalize format, swizzle, pitch, origin and resolve semantics only as declared
by the canonical Horo capture point; they cannot change scene policy, quality,
shader variants or comparison thresholds.

Each backend/platform uses an exact reviewed baseline key while cross-backend
reports separately assert semantic feature/capture invariants. No backend may
substitute another baseline/device/software route, treat unsupported or absent
hardware as pass, retry an image mismatch into success or auto-promote its output.
Null proves orchestration and comparison fixtures but cannot qualify native
pixels, compiler routes, color conversion or driver behavior.

[ADR-051 benchmark parity](../../adr/051-renderer-benchmark-and-regression-gates.md)
requires common workload identity, measurement semantics, environment provenance,
typed outcomes, bounded artifacts and gate math. Native placement/calibration may
differ under ADR-042, but a backend cannot change workload policy, enable cheaper
fallbacks, omit slow frames, fabricate unavailable totals or compare against
another hardware cohort.

Performance parity means each supported backend has explicit qualified results;
it does not require equal frame time across different APIs/devices. Missing or
unstable baselines, invalid environments and absent required hardware remain
unqualified matrix states. Null proves sample/baseline/gate fixtures but cannot
establish native performance or instrumentation overhead.

In particular:

- OpenGL global state is private to the OpenGL integration.
- Metal command buffers, render-pass descriptors, and encoders are private to
  the Metal integration.
- ImGui OpenGL and Metal backend calls are private to matching editor integration
  targets.
- Editor feature code consumes opaque editor image identities, not `GLuint` or
  `MTLTexture*`.
- The static-mesh executor obeys the same target-generation, extent, readiness,
  texture-view, and shutdown contract for every backend.

The integration layer adapts editor GUI work to the selected renderer; it does
not own scene rendering policy or replace `RenderFrontend`.

## Required Baseline Capability

This is an interactive-backend lifecycle/parity requirement, not automatic
qualification for every scene or for the `Baseline` product rendering recipe.
[ADR-028](../../adr/028-renderer-capability-limits-and-product-profiles.md) defines
the distinct product profiles and effective support used for resource/plan
admission. Optional features require both native support and an implemented,
driver-policy-approved backend path; profile names never grant support.

OpenGL and Metal reach parity only when both provide:

- interactive window presentation;
- FIFO presentation mode;
- explicit immediate-mode support or a typed unsupported result;
- primary color output clear/store operations;
- resize and high-DPI drawable extent handling;
- zero-size suspension through the host contract;
- editor GUI rendering;
- offscreen color and depth targets for the editor viewport;
- generic immutable position/normal/UV mesh resources with `uint32_t` indexed
  triangle draws for every core procedural primitive;
- deterministic shutdown and initialization rollback;
- Debug and Release startup validation;
- deterministic GPU readback smoke coverage on supported CI hosts.

A backend may report additional optional capabilities. Optional capability
support does not grant different lifecycle rules or allow editor/runtime code to
special-case its identifier.

## Backend Availability

Built, installed, verified, host-supported, probed, selected, and initialized are
different states. The normative lifecycle is defined by
[Renderer Distribution And Availability](./renderer-distribution-and-availability.md).

Project Settings consumes component-registry snapshots. It may list a known but
not installed renderer and offer installation, but it must not claim runtime
support before verification and probe success. Selecting an unavailable backend
produces a typed install, repair, update, or startup result. The editor does not
silently substitute another backend.

## Parity Test Contract

The same behavioral suite must run against every interactive backend. Tests are
parameterized by backend module identity; backend-specific tests are additional,
not replacements for parity tests.

Required shared cases:

1. module metadata is valid and side-effect free;
2. window requirements are available before native initialization;
3. successful initialize, frame, execute, present, and shutdown;
4. initialization failure rollback;
5. overlapping presentation ownership rejection;
6. invalid and zero extent rejection;
7. resize-during-frame rejection;
8. stale, foreign, and reused frame-token rejection;
9. abort followed by successful frame reuse;
10. viewport initialize, resize, render, selected-instance styling, texture-view, and shutdown;
11. GUI frame encoding and presentation;
12. one-frame editor startup through `--renderer <id>`;
13. GPU smoke output satisfying the same image-level acceptance thresholds;
14. package/module ABI negotiation through the same host adapter contract;
15. startup rejection before window creation when the selected component is not
    available.

Platform-specific tests may verify GL state restoration or Metal resource and
command-buffer lifetime, but those tests do not weaken the shared contract.

### Resident Resource Lifetime Validation

Resident-resource qualification is layered so deterministic headless evidence
guards the shared contract on every host while native lanes prove API-specific
realization and GPU completion. A lane passes only when every applicable row
below passes; skipped native rows are unqualified, not successful.

| Behavior | Null/headless evidence | OpenGL evidence | Metal evidence | Pass threshold |
|---|---|---|---|---|
| Descriptor and capability admission | Frontend and Null resource tests | Fake-dispatch resource tests | Fake-runtime resource tests | Every malformed or unsupported request returns the expected typed code and performs no native creation. |
| Pending creation and publication | Frontend resource tests | Viewport GPU smoke | Viewport GPU smoke | Pending handles never resolve or enter a derived resource; each completed operation publishes exactly one ready generation. |
| Resize and generation replacement | Headless viewport-resource tests | Viewport GPU smoke | Viewport GPU smoke | The active target remains usable until the replacement is ready; the old handle is stale immediately after the atomic swap. |
| Mesh-source replacement | Headless viewport-resource tests | Viewport GPU smoke | Viewport GPU smoke | Existing draws retain the old generation until the replacement mesh is ready; no dependent generation is retargeted in place. |
| Dependency and submission pins | Resource-registry tests | Native API completion rules plus GPU smoke | Command-buffer completion plus GPU smoke | No backend destroy occurs before all dependency and accepted-submission pins drain. |
| Creation failure and rollback | Frontend failure-injection tests | Incomplete-framebuffer rollback test | Fake-runtime failure tests | The operation retains its original typed error, partial native state is released once, and the previous ready generation remains usable. |
| Shutdown | Registry and headless viewport-resource tests | Backend lifecycle and viewport smoke tests | Backend lifecycle and viewport smoke tests | Pending work is cancelled, dependents retire before dependencies, every native instance is released once, and a second shutdown is a no-op. |
| Backend/device loss | Deterministic failure fixtures required by ADR-179 | Required before recovery is advertised | Required before recovery is advertised | The first current loss closes old-generation admission, aborts exactly once, captures bounded evidence, calls no unavailable native API, and publishes recoverable records only under a new owner; terminal failure remains typed and actionable. |

Failures must retain a registered descriptor. Its domain identifies the
frontend, OpenGL, Metal, or Null boundary; its code identifies the operation and
failure class; its summary and remediation hint provide the actionable cause.
Tests compare descriptor fields rather than parsing message text.

### Shared Harness Realization

`tests/support/renderer/RenderBackendContractSuite.h` owns the deterministic
backend lifecycle harness. Each backend supplies only a factory and typed
expectations for its module identity and presentation capability. The harness
then applies the same initialization, frame, execution, presentation, resize,
token, abort, malformed-plan, and repeated-shutdown assertions. Backend-specific
fixtures remain responsible for injected
native failures and realization details; they supplement rather than replace the
shared cases.

| Backend | Shared harness target | Additional deterministic evidence | Native qualification |
|---|---|---|---|
| Null | `HoroRenderBackendRegistryTests` | Registry and generic-resource validation | Not applicable; Null makes no native-support claim. |
| OpenGL | `HoroRenderOpenGLTests` | Fake presentation port, command dispatch, rollback, resource validation | Display-capable OpenGL GPU smoke and editor first-frame lane. |
| Metal | `HoroRenderMetalTests` | Fake presentation port/runtime, rollback, resource validation | macOS Metal GPU smoke and editor first-frame lane. |

Every applicable shared section must pass with zero unexpected skips, crashes,
timeouts, or leaked backend instances. A backend-specific unsupported result is
accepted only where the shared contract marks the capability optional and the
fixture declares that typed outcome. Native qualification still requires every
applicable platform row below; the deterministic harness cannot turn missing
hardware evidence into a pass.

The deterministic resource tests run without a display or GPU and must pass on
every supported CI host. Native GPU smoke tests run only in their documented
hardware/display lanes and pass only when all pixel/readback assertions succeed
with the API debug layer enabled where supported. A crash, timeout, unexpected
skip, leaked native instance, non-zero test exit, or failed assertion fails the
lane. Device-loss recovery remains unqualified until the frontend exposes that
lifecycle; tests must not simulate success by preserving handles from the old
resource owner.

## Build Matrix

[ADR-030](../../adr/030-metal-platform-and-feature-baseline.md) limits the initial
Metal product component to macOS 14.0+ with separately qualified native arm64
Apple7 and x86_64 Mac2 devices. The macOS row below does not certify every Mac,
other Apple operating systems, translated processes, or simulator environments.
Its MSL 2.4/deployment and actual-device admission obligations supplement the
shared lifecycle; they do not waive parity or enable optional features.

These are qualification obligations, not claims that every lane is already
passing. For OpenGL, ADR-029 requires a published OS/architecture/window-system
and GPU/driver matrix backed by actual Core-context and parity/smoke evidence.
X11 and Wayland qualify separately; software GL test results do not qualify
hardware. macOS OpenGL remains conditional on a qualified system 4.1 Core path
and carries the platform deprecation warning without an automatic backend switch.

The intended matrix is:

| Host | OpenGL | Metal | Vulkan | D3D12 | Null |
|---|---|---|---|---|---|
| macOS | Build + parity + GPU smoke | Build + parity + GPU smoke | Portability deferred | Outside initial scope | Build + headless tests |
| Linux | Build + parity; GPU smoke on display-capable CI | Not compiled | Planned: build + parity + GPU smoke per shipped X11/Wayland path | Outside initial scope | Build + headless tests |
| Windows | Build + parity; GPU smoke on display-capable CI | Not compiled | Planned: build + parity + Win32 GPU smoke | Planned: build + parity + hardware GPU smoke | Build + headless tests |

[ADR-032](../../adr/032-d3d12-baseline-and-agility-sdk-policy.md) defines D3D12's
Windows 11 x86_64, FL12_0, SM6.0 and Agility baseline. Its planned lane also
requires packaged executable/runtime compatibility checks. WARP tests do not
qualify hardware, and the M0 decision does not add a D3D12 target to the build.

[ADR-031](../../adr/031-vulkan-loader-platform-and-version-baseline.md) defines
Vulkan's initial Windows 11/Linux x86_64 scope and independent loader, device,
feature, and WSI checks. No Vulkan target is implemented by that M0 decision;
its matrix cells describe future acceptance obligations. Software ICDs and
headless tests cannot qualify the native interactive lanes.

Metal being Apple-only is a host availability constraint, not a lower or higher
architectural rank.

Packaged-artifact lanes additionally verify independent component install,
signature/ABI rejection, probe behavior, and editor startup for every supported
installed subset.

## Project Settings Contract

Project Settings stores only the stable backend identifier and pending restart
state:

```text
render.backend = "opengl"
render.backend = "metal"
```

Settings code does not know SDL flags, native handles, ImGui renderer backends,
or concrete backend classes. A committed backend change:

1. resolves the identifier through the component catalog;
2. installs, repairs, updates, or probes the component when required;
3. persists only the stable backend identifier after the chosen workflow policy
   succeeds, or records an explicit unresolved pending request for offline use;
4. marks renderer restart as required;
5. leaves the active renderer untouched;
6. applies the new selection during the next startup sequence.

Machine-local module paths, trust state, install records, and probe results never
enter project settings.

## Transitional State

The current editor composition has OpenGL and Metal sibling GUI bridges and
static-mesh executors. Both consume the same public `RenderSceneView` and execute
from the frontend frame plan. Only ImGui texture resolution remains app-private;
the public renderer API intentionally exposes no native or GUI texture type.

Migration must not preserve either backend as a privileged composition path.

XR follows the same parity rule under
[ADR-160](../../adr/160-xr-rendering-openxr-compositor-and-renderer-ownership.md).
Every renderer advertised for an XR product tuple supplies a private OpenXR external-
resource bridge and proves equivalent typed format/usage negotiation, image-lease
synchronization, N-view semantics, completion evidence and deferred retirement. Native
handles remain private, and XROpenXR retains acquire/release/end-frame ownership. A
backend may use multipass or native multiview internally, but cannot omit/reorder views
or claim an unqualified greater-than-two configuration.
The common module metadata, window requirements, editor lifecycle, generic mesh
snapshot, target handles, submission contract, and parity test harness are shared.

The current in-process static registration path is also transitional for the
installed product. Development and test hosts may retain explicit static module
registration, but product composition loads only exact verified component paths
and adapts the negotiated module ABI into the in-process registry.

## Acceptance Gate

The switchable editor renderer foundation is complete only when:

```text
OpenGL shared parity suite: PASS
Metal shared parity suite:  PASS
OpenGL GPU smoke:           PASS
Metal GPU smoke:            PASS
--renderer opengl:          PASS
--renderer metal:           PASS
No native public leakage:   PASS
No concrete cross-linking:  PASS
Independent install/remove: PASS
Manifest/signature/ABI:     PASS
Probe/crash isolation:      PASS
No-renderer repair routing: PASS
```

Until this gate is met, backend selection remains an engineering/debug control
and is not exposed as a completed Project Settings feature.
