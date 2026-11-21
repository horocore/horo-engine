# ADR-119: Camera Authority During Cinematics

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Runtime, play-in-editor and editor-viewport camera authority; cinematic cut admission and handoff; rendered-frame validity; tiered transition semantics; backend-neutral render extraction
- **Issue**: [CIN-003.1](https://github.com/HoroCore/horo-engine/issues/1700)
- **Jira**: [HORO-1659](https://horo-engine.atlassian.net/browse/HORO-1659)
- **Related**: [ADR-014](014-sequencer-ownership-clock-authority-and-binding-boundary.md), [ADR-026](026-large-world-precision-and-floating-origin-strategy.md), [ADR-033](033-presentation-and-display-ownership.md), [ADR-040](040-reconstruction-frame-generation-and-latency-providers.md), [ADR-117](117-playback-ownership-frame-order-and-determinism.md)
- **Normative documents**: [Cinematic Sequencer Architecture](../architecture/runtime/cinematic-sequencer-architecture.md), [Rendering Architecture](../architecture/runtime/rendering-architecture.md), [Runtime Lifecycle](../architecture/runtime/runtime-lifecycle.md)

## Context

ADR-014 establishes that Cinematic Runtime submits typed camera requests and that
the camera subsystem retains final active-camera authority. ADR-117 gives concurrent
sequence players a stable priority/identity order and immutable evaluation batches.
The renderer already consumes immutable, backend-neutral camera data during render
extraction.

The remaining boundary is underspecified. A runtime game view, a play-in-editor
(PIE) game view and an editor authoring viewport can coexist, but a process-global
active-camera pointer cannot distinguish them. It is also unclear whether a sequence
owns the camera before its first cut, whether the last cut remains active after the
track ends, what early stop restores, and whether a cut may change between render
passes in one displayed frame.

Camera blending creates another ambiguity. A hard cut can publish one camera, while
a true cross-fade can require two fully rendered views and additional composition
cost. Treating both as one renderer-specific callback would leak backend policy into
the sequencer and make capability fallback unpredictable.

This ADR defines camera authority, handoff, frame validity and transition tiers. It
does not implement the camera service or the camera-cut track; those remain owned by
the focused CIN-003 delivery tickets.

## Decision

### 1. Authority is scoped to a view context

The host creates a generation-checked `CameraViewContextId` for every independently
rendered view. One owner publishes the final camera selection for each live context:

| Execution context | Final selection owner | Permitted proposals |
|---|---|---|
| Runtime game view | Runtime `CameraService` for that game session/view | Gameplay base proposals and admitted cinematic overrides |
| PIE game view | PIE session's isolated `CameraService` | PIE gameplay proposals and PIE cinematic overrides only |
| Editor scene viewport | Editor viewport controller | Editor orbit/fly/focus state and an explicitly admitted editor-preview proposal |

The editor camera is not a scene gameplay camera. Runtime or PIE cinematic players
cannot acquire the editor viewport's authority. A sequencer preview is an editor
request routed through the viewport controller; closing preview resolves the
controller's current authoring-camera state instead of restoring a saved pointer.

Each owner accepts immutable `CameraProposal` values and publishes exactly one
`CameraSelectionSnapshot` per view context and render-extraction boundary. There is
no process-global active camera, mutable camera pointer shared with the renderer, or
authority transferred to Cinematic Runtime.

### 2. Cinematic cuts are scoped override leases

Before activation, Cinematic Runtime resolves every camera binding and asks the
context owner for a generation-scoped `CameraOverrideLease`. The request identifies
the session, scene, view context, player generation, track, target camera, priority,
stable player identity, transition policy and whether the claim is required. It does
not carry a backend handle or native viewport object.

The lease grants permission to propose cuts; it does not make the player the final
selector. For concurrent sequences the Camera owner resolves admitted requests by
descending player priority and then ascending stable player identity, matching
ADR-117. Gameplay and losing cinematic proposals remain live. When the winner
releases, the owner resolves the current eligible proposal; it never restores a
camera pointer captured at acquisition time.

Required binding, context or capability failure prevents activation and unwinds the
lease plan. An optional camera track may remain disabled with a bounded typed
diagnostic. Evaluation order, worker completion or container iteration cannot change
the selected camera.

### 3. Entry and exit handoff are explicit

A camera-cut track owns no effective override before its first admitted cut keyframe.
The runtime/PIE Camera owner continues selecting the current gameplay proposal; the
editor viewport controller continues selecting the authoring camera. At the first
cut boundary the player submits its first candidate and the owner can admit it for
the next camera commit.

Every compiled camera-cut track declares one end policy:

```cpp
enum class CameraCutEndPolicy : uint8_t {
    HoldLastUntilPlayerEnd,
    ReleaseAtTrackEnd
};
```

`HoldLastUntilPlayerEnd` is the default because a camera track can end before the
rest of a sequence. `ReleaseAtTrackEnd` removes that track's proposal at its declared
end boundary. Player completion, stop, cancellation, required binding loss, scene
replacement, context destruction and shutdown always close proposal admission and
release the player's lease at the Camera owner's safe point.

After any release, the owner re-resolves the live proposal set. It normally selects
the current gameplay camera, another cinematic winner or the editor controller's
current authoring camera. It does not resurrect a destroyed camera or overwrite a
gameplay selection that changed during the cinematic. If no valid base proposal
exists, the context enters an explicit `CameraUnavailable`/suspended presentation
state; it never retains unowned mutable state or chooses an arbitrary camera.

### 4. One immutable camera decision covers one rendered frame

The Camera owner drains eligible proposals and commits one selection snapshot after
`VariableUpdate` and before `RenderExtraction`. The snapshot is immutable through
all views, passes, `RenderExecution`, `RenderGui` and `Present` work that consume that
rendered frame. A cut, stop, cancellation or async resolution arriving after this
cutoff is considered at the next corresponding camera commit.

If multiple fixed ticks cross camera keys before one rendered frame, only the final
eligible result at the commit cutoff becomes that frame's camera. Intermediate cuts
remain deterministic timeline history but are not promised a presented frame.
Optional presentation-clock sequences use the same cutoff: they may change the next
selection snapshot, never a camera already being rendered.

The snapshot carries stable view/scene/session generations, camera identity,
selection epoch, current backend-neutral transform and projection/lens values,
transition state and typed discontinuity flags. Render extraction copies or derives
frame-owned values from it. No later pass dereferences the source scene component.

An invalid or destroyed target cannot partially replace the current frame's camera.
The owner keeps the last committed valid snapshot for that already-open frame,
reports `CameraTargetUnavailable`, and applies the track's required/optional failure
policy at the next safe point. Later frames re-resolve current valid proposals; they
do not silently render from a stale entity pointer.

### 5. Transition semantics are capability-tiered

Camera transitions are explicit product capabilities:

| Tier | Contract | Render cost and fallback |
|---|---|---|
| Baseline `HardCut` | Publish one destination camera at the commit boundary with a discontinuity and new selection epoch | Mandatory; one rendered view; no hidden blend |
| Standard `SingleViewBlend` | Camera owner evaluates a bounded curve and publishes one interpolated transform/projection snapshot | One rendered view; admitted only for compatible cameras and finite duration/curve |
| Advanced `DualViewCrossFade` | Publish two immutable view snapshots plus a finite composition weight through an explicit frontend capability | Requires two qualified views, budget and compositor support; absence is a typed denial |

A requested transition declares its minimum mode and fallback policy at cook/
activation. `HardCut` replaces a denied blend directly. `LinearThenHardCut`
allows a denied dual-view request to fall back to linear geometric interpolation
with its original window when the single-view owner and endpoints admit it,
then to a hard cut if interpolation is unavailable. `Fail` returns the exact
transition error before acquiring playback authority. These are authored
policies, and the renderer never substitutes a transition.

`SingleViewBlend` interpolates camera pose and compatible projection/lens parameters
in Camera-owned, backend-neutral code. Incompatible projection models or invalid
parameters return a typed transition error and use only the declared fallback.
`DualViewCrossFade` duplicates only the qualified render views; it does not advance
simulation, sequence time or scene histories twice.

### 6. Rendering consumes decisions but never selects cameras

`RenderExtraction` receives a `CameraSelectionSnapshot` and emits a frame-owned
`RenderCameraSnapshot`/view description. The renderer frontend may validate view
count, extents, resource budgets and declared transition capabilities, but it cannot
choose gameplay versus cinematic authority or query sequence state.

A hard cut or incompatible camera transition increments the selection epoch and
publishes generic discontinuity evidence. Temporal reconstruction, exposure,
motion-history and other consumers decide how their own histories reset or migrate
under their existing contracts. Cinematic Runtime does not call OpenGL, Metal,
Vulkan, D3D12 or provider-specific reset functions.

Native matrices, command buffers, images and blend passes remain private to concrete
render backends. All backends, including Null, consume the same Horo-owned view and
transition descriptions. Backend availability cannot change cut ordering, camera
identity, handoff or fallback policy.

### 7. Camera selection does not imply other domain authority

A camera lease grants no transform-write authority. Animated camera entities still
use the ordinary Scene/Animation/transform owner seams and ADR-026 floating-origin
rules. The Camera owner samples their committed state into its selection snapshot;
the renderer never follows a mutable component.

Camera selection also does not automatically choose the Audio listener, input focus,
player controller, gameplay visibility or network authority. Those owners may consume
the published camera identity through an explicit product policy, but they retain
their own final decisions and lifecycle generations.

### 8. Failures and lifecycle outcomes are stable

Stable outcomes include `CameraContextUnavailable`, `CameraTargetUnavailable`,
`CameraLeaseDenied`, `StaleCameraGeneration`, `ConflictingRequiredCameraClaim`,
`UnsupportedCameraTransition`, `CameraTransitionBudgetExceeded` and
`CameraPresentationSuspended`.

Stop/cancel first closes new proposals, removes the player from a later immutable
camera batch, then releases its lease at the owner safe point. Late binding,
preparation or renderer-capability results carry context/session/scene/player
generations and cannot revive a released override. Context teardown retires snapshots
only after frame consumers finish, following ordinary frames-in-flight lifetime
rules.

### 9. Qualification covers authority, frame order and every tier

Required implementation evidence includes:

- simultaneous runtime, PIE and editor view contexts with independent final owners;
- no cut before the first key, both track-end policies, completion, early stop,
  cancellation, binding loss, scene replacement and viewport-preview closure;
- gameplay camera changes during an override followed by release to the current
  proposal rather than a saved pointer;
- concurrent players with priority/stable-ID arbitration independent of allocation,
  arrival, worker-completion and container order;
- multiple fixed-tick cut crossings before one render, requests immediately before/
  after cutoff, and one immutable camera selection for every pass in a frame;
- destroyed/stale camera targets and context generations with typed failure and no
  stale dereference or arbitrary selection;
- baseline hard cut, compatible/incompatible single-view blend, admitted/denied
  dual-view cross-fade, explicit hard-cut fallback and budget exhaustion;
- selection-epoch/discontinuity propagation to temporal consumers without native
  backend callbacks in Cinematic or Camera APIs; and
- OpenGL, Metal, Vulkan-capable and Null/recording adapters consuming identical
  backend-neutral snapshots, including headless authority tests.

## Consequences

### Positive

- Every independently rendered context has one final, lifecycle-scoped camera owner.
- Cinematic entry and every exit path resolve current live state instead of restoring
  stale pointers.
- A complete rendered frame sees one immutable camera decision.
- Hard cuts, geometric blends and true cross-fades have honest capability and budget
  requirements without backend leakage.

### Costs

- Runtime, PIE and editor preview need separate context generations and proposal sets.
- The Camera owner needs bounded transition state and a frame-boundary commit seam.
- Dual-view cross-fade requires explicit frontend composition, resource admission and
  history qualification beyond the baseline camera-cut implementation.

## Rejected Alternatives

### Let Sequencer write a global active-camera pointer

Rejected because runtime, PIE and editor views can coexist, scene objects have
generations, and rendering needs an immutable per-frame value rather than mutable
cross-owner state.

### Let runtime cinematics take over the editor viewport

Rejected because the editor authoring camera is not a gameplay component. Preview is
an editor-controller proposal scoped to that viewport and cannot transfer authority
to the runtime session.

### Restore the camera that was active when playback began

Rejected because gameplay or another sequence may legitimately change its proposal
during playback, and the saved camera may be destroyed. Release re-resolves live
proposals.

### Apply cuts immediately during render execution

Rejected because passes in one frame could observe different cameras and incompatible
history. Camera selection commits once before render extraction.

### Treat every blend duration as a renderer cross-fade

Rejected because single-view interpolation and two-view composition have different
visual meaning, resource cost and capability requirements. The tier and fallback are
explicit.

### Put backend-specific cut and history-reset hooks in Cinematic Runtime

Rejected because authority and sequencing must remain identical across OpenGL, Metal,
Vulkan, future peers and Null tests. Render consumers receive generic immutable
transition evidence and own their backend-private execution.
