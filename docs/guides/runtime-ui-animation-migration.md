# Runtime UI clock and property animation migration

The in-progress HORO-730 / #730 / [RUI-004.5] change implements the executable basis defined by ADR-077. The ADR retains its existing Proposed metadata. The shipping aggregate RuntimeUiService is not present on the base revision; this implementation composes actual typed backend owners and an explicit application adapter.

## Presentation source evidence

`FrameContext` now includes a scheduler-owned presentation clock baseline generation and explicit Initial, Continuous or BaselineReset continuity. The source identity is scoped to the actual RuntimeHost/participant lifetime. It is not a serialized or globally unique clock identity. Consumers must not infer reset/resume from zero delta, from gameplay pause, or from elapsed wall time.

`FrameScheduler::ResetClock` keeps its void/noexcept signature. It resets only presentation sampling, advances its never-wrapping baseline identity, and preserves committed simulation tick evidence and accumulator state. Exhaustion is latched: the next RunFrame fails with `runtime.scheduler.presentation_clock_generation_exhausted` before dispatch. The directly qualified maximum seam is the same non-installed checked baseline helper used by ResetClock and RunFrame; ordinary host Suspend/Resume is covered through real RuntimeHost integration. No public configuration seed exists for tests.

`FrameContext::presentationAdmittedDuration` is the scheduler's cumulative normalized duration admitted before callbacks.
It survives failed phases and `ResetClock`; suspension admits zero and resume excludes suspended wall time. An animation
candidate subtracts only its last successfully published cursor, never just the latest variable delta. Checked duration
exhaustion preserves the last total, latches admission failure and prevents subsequent dispatch. The maximum boundary
is qualified through the same private helper; ordinary failure and Suspend/Resume flows use the real producer.

`RuntimeHost` treats participant errors as fatal and shuts down; it cannot retry `RunFrame` afterward. Lower-level
`FrameScheduler`/started `RuntimeLifecycle` composition may explicitly retain lifecycle admission after a failed dispatch.
Its retry fence tests prove that boundary without changing the default host policy. The animation adapter must retire
on fatal host shutdown while recoverable UI-candidate rejection preserves its aggregate and unread source cursor.

Existing aggregate FrameContext callers retain defaults for the additive fields. Production UI animation must consume the actual scheduler contexts through the explicit adapter, not construct purported commitment/reset evidence. A fixed attempt is not a committed tick. Simulation UI advances only after the actual completedSimulationTick matches the staged fixed sequence and durations. Presentation reset never resets or manufactures simulation elapsed time.

## Namespace issuer proof

UiElementSlotAllocator retains its precondition: the application creates exactly one allocator per actual UiOwnershipGeneration. The allocator now creates one immutable private authority pin at load time; allocation failure returns typed capacity failure. Moves transfer that authority. Tree slot ranges retain the pin without borrowing the allocator ledger.

UiElementTree::WasIssuedBy validates the actual issuer, rather than comparing only numerical ownership/range values. Another allocator with the same caller-provided ownership representation cannot authorize animation ranges for that tree. The check does not invent a global uniqueness guarantee or turn a heap address into a raw/serialized handle. Host ownership generations, instance/canvas identities, actual burned slot ranges and never-wrapping slot generations still fence lookup.

The animation owner reserves and burns disjoint clock/timeline slots from that same actual allocator. Failed preparation does not return issued slots to the ledger. The allocator may move or retire after creation; immutable authority pins survive until the corresponding owner/read leases retire. Frame evaluation and pin lookup never allocate replacement authority or access a shorter-lived allocator borrow.

## Runtime UI domains

Simulation, PresentationUnscaled, ScreenTransition, EditorPreview, DeterministicTest and Manual are distinct closed domains. Runtime UI owns checked integer/rational cursor accumulation, local rate/direction, and generation-scoped outcomes. The host owns fixed commitments and normalized presentation evidence. Rendering reads immutable published values and cannot advance a clock or emit completion.

Preview/test/manual capabilities require explicit composition; shipping host samples cannot be fabricated by ordinary public callers. The stack-borrowed UiAnimationHostRead is constructible only by the actual application adapter and cannot be copied, moved or retained across its callback.

The owner/track/style/layout contracts and affected consumers are described below. Final compilation and regression validation are pending; no current-source result is asserted.

## Prepared style and layout publication

UiStyleResolver and UiLayoutEngine remain the sole mutable publishers. Their additive Prepare methods produce move-only copied pooled snapshots and reserve each owner's bounded scratch state. Competing Update/Invalidate calls return typed candidate-busy errors immediately. There is no wait or retry loop. Existing Update remains Prepare followed by Commit.

Prepared style candidates pin the exact immutable registry owner and tree issuer. Prepared layout candidates pin the actual issuer and copy root geometry, source revisions and font scale; they never retain an evaluator pointer. Commit revalidates the current tree's burned root identity, source document/tree generations, registry identity/generation, and owner lifecycle. Matching numerical fields from another owner do not grant publication.

Abandon and destruction cancel without publication. Moves transfer the single reservation; moved-from candidates are inert. Retirement/shutdown close admission while copied candidates and older immutable snapshot leases remain readable. Reentrant layout evaluator updates return candidate-busy; shutdown during evaluation closes admission without clearing scratch references before evaluation unwinds.

The eventual animation coordinator must finish every fallible clock, track, style, layout and route-gate operation before the callback-free publication sequence. These prepared-owner changes are intermediate source work: the atomic aggregate coordinator and its downstream failure regression are not yet implemented or validated.

## Sampled style lane

`UiStyleElementInput::animation` is an additive trailing span, so existing aggregate callers continue to supply an empty lane.
Samples are literal typed values, not token assignments or declarations that can seal a property. The sole style resolver applies
visual-state rules, then sampled continuous values, then accessibility policy. `UiStyleOrigin::Animation` is appended to preserve
existing origin ordinals. Registry category/range and authored sealing remain authoritative, and duplicate sample properties fail
admission. Content hashing includes sampled values even when authored/interaction revisions are unchanged. Retained snapshots
keep their previous values. Clock and timeline completion authority is not granted by this value projection contract.

This lane and its owner regressions are currently source drafts; no new animation source validation has run yet.

## Fixed dispatch provenance

`FixedStepContext` now appends scheduler-issued `attemptNumber` and `frameNumber`; existing aggregate callers remain source
compatible through default initialization. Zero attempt/frame is an unqualified legacy test context and cannot prove an animation
host commitment. `FrameContext::committedFixedStep` carries the exact last successful tick/attempt/frame/duration. The scheduler
reserves an attempt before callbacks and only publishes commitment after every fixed participant and the post-dispatch
cancellation check succeeds. Exhaustion returns a typed failure before dispatch, leaving the last identity intact.

After fixed work, the scheduler refreshes committed tick and attempt evidence before `NetworkFlush`, as required by the existing
`FrameContext` contract that evidence describes work committed before the receiving phase. No global rate is reapplied. A later
failed catch-up attempt leaves prior successful commits intact; an attempted same-tick retry receives a distinct identity. The
animation adapter must replace failed same-tick staging and consume the exact successful fence once, only on complete UI
publication. Its bounded ledger must retain earlier successful durations until that publication. Fixed-step duration is immutable
per actual host; different-duration retry histories belong to the actual ledger's private qualification, not an invented public
scheduler reconfiguration feature.

Five new source cases qualify failure before/after the observer, same-tick retry identity, partial catch-up preservation,
post-fixed cancellation and direct actual fixed/frame helper max/exhaustion boundaries. Frame admission now rejects ordinal exhaustion before sampling or dispatch instead of wrapping. They remain unrun source until a granted batch.

## Timeline policy and bounded candidate arithmetic

The additive `UiAnimationTimeline.h` owns inert domain, delay/duration/rate, direction, loop, fill, hold and lifecycle policy.
Timeline IDs are distinct from element and clock IDs; authored animation IDs do not contain a runtime cursor. Required-enter/exit
metadata does not issue a route gate. Accessibility policy is already resolved by its owning boundary; these types do not implement
HORO-732 or infer reduced motion from a zero rate. Public consumer traits qualify the type boundary.

Owner-private playback candidates use checked rational remainder, deterministic integer progress and a declared finite iteration
crossing budget. A required overflow/backlog rejects the entire candidate; it does not skip crossings or publish a partial terminal
outcome. Zero duration resolves its final value and completion once in the same update. Completion and cancellation remain mutually
exclusive; repeated reads/evaluation do not create another terminal event. Literal continuous interpolation preserves semantic color
roles and rejects discrete/category-changing or malformed endpoints. These helpers are source drafts to be used by the real bounded
owner, not independent publishing authorities.

The application-boundary fixed-attempt ledger is available only through a dedicated noninstalled integration interface depending on
Runtime and RuntimeUi; RuntimeUi does not acquire a Runtime dependency. It copies scheduler attempts, replaces a failed same-tick
candidate with the exact later attempt, and proposes only the matching successful prefix. Capacity, source mismatch and duration
overflow leave every unread commit intact. Actual participant binding and lifetime must still supply source authority; copied public
contexts alone grant none. Ledger consumption belongs to the callback-free successful aggregate publication.

Eight ledger, eight playback and three interpolation source cases add boundary/rollback qualification. No configure/build/CTest has
run on these sources. Full six-domain owner, bounded track pool, actual style/layout projection, route gate and application participant
integration remain required before this ticket is complete.

## Canonical host simulation timing

`RuntimeHost::SimulationControl()` and `FrameScheduler::SimulationControl()` expose the actual host-owned command capability.
It has no public factory or mutable frame-context authority. `ReadPolicy()` returns an opaque retained read; every command
checks the actual issuer and admission revision. Normal hosts retain rate 1/1, no pause, and their existing fixed quantum.
The new configuration tail capacities default to 64 pause leases and 64 pending/retained step receipts, preallocated at creation.

Timing commands issued during `ApplyQueuedOwnerThreadCommands` join that frame's successful cutoff. Commands issued later
are desired state for the next cutoff. Pause reasons compose: releasing a menu or cinematic lease does not release another
owner's pause. Release only signals the actual record; owner-thread cutoff reconciles it. Suspension admits no steps and does
not accumulate suspended wall time. Existing cinematic acquire/release hooks remain application-owned composition points;
Runtime has no dependency on the Cinematic implementation.

Positive rational rate scales only admitted accumulator time, with checked exact fractional carry retained across rate
changes. Fixed tick durations remain the configured quantum. While paused, explicit bounded steps commit one quantum only
after all participants and cancellation checks succeed, without consuming the paused accumulator. A failed RuntimeHost frame
still shuts down the host; pending receipts report HostRetired and no successful tick. The lower FrameScheduler/lifecycle
boundary retains its existing caller-owned recovery policy. Opaque receipts may retain their copied terminal records after
host destruction; they grant no new admission authority. Caller operations other than release/destruction are owner-thread only.

Public const access cannot issue commands or borrow a mutable timing publisher. Rate, pause and step mutations remain
non-const. Presentation duration remains unscaled, and UI Simulation clocks consume only matching successfully committed
fixed evidence. They never apply the global simulation rate again.

## Route action retirement

`UiScreenStackDescriptor::maximumRetiredActionRouters` reserves bounded deferred retirement storage at construction.
Zero uses `maximumRoutes`, preserving existing aggregate initialization. Pop, Back, Replace and Clear preflight the exact
number of attached routers they will remove. If retirement storage is full, the operation returns terminal Capacity
rejection before changing any route, revision or router admission. Applications explicitly drain at a quiescent load-time
boundary and may then retry; navigation never guesses that old router storage is safe to reclaim.

Publication invokes the existing bounded `BeginRetirement` cancellation contract, moves router ownership into reserved
storage and removes only moved-from active slots. It does not invoke handlers or release router storage. Shutdown closes
admission immediately but likewise retains active routers until `DrainRetiredActions()` or final stack destruction.
The drain rejects an active route transaction, shuts down the retained routers and releases storage outside frame work.
Existing handlers, copied command payloads and async cancellation remain owned by their original action-router contract;
this seam adds no callback, independent route authority or completion claim. Const stack borrows cannot drain.

New retirement regressions cover ordinary/aligned C++ allocation and actual nonnull deallocation during Pop (including
middle-container erase), Replace, Clear and Shutdown; these do not measure direct native allocations. Source changes remain
unvalidated until the next granted owner/consumer batch. Full required animation-gate integration is separate from this
retirement mechanism and remains part of HORO-730.

## Actual dispatch provenance

`FrameContext` and `FixedStepContext` append a default-invalid `RuntimeDispatchEvidence` member. Existing aggregate callers remain source compatible; fabricated or manually dispatched contexts carry no scheduler authority. Actual FrameScheduler callbacks receive privately issued evidence. Application composition binds its participant to the exact `RuntimeHost::DispatchSource()` or `FrameScheduler::DispatchSource()` capability; the UI target does not depend on Runtime or create an issuer.

Evidence reads return an allocation-free typed status and copied facts, checking immutable issuer-thread identity before any mutable dispatch state, then exact expected issuer, current dispatch ordinal and phase. Copies of the public DTO may change observations but cannot change facts read from the actual producer. Readonly evidence pins const issuer storage; a private const-propagating owner retains the sole mutable publisher pin. One load-time issuer allocation is retained by the scheduler. The only atomic protects admission retirement; callback facts and the monotonic ordinal are owner-thread only. Teardown retires admission before participant shutdown, while retained identity pins remain safe until quiescent release.

A dispatch guard revokes evidence on every callback exit, including cancellation and contained exceptions. Nested frames and foreign-thread dispatch reject before frame counters or clock sampling. Dispatch ordinal exhaustion is latched without wrapping or replacing active facts. The narrow private arithmetic seam qualifies max-boundary behavior; ordinary scheduler integration qualifies admission/revocation separately.

Fixed evidence describes a pending attempt, not successful commitment. Only the later scheduler fixed-success fence grants that commitment. UI publication stages during VariableUpdate and requires the actual successful whole-VariableUpdate fence at the following RenderExtraction boundary; application composition must put the animation adapter before real UI extraction consumers. A later VariableUpdate participant failure publishes nothing. A later RenderExtraction failure triggers existing fatal host teardown and no presentation; an earlier phase publication must not be described as whole-frame success.

The actual adapter and aggregate integration are implemented in this change; these contracts are not a shipping RuntimeUiService claim. No new compiled or allocation result is claimed for this source until the next registered validation batch.

## Atomic clipping and presented control sources

Animation preparation uses the existing style, layout, clipping and focus owners. The clipping engine now has a target-private move-only preparation reservation; ordinary `Update` prepares and publishes through the same implementation. A candidate pins its actual owner and tree issuer, copies complete scroll/clip geometry into preallocated inactive storage and rechecks tree/root/revisions before publication. Cancellation, retirement, owner destruction and move replacement preserve the current projection and invalidate candidate admission. `UiAnimationFrameLease::Clipping()` observes the matching immutable layout lineage. Focus uses the same scroll-translated, ancestor-clipped interaction boxes, without adding a reverse dependency from Focus to Clipping.

ADR-180 requires replacement presentation source generations. The animation composition therefore reserves distinct inactive control and action-router storage at load time. It does not modify an active control descriptor or active router owner identity. Replacement preparation preserves logical values, text edit drafts and their Cancel baseline; transient press/repeat state resets. Queued actions, pending defaults, async retention/completion/cancellation pins and undrained terminal error storage apply bounded backpressure. Router replacement carries the actual `LastIssuedSequence()` high-water mark; request identities never restart from one. Inactive error pins are released only by explicit quiescent drain. Ordinary standalone control/router creation and default handling retain their existing contracts.

A frame copies actual control source/state records. Old immutable frame records remain readable after a replacement; they cannot authorize new input. `ApplyPresentation` uses the existing current-interaction tracker: publication retains prior presentation evidence but closes current input admission until a successful matching receipt. Old interaction receipts cannot reopen it. `HandleControl`, `ApplyControlDefault` and `SuppressControlDefault` borrow the real privately owned control only while the exact view/source is eligible. Default resolution belongs to its already admitted input and consumes no extra command slot, avoiding a full command budget stranding its pending decision.

Binding write presentation remains last-PRESENTED authority. Preparation rejects undrained writes before changing the aggregate. A matching successful receipt rechecks actual binding/tree source admission, clears old edit evidence before adopting the new source and invokes no foreign `Abandon` callback. Ordinary public `UpdateWritePresentation` keeps its explicit cancellation semantics. No unchecked authored source DTO, alternate widget authority or shipping aggregate host is introduced.

These source additions and new real-host clipping/draft/receipt regressions have not yet been configured, compiled or executed. The prior intermediate 295-test result does not validate this source state. Required route gates and private terminal publication are now implemented below; final full owner/consumer qualification remains pending.

### Style observation and geometry fences

`UiComputedStyleSnapshotDescriptor::publication` and `sources` continue to record
every published observation, including interaction and paint provenance. The
resolver now separately issues `geometry`; application animation composition uses
that fence for `UiLayoutSourceRevisions::style`. Registry-declared `measure`,
`hitTest`, and `accessibility` effects cover layout, clipping, and interaction/focus
eligibility. Their effective values, including inherited values, advance geometry;
tree topology and registry generation changes advance it conservatively. Pure
paint or provenance changes retain geometry. Layout constraints, canvas, intrinsic
measurements and policy remain separately checked by the existing layout owner.

Consumers must retain full computed-style provenance for rendering and diagnostics;
the geometry fence cannot replace it. A property declaration affecting geometry or
eligibility must declare those effects. Geometry exhaustion fails preparation before
any slot publication, preserving the last-good style and layout. These source changes
and the new regressions are not yet compiled or validated.

The geometry fence is scoped to the real style resolver owner. Creating animation
composition explicitly invalidates the actual existing layout owner once; a numeric
revision match from a previous producer cannot establish first-publication equivalence.
Subsequent unchanged effective geometry keeps the existing interaction receipt and
pointer capture valid. A changed layout interaction atomically cancels old capture
leases with `InteractionRevisionLost` before the new frame can receive input.
`CapturePointer` on the animation owner/application participant routes normalized
requests through the existing actual tree and last-successful-presentation tracker;
it does not mint route, view or audience authority.

### Required route reservation source draft

The actual screen stack now has a private required-animation reservation that holds
its ordinary move-only transaction, the unchanged stack revision, checked next
revision, and a pre-issued route incarnation when the mutation creates a route.
Cancelled reservations burn an issued incarnation; it cannot later be reused. Normal
instant navigation retains its existing commit-time incarnation issuance semantics.
Both paths use one checked-preparation/no-fail-mutation implementation and retain
removed action routers until explicit quiescent draining.

Only the animation aggregate friend can construct the private terminal proof from
its actual completed candidate. Public consumers cannot create a gate or terminal
proof, and ordinary caller outcomes do not authorize required motion completion.
Owner progression now participates in the aggregate candidate and terminal publication described below.
These current private contracts and consumer constraints still require final compilation and validation.

An equal interaction/source observation is not a replacement generation. The private
control and action reservations retain their actual active storage, press/repeat/edit
state, queued actions and pending defaults/async work, while revalidating copied
source/state stamps. They do not swap pools or demand pending work drain. Only a
new interaction/source requires the replacement admission and cancellation rules.
Likewise a write target whose source is unchanged retains its edit and outstanding
work; no presentation-only observation clears it. The new real-host held-press and
pending-default regression is still unrun.

### Required route transactions and presented input

`UiAnimationRuntimeParticipant::Navigate` now admits an actual `UiScreenStack` transaction. Its private gate preissues an incoming
route instance and reserves bounded exit/enter timeline slots, burning every issued child generation even if admission later fails.
The inert `UiAnimationRouteBinding` selects only an existing catalog route, actual retained-tree root and admitted finite required
animation; unknown/duplicate bindings or tracks outside that root reject during owner creation. Neither authored IDs nor a public
completion DTO can construct the private stack terminal proof.

Exit stages retain their actual old route instance; an entering stage binds the preissued incoming instance. Clear runs bounded exit
stages from top to bottom. Pop/back may enter the revealed retained instance. The child clock derives only the candidate's admitted
`PresentationUnscaled` duration, never a controller delta or a second global scale. Each stage has a finite authored maximum wait.
Completion, explicit cancellation, deadline and teardown close the exact operation once; replacement cannot reuse an old child or
route incarnation. Competing navigation rejects while a gate is reserved. Non-policy operations still publish through the next
aggregate cutoff; existing direct screen-stack convenience operations retain their existing instant behavior.

A prepared frame carries both immutable committed `Routes()` and `RouteOperation()` evidence for the incoming/outgoing animation
root. No render backend is selected by this owner. Clock, timelines, style, layout, clipping, focus geometry, source replacement and
terminal stack publication share one validated publication. Required progress closes normalized input until terminal publication
and the new frame's actual renderer receipt. The aggregate supplies an explicit private presentation-eligibility mask from its actual route root and clipping policy; the focus owner does not infer semantic participation from public layout boxes.
ordinary public focus layout updates preserve their existing unprojected policy. Compatible control drafts and queued defaults are
retained on unchanged interaction generations. New interaction generations require pending defaults, router queues, async producer
pins and admitted writes to drain rather than silently rewriting their immutable source identity. Fatal `RuntimeHost` failure still
tears down its participants; it does not become a recoverable UI retry mechanism.

All stage, frame, retired-router and source replacement capacity is reserved at load time. Successful frame publication invokes no
foreign handler, provider prepare/commit/abandon callback or storage reclamation. Quiescent `DrainRetired` retains responsibility for
reclamation; shutdown revokes gate admission while old immutable frame leases remain readable. The new integration regressions
exercise the real host, stack, pointer capture, controls, admitted write authority and attached async router. These current source
changes require final compilation and regression validation; earlier intermediate results do not establish them.

## Explicit load-time animation reload

`UiAnimationRuntimeParticipant::Reload` is an owner-thread structural operation outside active scheduler dispatch. It uses the
existing sole `UiHotReload` publisher's actual Prepare/Reconcile/Commit path, including cooked provenance, semantic owner,
ever-issued namespace and actual source-stamp validation. New style, layout binding, controller and finite animation resources
are prepared before the commit. Its private inert publisher holder has no generation, admission or activation; only successful
commit transfers the existing publisher. Failure or cancellation leaves the previous publisher, source cursors and controllers
unchanged. No other publisher or independent control-state map is created.

The supported policy is explicitly `Cancel` or `Restart`. Cancel returns copied Reload terminal outcomes for every previously
active instance. Restart also admits surviving nonblocking authored definitions from their start with fresh handles; it never
carries an old cursor heuristically. Changed domain/lifecycle, conflicting restarted tracks and invalid descriptors reject.
Required route gates must finish or be explicitly cancelled and published before reload; reload returns busy rather than hiding
an interrupted route transaction. Unsupported numeric policy values reject. Preserve-cursor, snap and complete-old policies are
not implied by equal stable IDs and are not exposed as supported policies.

The actual 704 reconciliation preserves compatible control values/edit drafts and their Cancel baseline, focus, authored routes
and scroll state. Raw control, animation, clock and controller handles deliberately change incarnation. Retained old frames stay
readable; the new generation becomes input-eligible only after its actual new renderer receipt. Canonical committed simulation
and admitted presentation consumption remain in the actual participant, so reload neither replays nor discards host duration.
Queued optional-domain commands belong to retired controllers; applications retain the returned fresh controller. Reload and
reclamation may allocate/free bounded load-time storage and must not be called during frame extraction/publication.

The retirement/re-admission test proves recreation lifetime fencing only. Separate real in-place publisher replacement tests now
cover compatible draft state, stale source/controller rejection, malformed/cancelled rollback, explicit restart with a fresh cursor,
and pending-gate backpressure. These new tests are authored and unrun at the initial draft source freeze; historical intermediate
295 tests do not qualify the current implementation.
