# Scene Animation components and lifecycle (HORO-486)

## Ownership and application composition

Scene stores one optional inert `AnimationAuthoringComponent`. Animation owns
compiled assets, graph instances, cursors and pose publication. Runtime owns the
fixed-step dispatch and commitment fence. Persisted components contain no clocks,
pose handles, jobs, providers or backend objects.

Editor composition captures the actual RuntimeHost dispatch source and one shared,
nonwrapping Animation identity issuer before creating preview, Play and prefab
factories. Each Scene host owns its adapter and cancellation lifetime. The native
executable supplies the same explicit issuer model to configured Headless
composition. The app-private composition bundle carries Network factories and an
optional cold factory for the existing RuntimeLifecycleParticipant contract;
there is no new public Network ABI or second scheduler.

The immutable committed Scene provider routes only exact Skeleton, Clip and Graph
asset types through their original verified HCAF envelopes. Other types retain
the established decoded-payload route. The payload and artifact provider
interfaces keep their existing meanings. Source JSON is tooling-only.

## Persistence and native compatibility

Source schema4 is required only for animated documents. Documents without
Animation retain their minimum schema1/2/3. Older schemas containing Animation,
unknown fields or versions, contradictory source forms, invalid scalar values and
noncanonical or zero IDs fail through the shared strict codec.

The durable component contains `contractVersion`, `component`, `skeleton`,
`source` (kind and asset), nullable `retarget`, `rootMotion` and `startsPlaying`.
Clip and Graph bindings, including paused intent, survive source conversion,
prefab projection and editor history. Runtime retargeting, root motion and direct
additive Clip execution without a base are rejected; persistence does not imply
those native capabilities are implemented. Additive Graph programs retain their
actual compiled reference operands.

Animated cooked Scenes use `horo.scene.runtime.v4`; other Scenes retain exact v3
encoding. Canonical decoding rejects missing or contradictory typed dependencies
and version downgrades. Skeleton, Clip and Graph payloads use the bounded binary
`HANIM001` contract without persisted runtime generations. The ordinary cook
transaction validates canonical outputs, including cache hits, against the same
sealed input snapshot and projects their verified closure into prefab cooking.

Gameplay SDK27 changes native Scene component layouts, appends the Animation
built-in kind, and advances the activation participant contract to 5. Structural
virtual layout remains 4. Rebuild native modules and host together, use the exact
SDK fingerprint, and recook animated Scenes. Old modules must fail admission
before their factory is invoked. See the SDK27 migration guide for author steps.

## Fixed attempts and lifetime

Evaluation prepares candidates; it does not publish poses. All participants must
finish evaluation and prepare successfully, followed by the scheduler's final
cancellation check, before nofail commit. Abort runs in reverse order and retains
the last published pose and unconsumed transition intent. The first successful
pose supplies both interpolation endpoints.

Editor pause does not advance Animation; Step is consumed only by a committed
attempt. Resume supplies one actual fixed delta rather than catching up paused
time. Navigation, Scene retirement and shutdown retain the exact owner through
finalization. Reload creates fresh instance generations and invalidates retired
handles.

Configured Headless worlds run Gameplay, Animation and native Physics in order.
The fixed bridge retains bounded lifetime pins separately from the exact evaluated
record order. Even a partially failing Animation call is recorded for rollback.
Consecutive catch-up ticks reuse the pins; cold destruction is deferred to a safe
phase or shutdown rather than nofail publication. Travel candidates do not become
additional fixed owners before publication. All loading shares the original
absolute deadline and operation cancellation boundary.

Live Animation state has no Save adapter in this ticket. Animated Play capture
therefore returns `RestoreParticipantInvalid` rather than saving only authored
bindings and silently losing the running state.

## Verification scope

Focused coverage exercises strict serialization, actual source/cook/load closure,
Graph and Clip sampling, transitions, entity creation/destruction, reload,
malformed and unsupported inputs, cancellation and shutdown. Production Editor
factories are exercised through Controller and RuntimeHost with explicit Null
Physics. Headless product tests use the canonical native Physics backend; the
additional cold binding-only composition explicitly selects Null Physics to
preserve the sole Jolt process owner. Listen rollback tests use transport
qualification, not authenticated GNS transport acceptance.

These tests do not establish visual, GPU, Windows or broad performance acceptance.
The allocation probe measures owner-thread C++ allocation requests, not every
native allocator or RSS. Build/test receipts and current executable qualification
are recorded separately in the delivery checkpoint.
