# ADR-179: Device Loss and Renderer Restart Lifecycle

- **Status**: Proposed
- **Date**: 2026-09-20
- **Supersedes**: None
- **Scope**: Typed renderer loss detection, frame abortion, device/resource invalidation, incident evidence, host-owned restart and terminal failure
- **Issue**: [RND-003.7](https://github.com/HoroCore/horo-engine/issues/302)
- **Jira**: [HORO-302](https://horo-engine.atlassian.net/browse/HORO-302)
- **Related**: [ADR-027](027-renderer-resource-identity-and-descriptors.md), [ADR-028](028-renderer-capability-limits-and-product-profiles.md), [ADR-033](033-presentation-and-display-ownership.md), [ADR-041](041-backend-neutral-renderer-diagnostics-model.md), [ADR-048](048-gpu-crash-and-device-loss-diagnostic-bundles.md), [ADR-174](174-render-adapter-and-device-discovery-contract.md), [ADR-175](175-render-resource-state-and-barrier-model.md)
- **Normative documents**: [Rendering Architecture](../architecture/runtime/rendering-architecture.md), [Render Backend Parity Contract](../architecture/runtime/render-backend-parity-contract.md), [Runtime Lifecycle](../architecture/runtime/runtime-lifecycle.md)

## Context

The renderer already has separate contracts for backend frame ownership,
surface generations, capability admission, resource identity and bounded
device-loss evidence. Those contracts do not yet define who turns a native loss
fact into a frame abort, how an old device owner becomes invalid, or how an
interactive host returns to a usable renderer. Without one decision, a backend
could retry after loss, a frontend could preserve stale handles, or an editor
could silently switch to another backend. Each choice would break the
generation and ownership rules already established elsewhere.

Device loss is also not the same as a minimized surface, a recoverable frame
failure, a process crash, or a project renderer-setting change. A surface may be
recreated while the device remains valid. A live device loss can often be
recovered in-process, while a corrupted backend or unsafe native teardown may
require process restart. A project setting change is a planned restart and does
not imply that the active renderer has failed.

## Decision

### 1. The host owns the renderer restart state machine

The application host/composition root owns one renderer lifecycle controller.
`RenderFrontend` and concrete backends report typed facts and execute the
owner-thread operations requested by that controller; neither one chooses a
fallback backend, owns editor restart UI, or restarts itself from a native
callback. The controller is the only authority that may replace the active
frontend/backend pair.

The lifecycle has orthogonal identities and state rather than one lossy enum:

```text
RendererGeneration: monotonically increasing host composition identity
DeviceGeneration:   monotonically increasing native device/context identity
SurfaceGeneration:  ADR-033 primary-surface identity
FrameToken:         active-frame identity within one backend instance
```

The controller's externally observable phase is one of:

```text
Ready -> FrameActive -> Ready
Ready -> Recovering -> Ready
FrameActive -> Recovering -> Ready
Ready -> TerminalFailure
FrameActive -> TerminalFailure
Recovering -> TerminalFailure
any non-terminal phase -> ShuttingDown
```

`Recovering` is a host safe-point phase, not permission for callers to submit
work. Device/context loss accepted during an active frame first aborts that frame,
then enters `Recovering`; `BackendFatal` may transition directly to
`TerminalFailure`. A failed recovery enters `TerminalFailure` from `Recovering`.
A new device generation is not published as `Ready` until backend initialization,
capability/profile admission, surface realization, and required resource
reconstruction have committed. The old generation remains frozen and cannot be
made current again.

### 2. Loss is reported through a typed, generation-bound fact

Backends normalize native status into a backend-neutral loss fact. Native status
codes and bounded native evidence remain optional fields owned by the backend
adapter; control flow never parses native message text.

The conceptual contract is:

```cpp
enum class RendererLossKind {
    RecoverableFrame,
    SurfaceRecreationRequired,
    DeviceLost,
    DeviceHung,
    DeviceRemoved,
    BackendFatal,
};

struct RendererLossFact {
    RendererLossKind kind;
    RendererGeneration renderer;
    DeviceGeneration device;
    std::optional<SurfaceGeneration> surface;
    std::optional<FrameToken> frame;
    RendererErrorCode cause;
    std::optional<NativeLossEvidence> nativeEvidence;
};
```

The final representation may use the repository's established error and
identity types, but it must preserve these fields and validity rules:

- the renderer and device generations are non-zero and match the active owner;
- a frame token is present only when loss was observed during that frame;
- a surface generation is present only for a surface-scoped fact;
- `DeviceLost`, `DeviceHung`, and `DeviceRemoved` close the device generation;
- `SurfaceRecreationRequired` does not invalidate the device or its resources;
- `BackendFatal` is terminal for the current backend instance unless the host
  explicitly escalates to process restart.

Loss can originate from `BeginFrame`, `Execute`, `Present`, a bounded backend
poll, or an API callback. Callbacks only enqueue a bounded generation-tagged
fact for the render-capable owner thread. They do not traverse renderer state,
abort a frame, recreate a device, or notify UI directly. Duplicate facts for
the same device generation coalesce into one incident while preserving an
occurrence count and the first authoritative cause.

### 3. The first authoritative loss aborts the frame and closes admission

At the next legal render safe point, the controller accepts the first current
loss fact and performs this ordered transition:

1. close normal frame, resource, query and renderer-diagnostic event admission
   for the old device generation while preserving the pre-admitted ADR-048
   incident-evidence lane;
2. freeze the old generation's frame/graph/resource/surface correlation;
3. abort the active frame exactly once, if one exists; no `Present` is allowed
   after a loss fact;
4. run the ADR-048 live incident safe point while required native parents are
   still available;
5. complete or cancel backend work according to the backend's loss contract and
   mark all later old-generation completions stale;
6. detach or recreate the surface/device only through the host recovery plan.

No normal render, resource creation, upload, readback, query or present call is
allowed after old-generation admission closes. The only permitted native work
after that point is bounded incident evidence and API-required release on the
qualified owner thread. GPU-idle waits, unbounded polling, retry loops and
filesystem work are not part of the loss safe point.

A recoverable frame failure that does not close the device may abort only the
current frame and return the backend to `Ready`. A surface loss follows ADR-033:
the host suspends acquisition and realizes a new surface generation without
invalidating device resources. Device loss always invalidates the device
generation, even if the backend reports that some native objects appear usable.

### 4. Recovery replaces the native owner and invalidates every old handle

The recommended recovery is a bounded in-process renderer restart owned by the
host:

```text
accept loss at safe point
    -> freeze incident evidence and old generation
    -> abort frame and close producers
    -> release API-required old children while the old device is usable
    -> invalidate old registry and abandon unavailable native instances
    -> shut down old backend without calls after native invalidation
    -> rediscover/admit adapter, device, capabilities and product profile
    -> create a fresh backend/frontend and new renderer/device owners
    -> realize the required surface generation
    -> rebuild recoverable resources and derived plans
    -> publish the new generation as Ready
```

The replacement frontend receives a new ADR-027 resource owner identity. Every
old buffer, texture, view, mesh, target, graph import, plan, query, temporal
history, upload operation and frame token becomes stale, regardless of matching
slot numbers or native pointer values. Old completion callbacks are ignored by
generation check. The old resource registry is shut down with
`NativeUnavailable` when the graphics API can no longer be called; it must not
call destruction through an unavailable device.

Reconstruction uses the ADR-027 residency record, not the old handle:

- `RecreateFromAsset` re-resolves the stable asset identity and cooked revision;
- `RecreateFromRetainedCpuData` reuses explicitly retained bytes;
- `RebuildByOwner` asks the owning subsystem to submit a fresh typed request;
- `NonRecoverable` is retired and reported to its owner without an invented
  placeholder.

Reconstruction is transactional per bounded batch. A failed resource does not
publish a partial generation or retarget dependents to an old handle. Required
renderer resources and the primary surface must succeed before `Ready` is
published. Optional resources may remain unavailable only when their existing
capability contract declares that absence non-fatal.

### 5. Capability and plan snapshots are rebuilt, never reused

After device/context recreation the host reruns adapter discovery, driver
compatibility restrictions, validation realization, capability resolution and
product-profile admission under the new device generation. Reported facts from
the old device are evidence only and cannot authorize the new device.

The host discards old compiled render plans, graph execution plans, queue/fence
points, barrier-state evidence, temporal histories and presentation negotiation
results. New plans reference only new owner/generation identities and the new
effective capability revision. A reduced capability result is a typed recovery
outcome; it cannot silently lower required project/profile features or switch
backend.

### 6. Diagnostics are captured before recovery mutates evidence

ADR-048 is the sole incident-evidence policy. The first loss freezes bounded
generation-scoped rings, publishes one incident identity, and invokes each
admitted native fault query at most once within its declared affinity, byte and
time budget. Missing, unsafe, timed-out, partial and unavailable evidence remain
explicit coverage states. Encoding, compression, durable staging, export and
UI happen after teardown on their owning bounded services and never gate safe
renderer recovery.

The incident includes at least the renderer/device/surface generations, loss
kind and registered error code, active frame/graph/queue correlation when
available, capability and validation revisions, recovery attempt identity and
terminal outcome if recovery fails. It does not retain leases that prevent
native retirement, expose native handles in public contracts, or automatically
start graphics capture/upload.

### 7. Terminal failure is explicit and preserves project state

Recovery has a finite attempt budget and one host-owned terminal result. The
host enters `TerminalFailure` when adapter/device admission fails, required
surface realization fails, required reconstruction fails, the backend reports
corruption/unsupported recovery, or the bounded recovery deadline is exceeded.

Terminal failure:

- returns a typed result containing the original loss and recovery cause chain;
- stops frame/resource admission for the failed generation;
- leaves project files, authored scene state and renderer selection untouched;
- exposes diagnostics/repair/restart actions through the host surface when it is
  available; if surface realization itself failed, the host/platform delivers
  the typed terminal result through an independent platform-native notification
  or structured logging path;
- never silently falls back to `RenderNull` in an interactive editor;
- never silently selects another backend unless the host's explicit fallback
  policy named that candidate before the failure.

The headless host returns the typed terminal result to its caller. The graphical
host remains in its bootstrap/recovery route or requests a full process restart
when in-process teardown is not safe. A process restart is an escalation owned
by the host/platform, not a backend retry loop.

### 8. Planned renderer changes use the same ownership boundary

A project or user renderer-setting change is not a loss event. It records a
pending restart request, leaves the active renderer and project state untouched,
and applies the selected stable backend ID during the next startup/restart
sequence after component resolution and bounded probing. The host uses the same
replacement boundary and old-generation invalidation rules, but the cause is
`PlannedRestart` rather than a device incident.

## Consequences

Renderer recovery has one owner, one generation rule and one terminal policy
across OpenGL, Metal, Vulkan and future backends. Stale native work cannot
resurrect resources or publish into a replacement device, and diagnostics retain
useful bounded loss evidence without delaying teardown.

The cost is a new host lifecycle controller, backend-specific loss mapping,
reconstruction-source bookkeeping, bounded recovery UI/CLI states and native
qualification for induced loss where the platform permits it. In-process
recovery is intentionally not promised for every native failure; terminal
failure and process escalation are first-class outcomes.

## Migration And Verification

Implementation proceeds in these ownership slices:

1. `RenderErrorCode`/incident integration defines the typed loss fact and
   generation validation without exposing native status types.
2. Backend modules map native loss and callback facts, stop normal admission,
   and prove no graphics API call occurs after the backend declares its device
   unavailable.
3. `RenderFrontend` and the resource registry expose a safe-point transition
   that aborts frames, cancels pending work, invalidates the old owner and uses
   `NativeUnavailable` when required.
4. The application host composes the replacement frontend, reruns capability
   admission, rebuilds surface/resources/plans and owns terminal failure.
5. Observability composes ADR-048 incident capture; it does not create a second
   renderer-loss evidence path.

Deterministic Null/fake-backend tests must cover duplicate loss coalescing,
loss during `BeginFrame`/`Execute`/`Present`, exactly-once abort, stale and late
completion rejection, old-owner handle invalidation, `NativeUnavailable`
retirement, capability/profile re-admission, recoverable/non-recoverable
resource reconstruction, bounded retry exhaustion, terminal failure and
repeated shutdown. Native OpenGL/Metal/Vulkan lanes must additionally prove
the qualified owner-thread ordering and absence of calls after native device
invalidation; Null tests do not qualify native fault behavior.

## Rejected Alternatives

| Option | Decision and reason |
|---|---|
| Let each backend restart itself | Rejected: a backend cannot own editor/project state, resource reconstruction, capability policy or terminal UI. |
| Let `RenderFrontend` silently recreate the device | Rejected: it hides a host-visible lifecycle transition and encourages stale handles or incompatible plan reuse. |
| Preserve handles whose slots happen to match | Rejected: native identity, capability revision and dependency state may have changed; ADR-027 requires a new owner. |
| Restart the whole process for every loss | Rejected: loses live incident context and unnecessarily disrupts recoverable editor sessions. Process restart remains an explicit escalation. |
| Keep the old frontend alive beside the replacement | Rejected: old callbacks, leases and native references can cross generations and block bounded teardown. |
| Fall back silently to Null or another backend | Rejected: interactive hosts must preserve the selected contract and report actionable failure; only explicit host policy may select a fallback. |
| Retry the failed native call until it succeeds | Rejected: loss is a generation boundary, not a transient API exception; unbounded retry can call an unavailable device and hide terminal failure. |
| Build diagnostics during a crash handler or block recovery on bundle export | Rejected: ADR-048 requires fault-safe tombstones and bounded live evidence; durable encoding is secondary to safe teardown. |
