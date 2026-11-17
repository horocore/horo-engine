# XR simulator contract migration — HORO-2207 checkpoint A

XRApi 1.2 adds canonical articulated-hand evidence to the existing tracking snapshot.
`LeftHand`/`RightHand`, `HandJoints` and `HandTracking` append to their respective
closed vocabularies; earlier enum ordinals remain unchanged. The joint vocabulary is
26 anatomical joints per hand, with two hands and at most 52 records. Every populated
record carries a generation-safe hand device, handedness/anatomy, immutable simulation
pose with its own component validity, and an optional finite nonnegative SI radius.
Missing joints are omitted; a controller palm is never an articulated-hand alias.

`XRTrackingSnapshotLimits::maximumHandJoints` and descriptor `handJoints` default to
zero/empty. Existing native head/controller producers keep those defaults and must
not advertise HandTracking/HandJoints. Explicit hand publication requires the accepted
feature plan and prepared storage budget. Exact session, source device, cutoff time,
origin, canonical anatomy order and loss state are checked before atomic publication.
Hand queries recheck current snapshot/session/origin fences. Loss publishes empty
joint evidence, never a prior pose. This raw contract grants no gesture, consent,
recording/export or native hand-provider authority.

Consumers rebuild changed C++ layouts. Real version validators require 1.2 and reject
1.1 providers rather than silently accepting smaller capability tables. Rebuild signed
OpenXR component manifests with contract 1.2; loader API version ranges are independent
and remain unchanged. Existing native providers still report their actual unsupported
hand capability. Scenario/capture schemas will have their own version; they are not
serialized C++ layouts. Owning consumer tests report compiled record/snapshot sizes and
retain the existing 256 KiB snapshot budget; no maximum-view quota is raised.

The non-installed `XRFakeRuntimeHostComposition` owns one existing XRFakeRuntime and
composes XRTrackingPublication against that fake's exact private session lifecycle.
Only its resource adapter sees lifecycle-issued candidates. Candidate tracking binds
at CapabilitiesAndSpaces; failed preparation/exception rollback closes only the
candidate, while successful activation retires prior preparations and promotes the
new publication. Child resource stages release exactly once in reverse order for the
correct generation. A cold trusted child resource adapter must clean its own failed
Prepare before returning or throwing, as in IXRSessionResources. No mutable lifecycle
or forged identity factory is exposed to Editor consumers.

The supplied-step overload of XRFakeRuntime runs through the same consumption,
production session/frame gates, failure injection and cleanup as scripted Step.
It validates before consumption, rejects stale owners and non-increasing prediction
times across both supplied and script admission, and cannot be mixed with a loaded
script without explicitly clearing it. Loading a new script cannot rewind the last
consumed prediction timestamp; rejected replacement leaves cursor/session/frame
sequence unchanged.
Prepared successful Step and tracking publication allocate no memory and perform no
I/O/native/GPU work. Composition joins only evidence with the successful frame's
sample timestamp; failure revokes current tracking instead of reusing stale evidence.

This is an intermediate contract/runtime checkpoint. It does not yet deliver the
Editor Play/Input simulator host, user controls, scenario codec, actual interaction
recorder, runnable samples or final HORO-2207 acceptance. Those remain the next connected
implementation phase. Deterministic fake results never qualify a physical runtime,
headset, GPU, native hand provider, or live-sensitive capture consent.

## Simulator controls and artifact composition

`HoroEngine::XRSimulator` owns `XRSimulatorScenario.h` and
`XRSimulatorRuntime.h`; it composes `XRRuntime` and `XRInputBindings`. The
noninstalled `HoroXRSimulatorHostInterface` stages only
`XRSimulatorHostComposition.h`. A standalone owner has no scalar-action
permission. An application composes its existing `IXRSessionResources` and the
actual `XRActionBindingCoordinator` prepared for the fake owner's genuine
candidate. The simulator copies finite binding metadata during preparation and
checks the same coordinator session/current publication/revision before each
step. Existing coordinator callers need no migration; `Session()` only exposes
its existing immutable owner.

Version-one JSON stores normalized synthetic poses, anatomical joints, controls,
events and relative clock deltas. It never stores live session/device/action
handles. Action ordinals require a canonical semantic-order SHA-256 compatibility
fingerprint computed from the real resolved binding plan. The encoding uses
length-prefixed UTF-8 and little-endian 64-bit enum/count/schema fields; changing
action/context/type/role/profile/control or order rejects replay. This identity
is neither authentication nor Input authorization. Fresh genuine sessions can
replay equivalent semantic plans.

Artifacts have at most 256 steps, 64 scalar controls per step, seven pose sources,
52 hand joints and the existing `XRFrameHardLimits` view ceiling. Parsing rejects
unknown/duplicate fields, unsupported versions, invalid enums/order, nonfinite
values and overflow, with 8 MiB bytes, one million JSON nodes, 16 levels and
128-byte text bounds. The currently admitted simulator projection remains two
views; larger valid artifact view arrays fail before image acquisition. Prepared
step/tracking/recording paths do no file I/O or C++ allocation. Cold creation,
scenario import/export, session replacement and capture arming may allocate
bounded heap storage.

Capture accepts only admitted simulator interaction during execution, including
midrun control edits and consumed events. It cannot consume native snapshots or
grant native capture consent. The manifest fixes purpose to simulator development,
retention to memory until explicit export, and export to synthetic controls only.
Capture has independent step, conservative encoded-byte and logical-duration
bounds (hard maximum 60 seconds). Byte accounting reserves the manifest and
measures encoded steps, not RSS or `sizeof` storage. Exhaustion preserves a
partial result and dropped-step count. Cancellation exports nothing; successful
session replacement finalizes an existing nonempty recording as partial. A
rejected replacement preserves the prior owner and recording. Export retains
complete/partial status, actual elapsed duration and capture limits.

The connected developer workflow is documented in `examples/xr-simulator/README.md`.
Explicit `--xr-simulator` selection composes the same shared `SimulatorPlayHost`
for Editor and the CPU project runner. It creates the fake runtime only during
actual Scene participant preparation, then contributes to the existing Input
collection cutoff and canonical gameplay commands. Core tests, connected
workflow tests, GUI drawing tests and physical renderer qualification remain
distinct evidence; none establishes headset/native runtime/real hand-provider
qualification.


## Application and Editor migration

`XRSimulatorModel.h` is owned by the CPU `EditorXRSimulatorModel` target. Its
weak command port exposes only bounded copied controls/status and genuine
expected-session receipts; panels never retain a Scene-owned runtime or tracking
borrow. `XRSimulatorPanel.h` remains GUI-owned. Optional `PanelContext` simulator
fields are appended so existing positional aggregate consumers retain their
meaning. `WorkspaceReadSources` optionally carries a same-root artifact port;
failed project preparation does not promote it. Leave/destruction revokes it,
and panels lock a weak port for each cold operation.

The noninstalled Play view-source enum appends Simulator and preserves native
SelectedXr semantics by default. Both selections require the same actual factory,
host and Scene participant identity. Simulator preview copies an actual consumed
view after checking current session, Scene source and tracking evidence. Missing
or stale evidence clears the viewport revision and prevents old texture display;
it never selects an authored/editor camera. Native acquired-eye presentation
continues through its existing path.

`HoroEditorDocumentInternal` deliberately stages only `SceneDocument.h` and
`RuntimeSceneConversion.h` as the inbound document seam. The existing
`HoroEditorXrSceneInternal` stages the outbound geometry/tracking-root contracts
plus `EditorSimulatorSceneSource.h`. The primitive-only sample source owns a real
SceneDocument, caches and weak source owner. LoadSaved and canonical conversion
produce its exact document-state definition; the headless runner activates that
one definition, rather than guessing an Editor state revision from a source hash.
Imported mesh/prefab sample inputs are explicitly unsupported instead of omitted.

The headless workflow has one canonical scheduler and Scene service. Its cold
cohort reservation supplies one never-reused Scene occurrence per workflow,
with no Scene reload API; session reset leaves that occurrence unchanged. This
is a reservation for these workflow instances, not a global allocator for
unrelated Scene services. The prepared shared host borrows the actual registered
`gameplay.move` Axis2D action and enters GameplayInputFrameBuilder before the
same Lua/Scene command owner used by existing gameplay. The additive borrowed
Input overload on GameplayWorldComposition preserves default-empty callers.

Project artifact I/O stays in application composition. The existing fresh
workspace owner/generation-one source identity is reused with retained native
source/publication capabilities. Imports are project-relative; exports are
restricted to `.horo/xr-simulator/captures`, encode first, stage exclusively,
and use tracked atomic replacement. A committed receipt remains authoritative
when a later durability operation reports an error. No arbitrary user-path
write, native tracking snapshot, or consent boolean is introduced into the model.


Cold scenario import preserves paused simulator admission. Before another cold
activation (including Replay), the existing Input source must retire through an
actual canonical Input cutoff. Immediate repeated activation rejects with the
original typed `input.collection.stale` cause and preserves the loaded owner;
callers must drive the real host opportunity, then replay. The headless sample
does this explicitly without inventing a context or issuing a second commit.
Head pose source activity follows actual located components, while controller
and hand availability remains independent of tracking validity. This preserves
the canonical Head normalization rule during partial tracking loss.
