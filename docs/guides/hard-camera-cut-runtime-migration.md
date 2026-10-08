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

The adapter samples the final player position, including direct seek, reverse,
loop and ping-pong traversal. Before the first key it withdraws its proposal. The
default `HoldLastUntilPlayerEnd` retains the final camera until player completion;
`ReleaseAtTrackEnd` withdraws at the declared track end. Stop, cancellation,
completion, binding loss and shutdown release the player's lease. Handoff resolves
the owner's current base or another eligible claim, never a saved camera pointer.
The session service retains terminal players until the host performs its existing
restore/release protocol; camera lease release does not bypass scalar restoration.

This target implements hard cuts only. Global playback blend windows do not delay
or interpolate camera selection. A camera-only plan clears scalar blend windows
before session admission, including looping playback. Mixed scalar plans retain
their original blend windows and required restore evidence; all plans retain their
explicit restore policy. Malformed blend settings still fail typed validation.
No SingleViewBlend or DualViewCrossFade capability
is admitted by this baseline path, and no implicit transition fallback is chosen.
Successful selection performs no allocation and has fixed bounds: at most 64 keys,
32 simultaneous view claims and a hierarchy depth of 64 entities. Activation may allocate;
scene borrows and callback lifetimes follow the existing runtime plan contract.

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
