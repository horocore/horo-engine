# Hard camera cut runtime integration

The baseline camera cut path uses the existing canonical
`SequenceFrameEvaluationPlan::CameraKeys()` as its authored authority. Keys retain
their exact integer time, track/key identities and `SequenceCameraTargetId`.
`CameraCutActivation` supplies required authored-object bindings, one declared
track end, the end policy and a host-issued camera claim. Activation owns copies
of the admitted key times and generation-checked entity references. It rejects
missing/disabled cameras, duplicate bindings, ambiguous simultaneous keys and
multiple competing camera tracks before acquiring any live player state.

`HoroEngine::CameraRuntime` owns the final camera selection contract. Its public
headers are `CameraService.h` and `CameraErrors.h` under `Horo/Runtime/Camera`.
It depends on Runtime Scene and the backend-neutral Render API. The separate
`HoroEngine::CinematicCameraRuntime` composition target owns
`Horo/Cinematic/CameraCutRuntime.h` and depends on Cinematic Runtime and Camera
Runtime. Cinematic Runtime does not depend on the camera owner or editor. Render
backends receive copied camera values and never select camera authority.

Camera frame hooks now receive `const BorrowedCallbackContext&`, matching the
existing event-stage contract. Replace raw camera-context pointers with
`BorrowedCallbackContext{&owner}` and resolve the exact owner using `Get<Owner>()`;
use `{}` for stateless hooks. Owners retain their object through synchronous frame
dispatch. The adapter borrows itself for this call, accounts committed crossings
for its exact player and rolls back pending crossing state if evaluation fails.

Hosts create one `CameraService` per runtime, PIE or editor-preview view with a
generation-scoped `CameraViewContextId` and a non-reused `SceneRuntimeId`.
`CameraCutActivation::claim` identifies a player uniquely across every session
sharing that view; a player-local ID alone is insufficient when sessions overlap.
Higher priority wins, followed by lower stable claim identity. A scene replacement
requires a new context; a context and its owning services stay at stable addresses
while adapters borrow them. Destroy adapters before either borrowed service.

For packaged runtime composition, activate `CinematicCameraPlayback` using the
real session service, current scene view and final camera owner. Ordinary
play/pause/seek/rate commands use its `Player()` handle. Call `Publish()` after
commands or `Evaluate()` for committed ticks, then call `CameraService::Commit()`
after VariableUpdate and before RenderExtraction. Extract every pass from the
returned owned `CameraSelectionSnapshot`. Repeating a commit for the same frame
returns the same decision; late proposals apply to the next frame. The owner
requires the current gameplay base proposal at each commit. With no eligible base
or override it returns `CameraUnavailable`; it never picks an arbitrary entity.

`Commit(frame, scene, base)` now requires the current `RuntimeSceneView` at that
extraction safe point. `ResolveSceneCamera` stamps `CameraProposal::sceneRevision`
with `scene.StructuralRevision()`. Manually composed editor authoring proposals
must carry that same non-zero revision. Both endpoints of a geometric blend must
come from the same revision. This fence prevents a rebase or other Scene mutation
from pairing new model transforms with older camera values.

An old pending proposal or an already committed frame from another Scene revision
returns `CameraStaleFrame`. Republish from the current Scene and use a new rendered
frame. The owner retains the earlier immutable decision for its original paired
frame consumers. Runtime, PIE and editor callers migrate together to the required
Scene argument; the previous two-argument commit contract is replaced.

The adapter samples the final player position, including direct seek, reverse,
loop and ping-pong traversal. Before the first key it withdraws its proposal. The
default `HoldLastUntilPlayerEnd` retains the final camera until player completion;
`ReleaseAtTrackEnd` withdraws at the declared track end. Stop, cancellation,
completion, binding loss and shutdown release the player's lease. Handoff resolves
the owner's current base or another eligible claim, never a saved camera pointer.
The session service retains terminal players until the host performs its existing
restore/release protocol; camera lease release does not bypass scalar restoration.

Existing four-field `SequenceFrameCameraCutKey` initializers retain hard cuts.
The appended fields declare `transitionTier`, `fallback`, `transitionDuration`
and `transitionCurve`. Hard cuts require zero duration. Blends require a positive
integer duration fitting inside the sequence. Required blends need an earlier
source key and completion by the next key or track end. Unsupported overlapping
windows and missing sources use an authored hard-cut fallback or fail typed
activation before a lease or player is retained. Invalid vocabulary and malformed
numeric durations fail frame-plan validation.
Rebuild consumers of Cinematic Runtime and Camera Runtime together because the
in-process key, crossing-request, proposal and owner layouts and the `Create`
signature have changed. The additional fields make transition intent part of the canonical
immutable key.
No persisted sequence-asset encoding is changed by this frame-plan API extension.
Frame camera hooks receive the same authored transition fields copied from the
canonical key, preserving intent across staging and host adapter boundaries.

`CameraService::Create` accepts the host's `CameraTransitionSupport`. Its default
admits the implemented CPU `SingleViewBlend`; hosts selecting the baseline product
policy pass `HardCutOnly`. Capability admission is independent of backend names.
`SingleViewBlend` evaluates Linear or SmoothStep weight from final player time,
then delegates quaternion slerp, position and compatible projection/lens
interpolation to the camera owner. Both endpoints are resolved from the same
current scene borrow on every publication, including seeks, reverse playback and
origin changes. Published frames retain copied values.

The current one-view composition returns `UnsupportedCameraTransition` for a
required `DualViewCrossFade`. An explicitly authored `HardCut` fallback admits the
destination cut. A denied one-view capability follows the same rule.
Perspective/orthographic mixing returns `IncompatibleCameraTransition` unless
that key explicitly permits a hard cut. The frontend currently has no admitted
dual-view camera compositor or two-view budget contract. Enabling real cross-fade
requires that frontend contract and native qualification before exposing support.

Global playback blend windows remain scalar playback settings. A camera-only plan clears scalar blend windows
before session admission, including looping playback. Mixed scalar plans retain
their original blend windows and required restore evidence; all plans retain their
explicit restore policy. Malformed blend settings still fail typed validation.
Hard cuts and compatible single-view selection perform no allocation and have
fixed bounds of at most 64 keys,
32 simultaneous view claims and a hierarchy depth of 64 entities. Activation may allocate;
scene borrows and callback lifetimes follow the existing runtime plan contract.
Exceptional interpolation/fallback diagnostics use the existing owned error
representation. A finite camera pair whose intermediate pose cannot be represented
returns `IncompatibleCameraTransition` and uses its authored fallback.
`CameraProposal::transitionResolution` records the requested transition or its
admitted hard-cut fallback. Changing the selected outcome increments the final owner's
selection epoch, including a fallback inside one camera key. Repeated publication
of the same outcome retains that epoch and the committed frame stays immutable.

The editor viewport controller owns a distinct preview context and stable-address
session state. Its programmatic Start/Advance/Seek/Stop camera-preview entry points
feed the same adapter into `ExtractEditorViewportScene`; no new UI is introduced.
The authoring camera remains current during preview navigation and supplies the
before-first and released-lease base. Preview admission is camera-only; additional
property/event tracks require their own explicit owner bindings. Entering PIE
releases editor preview authority first. A replaced scene or deleted required target
releases preview and extracts the current authoring camera at the same owner boundary.

Regression coverage exercises real RuntimeScene and CinematicRuntime services,
frame cutoff immutability, deterministic arbitration, exact boundaries, blend-window
independence, seeks/reverse/loops, both handoff policies, missing bindings, deleted
targets, context fencing and shutdown. Controller tests inspect the actual owning
viewport extraction snapshot; a public-header consumer verifies target boundaries.
