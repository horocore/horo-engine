# Resolved VFX spawn pipeline

`VfxSpawnRequest` is a copied, bounded command owned by `HoroVfxApi`. Gameplay
meaning and event/tag selection belong above VFX. An asset identity, occurrence or
owner number alone grants no semantic gameplay permission. Application composition
must admit the corresponding scope before forwarding a resolved command.

The new World mode is constructed atomically with static `VfxWorld::PrepareSpawns`
before attaching the complete World to its real Scene. It reserves one aggregate queue/result/owner/producer
envelope and a finite palette of CPU recipes. Preparation publishes atomically;
failure preserves all external published ownership. Legacy raw CPU/GPU emitter preparation remains
source compatible. Mixing legacy emitter bindings and the new palette in one World
is rejected explicitly, so their instance ranges cannot silently overlap.

Each palette entry binds an actual validated `ParticleSystemDescriptor`, one
host-admitted additive material, initial burst, finite effect duration, scalar input
schema and concurrency reservation. This recipe executes the existing canonical
`CpuParticleSimulator`; it does not implement an effect graph compiler. The initial
supported path requires explicit `RequireCPU`, unsorted camera-facing billboards and
copied analytic/no-collision inputs. GPU recipes, native collision callbacks,
scene-depth inputs, arbitrary stage callbacks, sorted/mesh/ribbon recipes and other
unprepared paths fail cold. They do not silently become CPU work.

`VfxSpawnBudget` is one Scene-wide reservation. Pool payload charges conservatively
reserve kernel storage and then verify actual simulator bytes before publication;
these charges are not a claim about process RSS. Queues, results, authority records,
instance controls and actual vector capacities are charged as well. The preexisting
extraction reservation remains explicit; the spawn slice must fit its particle,
batch and draw capacities. No per-asset budget creates another aggregate allowance.

The application submits one complete bounded fan-out in canonical layer order.
Validation checks every layer before queue or producer watermark mutation. Missing
assets, malformed placement, unsupported scalar schema, stale ownership and capacity
denials return correlated enum outcomes without diagnostic string allocation.
Pending work reserves a terminal-result credit. Clearing result evidence preserves
source deduplication watermarks. Source sequences never rewind or wrap in one Scene.

Spawn cutoffs use one-based owner ticks; zero denotes the unpublished initial World.
This does not change the existing local `EffectPlaybackRequest` contract, which still
permits tick zero. Update freezes the accepted prefix and orders it by tick,
producer, sequence, destination and layer. It advances the real CPU kernels once at
the chosen owner cutoff, independently of extraction or view count. Required
requests need both a CPU gameplay recipe and an actual required pool reservation.

The request carries canonical millimeter world position, normalized orientation,
strictly positive effective axis scales, optional unit world impact normal and up to
32 schema-declared scalar GameplayInput values. Empty parameters select prepared
defaults. The optional normal remains explicit occurrence data; VFX does not invent
a gameplay meaning or silently turn it into an orientation. Placement transforms
the actual committed particle positions and sizes during immutable extraction.

The World alone assigns contiguous, disjoint same-Scene pool slot ranges. The added
`EffectPoolDescriptor::firstInstanceSlot` defaults to zero for existing callers;
nonzero ranges preserve offsets through restart and retirement, and invalid-sentinel
or wrapping intervals fail preparation. Callers creating independent pools outside
the World still own their own disjoint-range reservation; a local pool cannot prove
global uniqueness.

`CpuParticleSimulator::Reset` reuses prepared storage for a strictly newer activation
of the same scope/slot. It preserves particle IDs, tick counters, lifetime statistics
and committed-generation monotonicity. Both buffers, pending kills, input defaults
and spawn carry retire together. The owner must quiesce borrowed views and prior
work first. World kernels never escape the World; immutable extracted frames own
their own copies and survive effect reuse or Scene destruction.

`Advance` and the added `AdvancePrepared` execute one shared canonical stage sequence
and candidate/commit path. Prepared execution returns copied counts and a pointer to
the original static failure descriptor; expected capacity, cancellation, invalid-step
and analytic-collision failures do not materialize diagnostic strings. World result
and update evidence retain that exact kernel descriptor. A failed first step replaces
its initial activation result before the owner cutoff returns; it cannot publish a
successful `Spawned` terminal for an uncommitted effect.

Legacy `Advance` translates the same stage failure to its existing full `Result` error
and preserves provider metadata and immutable cause wrapping. Its busy scope remains
active through error materialization. Observer callbacks, native Physics providers and
scene-depth diagnostic profiles are explicitly unavailable through `AdvancePrepared`,
and are never invoked by a rejected prepared call. They remain available through the
legacy API; no provider error is shortened, suppressed or silently converted to success.
The shared stage carrier introduces no alternate simulation kernel or ambient
diagnostic mode; the public prepared outcome retains only copied counts and its
process-lifetime static failure descriptor. Prepared allocation tests cover both successful work and
expected failures; actual qualification results are reported separately.

## Gameplay mapping and source publication

GameplayVfxBindingDocument is versioned application data. Its strict bounded JSON
codec admits exact event OR tag selectors, source schemas/ranges, canonical layers,
asset identities, scalar constants/routes and required/cosmetic policy. No event
name, tag number or asset identity grants permission. Cold Cook validates actual
pinned registry records, provider availability and the genuinely prepared World's
asset/schema palette. It does not compile an effect graph from provider bytes.
Published generations increase; failed reload preserves the prior table, and queued
semantic commands retain the exact immutable generation they captured.

The lower GameplayVfxIntegration target sits above GameplayApi and VfxApi. The
optional installed Vfx SDK component exports that lower capability/codec surface;
RuntimeScene, Cinematic, trusted broker headers and render backends remain outside
that component. GameplayApi itself only forward declares the optional capability.
Its backend virtual is appended and its default returns empty; existing capability
ordinals and default-empty service fields retain their ordering. Native modules
must rebuild for GameplaySdkBoundaryVersion 29; exact older-boundary rejection
remains in force, so a new module cannot invoke the added capability on an old host.

GameplayVfxSceneIntegration attaches the same canonical World to the actual
unpublished Scene. Register MakeActivationParticipant before Scene service startup;
its unique service registration retains the same shared application facade. It
creates no second Scene, runtime identity or kernel. Immutable recipe spans are
copied cold. Candidate rollback and old Scene retirement revoke only their exact
source lease; a replacement's grants survive stale cleanup.

Native BehaviorContext::VfxContext and Lua ctx.vfx use the same admitted module,
runner and entity scopes. The trusted host supplies both the client and exact
module principal; a foreign module/Scene is rejected before callbacks. Runner and
attachment teardown revoke only their independently derived children. Successful
Scene structural retirement immediately denies further entity calls while retaining
already admitted commands; explicit client revoke, failed source commit and
runner/module/Scene revocation still cancel pending work. Holding a
client or Lua closure beyond a callback never holds a World alive or extends the
callback window. No VFX capability or live handle is serialized into saves.

Lua calls ctx.vfx.emit(eventNameOrPositiveTag, payload). Placement uses exact Lua
integer x_mm/y_mm/z_mm, optional finite scale, orientation={x,y,z,w},
normal={x,y,z}, axes={x,y,z}; parameters are at most 32 ascending {channel,value}
pairs under an explicit positive schema integer. Absent normal remains absent.
Lua's representable positive integer range applies to tags/schema identities;
malformed or unauthorized inputs return typed numeric status, without spawning.
The second return is an opaque decimal string preserving the full uint64
application occurrence sequence, not an effect handle.
A staged Accepted result does not claim Scene commit, World queue admission or
Spawned completion. Lua callback errors and VM allocation failures cancel only the
current attachment's uncommitted commands at that current tick. Prior sealed work,
other attachments and independently committed cinematic events remain intact.
Cancellation preserves sequence watermarks and does not permanently revoke the
client, so a subsequently valid script reload can recover. All retained C++ owners
live outside the protected Lua longjmp boundary; userdata receives a fully prepared
finalizer before any subsequent fallible Lua allocation.

Configured product composition selects an explicit GameplayVfxScene prototype.
ComposeHeadlessSceneNetworkServices captures a cold sibling for each genuine
current/replacement WorldRecord and registers it before Scene startup. Siblings
share immutable recipe/policy inputs, never a World or active source capability;
explicit foreign clients still fail Scene/module admission. The actual fixed owner
tracks gameplay before fallible callbacks so partial attempts receive abort. Omitting the prototype preserves the existing product path.

The real host opens each source attempt from the actual fixed tick. Scheduler
prepare/commit seals bounded tick metadata; failed attempts abort only their
unsealed behavior commands. Sealed batches retain original ticks, deltas, order and
captured policy until the actual Scene structural receipt completes. OnPhase
success alone is insufficient proof: Scene can report a structural rejection
through its tracked receipt while returning success. A rejected source transaction
cancels its behavior commands; independently committed cinematic commands preserve
their own commit facts. The World alone resolves/adopts commands and advances in
original cutoff order. The canonical scheduler extracts before deferred Scene
publication, so committed effects become visible in the next genuine extraction;
a presentation frame with unchanged clock observes them without another fixed tick.
Capacity, simulation and cosmetic outcome failures remain
copied source/World evidence; they do not retroactively invalidate committed Scene
state or silently shut down a healthy scope.

The unchanged CinematicEventDispatcher callback/registration contract is consumed
by CinematicVfxEventAdapter. Cold preparation decodes the real CookScriptEvents
canonical ScriptValue Array into bounded cached placement/scalars. Dispatch verifies
the exact pinned plan key, binding, schema and payload, then stages through the
Scene-issued committed producer. Full player/session generations, track/key/time,
traversal/direction and committed tick are retained in correlation; traversal alone
is not an occurrence identity. Dispatcher commit/order/dedup remains authoritative,
and registration leases retain adapter state without inline parsing, evaluation or
completion callbacks.

Allocation guarantees cover prepared semantic resolution/staging, sealed metadata,
canonical World queue/update and expected prepared-kernel failures. Cold cooking,
registration/derivation, legacy diagnostic materialization, Lua VM allocation and
Scene structural command/receipt allocation retain their existing separate scopes.
CPU tests and installed SDK consumers do not imply GPU/backend/hardware clearance.
