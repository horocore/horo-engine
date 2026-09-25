# XR Architecture

## Purpose

This document defines Horo Engine's virtual reality, augmented reality, and
mixed-reality architecture. It owns the engine-facing XR contracts for runtime
composition, session lifecycle, views, tracking, input, presentation,
interaction, diagnostics, and qualification.

OpenXR is the first runtime backend. It is not the public engine API, and OpenXR
handles or extension types do not cross Horo-owned subsystem boundaries.

## Current Status

The repository does not yet contain a production OpenXR backend. The contracts
below describe the target architecture. A runtime, headset, or platform is not
supported merely because it appears in a design example or because an OpenXR
loader can discover it. Support requires reproducible qualification evidence.

The backend-neutral `HoroEngine::XRApi` contract is implemented. It owns the
Horo XR contract version, generation-safe system/session/space/view/action/device
identities, immutable fixed-size system capability snapshots, finite hard limits,
pre-dispatch admission, coordinate and time-domain pose evidence, and immutable
bounded tracking snapshots with canonical device/pose queries. Tracking publication
copies only after complete validation; frame-hot queries remain allocation-free and
never reuse lost evidence. The target also owns the `horo.xr` error descriptor contribution. It does
not discover or activate an XR runtime and therefore does not constitute OpenXR,
platform, headset, or product-profile support.

[ADR-157](../../adr/157-xr-ownership-runtime-composition-and-capability-tier.md)
is the normative foundation for XR module ownership, host composition, typed
capability admission, 1.0 profiles, unsupported paths and lifecycle. The sections
below specialize that authority; downstream decisions may add exact schemas and state
machines without moving ownership into Platform, Renderer, Input, Platform Services or
post-processing.

[ADR-158](../../adr/158-openxr-loader-backend-packaging-and-host-composition.md)
specializes the first-party OpenXR target/package boundary, verified loader input,
application composition and platform-specific loader policy. Loader discovery does not
constitute product support, and the backend is not an ambient ExtensionHost plugin.

[ADR-159](../../adr/159-xr-action-tracking-and-input-projection-ownership.md)
specializes native action, tracking snapshot, Input projection, fixed-tick, gesture,
haptic and privacy ownership. Input never polls OpenXR, and presentation-late tracking
never rewrites committed simulation input.

[ADR-160](../../adr/160-xr-rendering-openxr-compositor-and-renderer-ownership.md)
specializes runtime-driven N-view admission, swapchain/external-resource leases, frame
and layer submission, auxiliary targets and XR render-quality plans. XROpenXR owns native
frame/compositor calls; Renderer owns Horo graph/GPU work and completion evidence.

[ADR-161](../../adr/161-xr-interaction-runtime-ui-locomotion-and-accessibility-ownership.md)
specializes the XR evidence-to-intent boundary across Input, Runtime UI, gameplay,
Character, Camera and Accessibility. XR never owns UI focus or authoritative movement,
and baseline comfort semantics are independent of renderer/device tier.

[ADR-162](../../adr/162-mixed-reality-ownership-privacy-and-capability-tier.md)
keeps passthrough, anchors, scene understanding, hit tests and light estimation as
independent post-1.0 families with purpose-bound privacy. Native providers, Horo
snapshots, derived domain realizations and durable storage remain separate owners.

[ADR-163](../../adr/163-xr-tooling-diagnostics-privacy-and-qualification-ownership.md)
separates portable project intent, effective runtime snapshots, editor tooling,
Observability/captures and release evidence. Metrics use finite dimensions, and runtime
compatibility never substitutes for physical-device qualification.

[ADR-172](../../adr/172-immersive-agent-ownership-authoring-mode-and-risk.md)
specializes the optional developer-only immersive Editor AI consumer. XR supplies
validity- and generation-checked poses, rays, gaze and actions as bounded evidence;
it does not interpret agent intent, approve proposals, own editor transactions or
expose the capability in packaged games.

## Ownership

```text
application composition
  selects XR backend, renderer backend, platform host, and project policy
          |
          v
XR runtime frontend
  capabilities, sessions, spaces, frame lifecycle, bounded snapshots
          |
          +------> OpenXR backend
          |          loader, instance, system, session, actions, swapchains
          |
          +------> Renderer
          |          external images, N-view extraction, passes, synchronization
          |
          +------> Input
          |          canonical actions and neutralized device state
          |
          +------> Runtime UI / Interaction / Accessibility
          |          world-space UI, locomotion, comfort, focus
          |
          +------> Editor diagnostics
                     setup, inspection, capture, qualification evidence
```

Required boundaries:

- The composition root selects the concrete XR backend before session and
  presentation resources are created.
- The XR frontend owns engine-visible session, space, frame, capability, and
  device identities.
- The OpenXR backend owns native loader, instance, system, session, action,
  swapchain, composition-layer, and extension state.
- Renderer owns rendering work and GPU resources after a runtime image is
  imported through a typed external-resource contract.
- Input owns canonical game/editor action projection. It does not poll OpenXR.
- Runtime UI and interaction consume XR rays, touches, poses, and actions through
  adapters; they do not become a second XR source of truth.
- The XR runtime owns neither asynchronous reprojection nor platform boundary
  systems such as guardian/chaperone. Those remain runtime/platform behavior.
- Editor panels are query and command clients. They do not own the active XR
  session or directly call native runtime APIs.

The logical target split is `HoroEngine::XRApi`, `HoroEngine::XRRuntime` and
`HoroEngine::XROpenXR`. `XRApi` contains only backend-neutral public values and narrow
interfaces. `XRRuntime` owns Horo-visible runtime/session/frame/space/view/capability
generations and immutable snapshots. `XROpenXR` privately owns native loader, instance,
system, session, action, space, swapchain, layer and extension state. The application
host selects and connects the exact XR, Platform, Renderer and Input tuple before
native/session resources exist.

## OpenXR Packaging And Host Composition

`XROpenXR` is an optional first-party product component. Package/install services
verify one exact backend and loader artifact record for the product target; the
application composition root then supplies that record with the selected Platform,
Renderer, Input, product-profile and diagnostics generations. `XRRuntime` never scans
for the backend, and `ExtensionHost` does not activate it as a general plugin.

Loader source is an explicit product policy: a target uses either a bundled verified
loader or a platform-provided loader controlled by that deployment contract. The
backend never searches the current directory, project paths, `PATH`, arbitrary
environment-provided paths or vendor-runtime libraries as fallback. Development runtime
overrides and API layers require a typed non-shipping policy and mark evidence as
non-release-qualified.

Loader discovery and active-runtime selection cross this boundary as one immutable
`XRLoaderPreflightSnapshot`. The application supplies one stable backend, verified
install record, product profile, exact loader source and owner-issued attempt. The
private adapter supplies bounded redacted loader/runtime/system evidence for that same
attempt. Validation neither probes nor publishes: it rejects malformed or contradictory
evidence, unapproved developer overrides, version drift, cancellation and every failed
discovery layer with distinct stable error identities. Successful evidence is rechecked
against the current attempt and composition identities immediately before native
activation, so replacement and shutdown cannot reuse a retained preflight snapshot.

The layers report distinct typed states:

```text
component installation
  -> backend composition
  -> loader availability
  -> runtime availability
  -> system support
  -> session activation
  -> capability availability
```

Success at one layer does not imply success at the next. Diagnostics preserve the
attempt/revision and redacted failing layer; qualification remains keyed to the complete
product tuple.

## Capability And Identity Model

### Implemented XRApi foundation

`XRContractVersion` is the version of Horo's public semantic contract, never a
native API version. Compatibility requires an equal non-zero major version and a
provided minor version at least as new as the consumer requirement. Patch changes
remain compatible. Producers publish this version in every capability snapshot so
an incompatible contract fails before owner or native state is touched.

`XRSystemId` is scoped to one `XRRuntimeGeneration`; `XRSessionId` is scoped to
one exact system; and `XRSpaceId`, `XRViewId`, `XRActionId`, and `XRDeviceId` are
scoped to one exact session. All are process-local live identities. They are not
serialized, are not native handles or runtime paths, and cannot be rebound by
matching a slot after replacement. Shutdown is represented by an invalid active
owner and rejects all admission before registry access.

`XRCapabilitySnapshot` is a fixed-size owned value. Its private state has no
mutation API and captures one exact system, contract version, non-zero publication
revision, closed capability states, and finite view/space/action/device limits.
The hard ceilings are public compile-time storage/work bounds, not product support
claims. `AdmitXRCapability` is a side-effect-free pre-dispatch check: it validates
the active owner and exact revision, preserves unsupported/unavailable/incompatible
categories, and rejects over-capacity requests without allocation, blocking I/O,
job submission, native calls, or CPU/GPU synchronization.

`XRTrackingSnapshot` is an immutable owner-backed publication with explicit session,
snapshot revision, sequence, runtime sample time, world-origin revision, focus,
visibility, aggregate tracking state, and admitted device/pose bounds. Devices and
poses are generation-safe and canonically ordered. Pose validity and confidence come
from `XRPoseSample`; tracking loss removes pose evidence instead of preserving a
last-known value. `QueryXRTrackedPose` performs bounded binary lookup without
allocation and distinguishes unsupported pose purposes, temporary unavailability,
device replacement, snapshot replacement, shutdown, and origin replacement.

`XRViewRenderPlan` owns the implemented bounded runtime-ordered view/configuration and
external-target contract. It uses one representation for mono simulator, primary stereo,
and future N-view evidence; the first production admission remains exact opaque primary
stereo. View poses retain presentation-prediction time, while color/depth targets carry
distinct generation-safe runtime image identity plus Horo RenderApi format, usage,
extent, rectangle, array-layer, and sample requirements. Creation and pre-Renderer
revalidation are allocation-free and fence session, configuration, origin, and current
acquired-image generations without exposing native handles.

The descriptor set returned by `XRErrors::Descriptors()` is the complete bounded
`horo.xr` contribution for later host module registration. XRRuntime and XROpenXR
must extend this Horo-owned vocabulary through their owning module descriptors;
they must not expose native result integers or branch on native message text.

Capabilities are discovered from the selected runtime, platform host, renderer,
device, permissions, and admitted optional extensions. They are not inferred
from a headset product name.

```cpp
struct XRRuntimeIdentity {
    XRRuntimeId runtime;
    SemanticVersion runtimeVersion;
    XRBackendVersion backendContract;
};

struct XRCapabilitySnapshot {
    XRCapabilityRevision revision;
    XRSessionGeneration sessionGeneration;
    std::span<const XRViewConfigurationDescriptor> viewConfigurations;
    std::span<const XRInteractionProfileDescriptor> interactionProfiles;
    std::span<const XRExtensionCapability> extensions;
    XRTrackingCapabilities tracking;
    XRPresentationCapabilities presentation;
    XRMixedRealityCapabilities mixedReality;
};
```

Snapshots are owner-backed, immutable for their documented lifetime, and
bounded. A capability can be `Unavailable`, `Available`, `PermissionRequired`,
`Denied`, `TemporarilyUnavailable`, or `Lost`. Boolean feature flags are not
sufficient for permission-sensitive or replaceable capabilities.

Runtime discovery, project policy, feature admission, activation, and current
availability are distinct states. Horo never silently selects another runtime,
renderer, view configuration, interaction profile, or privacy-sensitive feature.

### Version 1 Product Profiles

`XRProjection1_0` requires primary opaque stereo projection, runtime-driven views,
validity-aware head pose, local/view spaces, predicted-frame sequencing, Renderer
external color targets, canonical action projection and complete focus/session/loss/
shutdown behavior. `XRTrackedInteraction1_0` adds two tracked controller roles with aim/
grip poses, product-declared select/menu/trigger/squeeze/thumbstick actions, cancellable
haptics and Runtime UI/comfort adapters.

These are named requirement sets, not ordered quality/headset tiers. A product requiring
tracked interaction fails preflight when the profile is incomplete; it never silently
falls back to projection-only. Depth submission, fixed non-gaze foveation, refresh-rate
selection, visibility masks, controller presentation and qualified Android standalone
hosting are optional 1.0 capabilities with explicit plan identities.

Passthrough, anchors, scene understanding, eye/hand/body/face tracking, gaze foveation,
quad/multi-view, space warp and vendor-specific SDK features are post-1.0 unless a later
roadmap revision explicitly promotes and qualifies them. Version-1 profiles neither
depend on nor emulate them.

## OpenXR Backend Lifecycle

The backend follows explicit state transitions:

```text
Unconfigured
  -> LoaderAvailable
  -> InstanceCreated
  -> SystemSelected
  -> SessionCreated
  -> Ready
  -> Running
  -> Stopping
  -> Idle / Lost
  -> Destroyed
```

Runtime events drive session transitions. Duplicate, delayed, focus-loss,
visibility-loss, instance-loss, and session-loss events are normal inputs. Every
session-owned handle carries a generation so stale actions, spaces, swapchain
images, and snapshots fail before native use.

Frame submission follows the runtime's predicted timing contract:

```text
poll runtime events
  -> wait frame
  -> begin frame
  -> locate admitted views/spaces at predicted display time
  -> acquire and wait for required images
  -> render admitted views
  -> release images
  -> submit composition layers / end frame
```

Cancellation and shutdown finish or abandon the current frame only through
runtime-valid transitions. The host stops producers before destroying session,
instance, renderer, and platform dependencies.

Before this native lifecycle begins, application composition validates an inert backend
plan, Platform and Renderer prerequisites and the verified loader input. Ready publishes
only after loader, runtime/system, capability, native resource and adapter preparation
all succeed. Failure rolls candidate resources back in reverse order. Shutdown retires
Input/UI projections and Renderer external images before native session/instance and
loader dispatch, and Platform library/activity leases outlive all native users.

## Extension Negotiation And Graceful Degradation

Optional OpenXR features use a typed negotiation record:

```cpp
struct XRExtensionAdmission {
    XRExtensionId id;
    XRExtensionVersion discoveredVersion;
    XRExtensionAdmissionState state;
    std::span<const XRExtensionId> dependencies;
    std::optional<XRCoreVersion> promotedToCore;
    XRCapabilityProviderId provider;
};
```

The backend records discovered, requested, enabled, rejected, dependency-missing,
and promoted-to-core states. Providers are registered only after validation and
are invalidated when their backend/session generation ends. Unsupported optional
extensions disable the associated capability with an actionable reason; they do
not prevent a baseline session unless the active project capability profile
requires them.

## Views And Coordinate Spaces

Horo uses metres, a documented handedness, and explicit local/stage/view space
identities. Poses include validity, tracking confidence, timestamp, source space,
and session generation. Invalid orientation or position components are not
replaced with fabricated identity values.

View count is runtime-driven. Public and renderer-facing contracts must not use
fixed two-element eye arrays:

```cpp
struct XRViewDescriptor {
    XRViewId id;
    XRViewRole role;
    XRPose pose;
    XRProjection projection;
    Extent2D recommendedExtent;
    XRSwapchainTargetId colorTarget;
    std::optional<XRSwapchainTargetId> depthTarget;
};

struct XRLocatedViewSet {
    XRViewConfigurationId configuration;
    XRSessionGeneration sessionGeneration;
    XRTime predictedDisplayTime;
    std::span<const XRViewDescriptor> views;
};
```

The contract supports a bounded variable number of views. The first production
slice may admit only primary stereo. A runtime configuration with more views is
then reported as unsupported before image acquisition; it is never truncated to
two views. Quad-view and foveated-inset execution can be added later without a
public contract migration.

Reference-space changes, recentering, bounds changes, and world-origin rebasing
are translated through an explicit adapter. Runtime tracking state never mutates
authoritative gameplay transforms from a native callback.

## Rendering And Presentation

The OpenXR backend supplies view descriptors and generation-safe runtime-owned
image identities. Renderer imports those images into the render graph with
declared format, usage, extent, array/view index, synchronization, and lifetime.
Native image handles remain private to the concrete XR/renderer adapter.

The initial implementation may render stereo as multi-pass or instanced
multiview according to backend capability. Instanced rendering is an
optimization, not a correctness requirement and not a claim that draw calls are
always halved.

The external-target contract anticipates:

- runtime-selected color and depth formats;
- per-view or array-backed image layouts;
- variable render extents and dynamic-resolution history invalidation;
- fixed foveation, variable-rate shading, and density-map capabilities;
- optional gaze-driven foveation after consent and valid eye tracking;
- motion/depth/timing inputs required by admitted runtime space-warp features;
- frames in flight, deferred destruction, loss, and swapchain recreation.

Foveation is a renderer/backend capability, not a generic post-process effect.
Horo does not emulate runtime-owned asynchronous timewarp or reprojection. When
an optional feature is unavailable, ordinary projection-layer rendering remains
the explicit fallback.

Normal frames do not use device-idle waits. Image acquire, wait, import, render,
release, and composition submission maintain explicit ownership and
synchronization. Session or surface loss retires resources only after queued GPU
references are safe.

The first production `XRProjection1_0` renderer admits exactly one primary opaque
stereo configuration with two runtime views. The public plan remains bounded N-view.
One-view inputs require an explicit simulator/test profile, while any greater-than-two
configuration remains discovered-but-unsupported until fully implemented and qualified;
it fails before image acquisition and is never truncated.

An XR swapchain image and its imported `RenderResourceId` are correlated but distinct
identities. XROpenXR retains allocation/acquire/release/destruction ownership; Renderer
borrows an external lease with typed format, extent, render rectangle, subresource,
usage, color/depth/motion/density role, synchronization and generations. Release waits
for Renderer completion evidence, not merely CPU command recording, without a normal-
frame device-idle wait.

XRRuntime validates one immutable frame/layer intent. XROpenXR performs wait/begin,
view location, acquire/wait, native layer encoding and end-frame. Renderer performs
N-view extraction, graph execution and GPU retirement. Required layer/view failure is
typed; a partial view set is never successful presentation.

Dynamic resolution and fixed/gaze foveation use one immutable XR/Renderer plan that
separates allocation extent, active render rectangle, scale, VRS/density mechanism,
privacy state and fallback. Runtime space-warp depth/motion inputs remain post-1.0 until
their exact semantics are admitted. Runtime asynchronous reprojection and guardian/
chaperone composition never become Horo passes.

## Tracking, Actions, And Haptics

OpenXR action sets and suggested bindings are backend data. Engine systems
consume canonical Horo actions and bounded tracking snapshots:

```cpp
struct XRTrackedPose {
    XRDeviceId device;
    XRPoseKind kind;
    WorldTransform pose;
    LinearVelocity linearVelocity;
    AngularVelocity angularVelocity;
    XRTrackingValidity validity;
    XRTime sampleTime;
};

struct XRHandState {
    XRHand hand;
    XRTrackingValidity validity;
    std::span<const XRHandJoint> joints;
};
```

Frame-hot hand data uses fixed-capacity or owner-backed storage; it does not
allocate a `std::vector` per hand per frame.

Suggested-binding data has its own schema version. Resolution records the active
interaction profile, profile changes, generic fallback, conflicts, and user
override migration. A runtime path such as an OpenXR component path never
becomes a localized UI label or a public gameplay identifier.

Focus loss, tracking loss, disconnect, profile replacement, permission
revocation, and session replacement neutralize affected actions immediately.
Haptic requests have explicit owner, duration, cancellation, device generation,
and capability fallback.

Application/project policy declares canonical actions and XR profile requirements;
Input owns semantic action IDs, contexts, player assignment, user overrides and
consumption. XROpenXR compiles the admitted plan into native action sets/actions/spaces,
synchronizes and locates them, and keeps native paths private. XRRuntime publishes one
bounded generation-scoped snapshot, and the host-composed XR/Input adapter joins it to
the normal Input snapshot transaction before the cutoff.

Rendering-time view/head poses are a distinct predicted-frame snapshot. They may reduce
visual latency but cannot overwrite the Input snapshot, a tick-assigned gameplay frame,
recording or network command. Fixed simulation receives only immutable Horo values tied
to an exact tick, player/input-user assignment and source sample; it never queries live
XR state.

Gesture recognition is an explicit host-composed derived provider. It consumes admitted
immutable joints/poses/gaze, publishes bounded candidates before Input commit and owns
no focus, routing, gameplay meaning or scene mutation. Haptic admission/cancellation is
owned by the Input/application coordinator; XROpenXR owns native apply/stop and reports
submission separately from physical completion.

## Interaction, Runtime UI, And Comfort

XR interaction sources publish bounded generation-scoped ray/direct/proximity evidence.
Host adapters may combine it with read-only Physics/Scene queries, but a hit remains a
candidate. Runtime UI alone resolves its last-presented hit tree, hover, focus, capture
and semantic action; gameplay alone validates use/grab intent; Character alone commits
collision-root movement. Source or target loss neutralizes capture/intents rather than
reusing the last ray/hit.

World-space canvases remain ordinary Runtime UI owner scopes with the same layout,
localization, semantic accessibility and action-command model. Renderer owns per-view
projection/depth/occlusion/pixels. A native composition layer does not become a second UI
tree or bypass UI focus.

Locomotion produces tick-assigned gameplay intent. Continuous/snap movement and teleport
reach Character through typed commands; moving the camera or recentering tracking space
is not a collision-safe teleport. Camera/view composition combines committed Character
root, calibration and tracked head pose without acquiring movement authority.

When artificial locomotion/rotation exists, disable controls where gameplay permits,
snap-turn configuration, speed/handedness, seated/standing/recenter behavior, motion-
reduction controls and declared teleport/continuous availability remain visible across
renderer/device tiers. Missing authored alternatives are explicit limitations, never
silent fallbacks.

Refresh rate, view extent and frame budget come from runtime/device capability
snapshots; architecture does not prescribe a universal 90 Hz minimum. Each qualified
tuple records its supported modes and measured comfort/performance evidence.

## Mixed Reality And Privacy

Passthrough, anchors, plane/mesh understanding, hit testing, and light estimation
are optional capabilities with explicit consent, permission, lifetime, and
failure states.

- Raw camera frames do not enter ordinary engine logs, captures, or public APIs.
- Eye gaze, continuous poses, environment geometry, voice, and spatial anchors
  are privacy-sensitive diagnostic inputs.
- Articulated hand joints and eye gaze require product policy, runtime capability,
  applicable OS permission and purpose-bound consent. Discovery or permission alone is
  insufficient.
- Raw joints/gaze and continuous pose history are excluded from ordinary logs, crash
  dumps, metrics, replay, analytics, AI context and support bundles.
- Permission revocation removes derived capability state and invalidates
  outstanding work.
- Persistent anchors identify localization state and failure; they do not imply
  that every runtime supports cross-session restore.
- Renderer consumes admitted passthrough/composition descriptors without owning
  camera permission or environment understanding.

There is no aggregate MR-enabled tier. Passthrough, spatial anchors, scene understanding,
environment hit tests and light estimation have independent capability/access/lifecycle
states and fallbacks. `XRProjection1_0` and `XRTrackedInteraction1_0` require none of
them.

Baseline passthrough is a runtime compositor-layer intent and exposes no raw camera
frames. Live anchors use Horo generation-safe localization identity; durable restore is
a separate capability with opaque protected provider references. Plane/mesh snapshots
are bounded observed evidence and never mutate authored Scene, Physics or Navigation
without an explicit destination-owned adapter and transaction. Hit tests cannot invoke
UI/gameplay/editor actions, and light estimates cannot rewrite authored lights/exposure.

## Android And Standalone XR

Standalone Android-class headsets require the Android runtime host in addition
to XR implementation. Android activity lifecycle, native surfaces, permissions,
storage, ABI packaging, deployment, thermal state, and device diagnostics are a
parallel critical path.

```text
Android runtime host --------+
                             +--> standalone device qualification
XR runtime/render/input -----+
```

PC streaming or tethered evidence does not qualify the standalone path. Each
connection mode is a separate qualification tuple.

## Immersive AI Authoring Boundary

Immersive AI authoring is owned by the Editor AI system, not XR. XR contributes
timestamped poses, rays, selections, interaction events, and session identity.
Audio contributes an admitted timestamped capture stream. A provider-neutral
speech service contributes transcript hypotheses. AIA aligns this evidence and
invokes approved MCP/editor capabilities.

Editor-owned authoring proxies and real gameplay objects may both provide
spatial evidence. AIA does not own grabbing, throwing, contact generation,
physics, or the gameplay meaning of those objects.

Voice, gaze, pointing, contact, or a final transcript never constitutes
approval. Agent-proposed scene mutations require a visible preview and an
explicit approval bound to the proposal/document revision. Application uses the
editor command/transaction owner so a successful proposal is one coherent undo
entry and failures cannot partially mutate the scene.

## Diagnostics And Qualification

Setup and diagnostics consume the same typed runtime snapshots as production
code. They show loader/runtime discovery, system/session state, view
configuration, interaction profile, optional extensions, permissions,
swapchains, frame timing, tracking confidence, and redacted failure details.

Setup view models show requested, effective, availability/reason and evidence level as
separate fields. Editors validate drafts and invoke application commands; they never call
native XR or persist local discovery/evidence into project settings. Metrics admit only
finite dimensions such as registered backend/profile/phase/outcome classes—never device
names, serials, native strings/paths, live IDs, poses, gaze or environment geometry.

Detailed capture is an explicit bounded operation with channels, duration/frame/byte
limits, privacy purpose, retention/export and a manifest recording completeness,
truncation and drops. Sensitive channels are unavailable unless separately admitted;
development build type is not consent.

Support evidence is keyed by the full tuple:

```text
headset model
+ runtime and version
+ operating system/platform host
+ renderer backend and driver/device class
+ connection mode
+ interaction profile
+ admitted extension/capability set
```

Qualification categories describe planning intent rather than product-name
guarantees:

| Category | Meaning |
|---|---|
| Reference | Primary regularly tested development tuple |
| Compatibility | Additional or constrained tuple tested for compatibility |
| Enterprise | Hardware/access-dependent professional target |
| Conditional | Requires an external runtime, licensed SDK, or platform target |
| Future | Deliberately outside the current release commitment |

Every result is `Passed`, `PassedWithLimitations`, `Failed`, `Blocked`, or
`NotRun`, with evidence provenance and known issues. Simulator and replay results
are never reported as physical-device qualification. Refresh rates and render
extents are recorded from runtime enumeration, not a static headset table.

## Performance, Concurrency, And Shutdown

- Frame-hot snapshots use bounded owner-backed storage and avoid heap allocation,
  blocking I/O, logging-string formatting, and contended locks.
- Runtime event polling and frame submission occur on their declared owner
  threads. Native callbacks publish bounded state and do not mutate scene data.
- Diagnostics observe revisions and snapshots at their own cadence; they never
  stall frame production.
- Extension providers, action/profile state, spaces, images, and captures are
  invalidated by session generation.
- Shutdown stops new frame/input work, cancels owned jobs, drains valid runtime
  transitions, retires GPU resources, and destroys dependencies in reverse order.

## Validation

Required automated coverage includes:

- loader absent, runtime unavailable, unsupported system, and permission denial;
- session lifecycle, focus/visibility loss, instance/session loss and recovery;
- one-view, stereo, unsupported greater-than-two, and recorded variable-view data;
- extension dependency, promoted-to-core, rejection and provider invalidation;
- action/profile changes, tracking loss, hand-joint bounds, and haptic cancel;
- image acquire/wait/import/release, resize, loss and frames-in-flight teardown;
- fixed foveation/dynamic resolution fallback and history invalidation;
- deterministic simulator, capture/replay and fault injection;
- sensitive-data redaction and diagnostic bundle policy.

Physical-device evidence is required for every declared supported tuple. Missing
hardware is reported as `NotRun`; deterministic fakes do not silently satisfy a
device release gate.

## Related Documents

- [XR Ownership, Runtime Composition and Capability Tier](../../adr/157-xr-ownership-runtime-composition-and-capability-tier.md)
- [OpenXR Loader, Backend Packaging and Host Composition](../../adr/158-openxr-loader-backend-packaging-and-host-composition.md)
- [XR Action, Tracking and Input-Projection Ownership](../../adr/159-xr-action-tracking-and-input-projection-ownership.md)
- [XR Rendering, OpenXR Compositor and Renderer Ownership](../../adr/160-xr-rendering-openxr-compositor-and-renderer-ownership.md)
- [XR Interaction, Runtime UI, Locomotion and Accessibility Ownership](../../adr/161-xr-interaction-runtime-ui-locomotion-and-accessibility-ownership.md)
- [Mixed-Reality Ownership, Privacy and Capability Tier](../../adr/162-mixed-reality-ownership-privacy-and-capability-tier.md)
- [XR Tooling, Diagnostics, Privacy and Qualification Ownership](../../adr/163-xr-tooling-diagnostics-privacy-and-qualification-ownership.md)
- [XR Setup UI Reference](../../../mock-studio/designs.md#architecture-runtime-xr-setup)
- [Rendering Architecture](./rendering-architecture.md)
- [Render Backend Parity Contract](./render-backend-parity-contract.md)
- [Input Architecture](./input-architecture.md)
- [Physics Architecture](./physics-architecture.md)
- [Game UI And HUD](./game-ui-and-hud.md)
- [Accessibility Architecture](./accessibility-architecture.md)
- [Audio Architecture](./audio-architecture.md)
- [Runtime Lifecycle](./runtime-lifecycle.md)
- [Platform Abstraction](../foundation/platform-abstraction.md)
- [Android Platform Host](../foundation/android-platform-host.md)
- [Editor AI Agent Architecture](../editor/editor-ai-agent-architecture.md)
- [Application Security](../security/application-security.md)
