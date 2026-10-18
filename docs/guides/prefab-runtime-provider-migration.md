# Runtime prefab provider integration

This guide describes the cooked template provider and its application-owned
Editor Play and headless integration. Provider and transaction unit tests are
complemented by production-host Physics/AI/Gameplay aggregate regressions;
current execution evidence and platform limits are recorded in the PR.

## Provider authority and lifetime

`HoroEngine::PrefabRuntime` owns `PrefabTemplateProvider.h`; consumers no longer
need implementation include paths. The host explicitly supplies the registry,
shared asset loader, Scene service, limit profile and resource budgets. Those
borrowed authorities outlive the provider. Provider mutation and load observation
use the host owner lane; cancellation tokens may cross worker lanes.

A request names the exact cook target and SHA-256 of the complete published
artifact envelope. A successful lease owns the verified root and dependency byte
allocations and captures the registry snapshot and Scene incarnation. A retained
lease remains readable after eviction but is not permission to spawn: admission
must recheck provider identity, current catalog and current Scene. Dropped and
move-replaced load observations cancel their requests without refunding capacity
while the shared-loader worker is still running. Provider shutdown cancels and
releases its ownership, never joins or shuts down the shared loader.

## Scene publication

The host composes exactly one structural participant per subsystem owner before
Scene service startup. Owner preparation is detached. After every fallible owner
preparation and validation, Scene checks generation, current catalog, request
cancellation and provider retirement again at the final publication fence.
All owner publication is allocation- and callback-free; Scene then publishes its
complete entity group before postcommit notifications. Public direct Scene commit
does not supply catalog authority for resource-bearing prefab groups.

`QueuePreparedGroup` requires complete typed component projections in dense cooked
entity order. This is not a property-bag spawn API or permission to drop cooked
component occurrences, references or bindings. The public Gameplay spawn request
and initializer surface remains the existing HORO-1039 boundary.

The provider now checks occurrence and reference coverage against the immutable
cooked tables before queuing Scene work. Pass dense member rows and reference
rows in cooked order, including explicit unbound optional interfaces. Preserve
raw component occurrence identities; freshly allocated behavior instance IDs are
allowed. Internal entity/member targets and asset dependencies cannot be redirected
by projection. External bindings retain their declared component type and required
status; Scene continues to validate the actual live target at publication. Callers
which omitted interface metadata must supply the complete projection rather than
relying on an empty table. The host supplies the canonical native component projection described below;
separate aggregate regressions exercise publication across the real owners.

## Gameplay owner

`BehaviorRuntime::MakeStructuralParticipant` stages against the existing runner,
not a replacement scene population. It validates descriptors, attachment identity,
multiplicity and the live instance budget, subtracting instances retired by the
same transaction. Preparation copies attachment metadata and reserves storage,
but calls no factory or lifecycle hook. After aggregate commit it retires removed
instances and constructs only new attachments in deterministic callback order.
A factory/hook fault tears down this group's partially constructed instances and
reports a postcommit diagnostic; it does not roll back published Scene entities or
revoke the capability of unrelated existing instances.

Reentrant shutdown during structural notification requests deferred teardown.
Prepared candidates retain the runner state and copied factory bindings; teardown
occurs after callback references are released. The Scene and native module generation
still obey their owning host lifetime contracts. No adapter owns or reloads the
Scene, discovers a backend, or chooses another module.

The non-installed application factory `MakeGameplayStructuralParticipant` borrows
an explicit host-owned active composition slot. Register it before Scene startup;
populate and retire the slot at drained host lifecycle boundaries. The slot is not
a service locator. Required Gameplay work fails explicitly if the selected owner
is absent or retired.

## Physics detached body admission

`PhysicsWorld::PrepareSceneBodies` returns an owner-lane preparation with reserved
handles, not active bindings. The canonical provider creates native bodies without
adding them to the solver and prepares their broadphase insertion before Scene's
final commit fence. Rollback destroys every detached body. World teardown cancels
the pending native batch before deleting the solver, making retained preparations
stale without accessing a destroyed owner. Publication admits the complete batch
and invalidates the old query/event publication only then.

`PrepareSceneGroup` additionally owns a detached analytic/compound shape DAG; child
indexes address only earlier entries in that group. No shape enters the resident
world table during preparation. Rollback destroys constraints before bodies and
shapes, refunds resource capacity, and never rewinds issued handle identities.
`PrepareConstraints` resolves reserved or resident body endpoints, constructs native
constraints without registering them, and poisons final validation on failure.

The pinned Jolt 5.6.0 archive and license are unchanged. The reviewed hash-checked
reservation patch adds supported `PhysicsSystem` capacity methods delegating to a
locked `ConstraintManager` reserve. It does not add/remove constraints, set indices,
change enabled state, or publish a binding. Preparation asserts unchanged native
membership/order/enabled state around reservation; final constraint addition asserts
unchanged native capacity. Horo constraint and collision-pair storage is also fully
reserved before publication. The generated third-party notice identifies the patch
and its digest; generated notices/build outputs are not repository inputs.

Native owned-resource regressions, public-header consumers and the concrete Physics
structural adapter now run in the Linux skeleton suite. Adapter regressions cover
rollback, publication and removal of generation-qualified bindings. The provider-driven application aggregate uses the same concrete adapters and
qualifies them together, as described under Integrated acceptance.

## AI owner staging

`AiSceneRuntime::MakeStructuralParticipant` copies the immutable controller catalog
and borrows the explicit AI runtime. It may be registered before initial activation;
preparation resolves that runtime's current publication, not a cached startup pointer.
The runtime must outlive the registering Scene service.

The staged adapter preserves resident agents, blackboards and running tasks. New
agents and their blackboards remain detached until aggregate publication. Removed
agents' work is retained until postpublication cancellation/observer teardown.
Preparation reserves destination slots, cancellation sources and the complete owner
index. Replacements may reuse retired capacity only with a fresh agent generation;
generation-exhausted slots remain tombstones. Owner mutation, replacement and shutdown
invalidate the captured publication before commit. A candidate retains the publication
storage, not a borrowed pointer to a deleted runtime state.

AI activation's internal-state constructor argument is now `shared_ptr` rather than
`unique_ptr`; application callers should continue obtaining activation candidates
through `PrepareScene`, not constructing implementation state. The control block is
allocated during preparation, never during allocation-free activation publication.
Affected C++ consumers must rebuild; the public module C ABI is unchanged.

The AI group cases and public consumer run in the Linux skeleton suite. The application composition and provider-driven Physics/AI/Gameplay aggregate
rollback regressions cover the integrated boundary described below.

## Consumer migration and evidence

Rebuild affected C++ host consumers after the public header/layout changes. This
does not change the frozen native Gameplay module SDK/C ABI or install a second
runtime. Register owners explicitly and retain their authorities through the
complete synchronous Scene transaction and postcommit callback phase.

The narrow provider lifecycle tests and Gameplay metadata rollback, unknown
attachment, reentrant shutdown and full-budget replacement tests cover the
implemented paths. Detached Physics body and constraint tests run with the native
provider; the omitted-provider composition has a separate targeted pass. Jolt patch
regressions verify fresh and repeated application with both LF and CRLF inputs and
reject unexpected source changes before mutation. The integrated targets described below extend these foundation checks through
the actual host routes and complete owner lifecycle.

## Quality repair migration

The provider observation/cancellation operations, Gameplay runner dispatch,
native preparation operations and AI safe-point operations now expose
const-qualified wrapper methods. They still
mutate their explicitly owned or borrowed runtime state on the documented owner
lane; const qualification does not grant concurrent access. Existing ordinary
call expressions remain valid, but affected C++ consumers and member-function
pointers must rebuild against the new declarations. Scene group admission is
borrowed by const reference and copied into the owned command buffer.

The non-installed Gameplay structural composition factory now borrows a raw
pointer slot by reference. A host updates that slot only at drained lifecycle
boundaries and keeps the slot and pointed-to composition alive through each
transaction. It does not transfer ownership or capture a stale pointer snapshot. Temporary
pointer slots are rejected at compile time; pass the host-owned pointer variable.

`DecodeCookedArtifactBytes` accepts immutable cache byte leases directly. The
existing integer-buffer decoder forwards to that same implementation without
copying the encoded input. Both entry points retain identical envelope bounds,
identity, digest and malformed-input checks. Public header ownership and target
dependencies remain unchanged. Native detached candidates explicitly delete copy
operations so no second object can inherit their rollback obligations.

## Complete provider composition (HORO-1038)

The completion uses the published Character Scene host composition rather than
introducing another Scene/world authority. The runtime provider, spawn service,
registry and shared asset loader belong to the same application world record.
Shutdown revokes the module grant and pending publication first, closes the
provider's pending loads/cache admission, then retires the Scene-owned aggregate
and its AI/Gameplay/native resources. Scene group leases continue pinning any
evicted bytes until owner retirement completes. Only the loader owner
cancels/joins loader jobs; provider observation remains nonblocking.

### Canonical component projection boundary

`RawComponentPayload` and the HPFB member encoding remain the durable carrier.
A host explicitly supplies a borrowed, owner-lane `PrefabComponentProjector` to
the existing spawn service. Its lifetime encloses pending load observation,
projection and service shutdown; the host retires it only after pending operations
are cancelled and Scene group owners drain. Queued Scene transactions and retained
immutable leases never retain its callbacks. The
runtime public boundary carries Horo typed values only: no JSON implementation,
source resolver, filesystem path, native backend or service discovery.

The application implementation delegates to one inert built-in projection in
the Scene conversion owner. That implementation calls the existing canonical
component decoders through `HoroSceneSourceCodecInternal`, already shared by the
Scene source and cooked Scene owners. Source-prefab conversion uses the same
projection. It does not parse a source file or expand a source graph at runtime.
Unknown project components continue through the frozen `ComponentRegistry`;
recognized but unsupported built-ins fail explicitly rather than becoming opaque
Gameplay components or being discarded.

The Runtime Scene owner defines the stable built-in schema identities and counts
for group occurrence validation. Dense member ordering, original occurrence IDs,
portable reference interfaces and typed Physics fixups remain distinct. The
existing Scene group transaction validates their complete correspondence before
preparing any subsystem. Local Physics endpoints resolve the exact cooked
provenance scope into dense group slots, never an assumed numeric Scene ID.
Behavior instances preserve their existing host allocation boundary. AI identities
are reserved by the one registered AI owner, never by Prefab. Structural contract
version 3 adds an explicit Identity preparation phase before DerivedResources.
The AI participant prepares detached additions and complete entity-qualified
original/resolved identity facts. Scene rejects foreign, duplicate, incomplete or
conflicting facts before applying them to its private candidate and before any
Physics/Gameplay derived owner prepares. Host registration order grants no
priority. Existing owners default to DerivedResources and return no facts.

Only newly created groups with exact retained prefab envelope/lineage evidence
participate in AI remapping. Canonical projection rejects malformed duplicate
original IDs within the same source asset and nested occurrence before remapping;
separate nested placements may carry the same authored template ID. Ordinary
Scene IDs and opaque custom bytes are never rewritten. AI's publication-owned
high-water is initialized from the initial population, accounts for ordinary
new authored IDs before reservations, rejects overflow, and advances only in the
existing no-fail aggregate Publish. Rollback/retry leaves it unchanged; another
publication invalidates any overlapping reservation through the owner revision.
Currently the only typed agent-identity reference is AiAgentComponent::agent;
controller/schema/decision identities are separate domains and are preserved.

The supported set follows actual canonical codecs, including disabled values and
collection occurrences. Audio listener/UI canvas representations that the current
canonical cooker rejects stay typed unsupported. The published Character
contract's enabled structural Character rejection remains in force: this work
must not manufacture a dynamic Character owner or silently omit that component.

Version 3 changes the structural participant/candidate virtual contracts and
requires rebuilding every host implementation and consumer. The explicit public
consumer asserts version 3; hosts overriding only Prepare remain DerivedResources,
and host candidates with no identity authority retain the empty default span.
There is no parallel structural commit API or post-publication patch.

This also changes host C++ construction/projection contracts (including trailing
primitive geometry in prepared group options) and requires rebuilding
`PrefabRuntime`, Scene conversion, both applications and their consumers. It does
not change the RawComponentPayload/HPFB layout or module descriptor layout.
`CMakeLists.txt` exports the explicit minimal/Physics/Navigation/RuntimeUi SDK
sets; PrefabRuntime/RuntimeScene are a separate host-only PIC fixture closure,
not exported SDK targets. External SDK consumer qualification is still required;
layout inspection alone is not execution evidence.

### Integrated acceptance

Completion requires a real verified artifact flowing through Asset Registry,
AssetLoadService and the production host route into Physics, AI and Gameplay
owners in one Scene transaction. Unit tests of independent adapters are useful
but insufficient. Required regressions cover all-owner publication, each owner
failure and final admission failure, resident preservation, repeated spawn and
despawn, cancellation, retained-lease eviction, catalog replacement, scene unload
and shutdown with outstanding loads. Native-omitted and absent-owner paths must
return typed failures without selecting another backend. Current-head execution
and its limitations are recorded separately in the PR validation receipt.

### Application permission and owner migration

Both normal application entry points accept an independent `--prefab grant|deny`
option. Omission retains denial. Headless grants require explicitly selected
gameplay and a configured project Scene; legacy synthetic network fixture modes
keep their previous behavior. The existing Physics grant suffix remains a
separate capability. Editor restart forwards the resolved prefab selection rather
than inferring permission from the existence of a cook directory.

Configured headless WorldRecords own one runtime provider borrowing their exact
registry, loader and single Scene service. The actual Gameplay composition owns
the scope/service and typed codec adapter. Editor Play's application host owns
the corresponding provider/service/context; its private controller boundary
acquires the context only after aggregate readiness, advances fixed-tick admission
before behavior callbacks, and pumps provider preparation around the same Scene
commit. Reloaded grants inherit that host's persistent fixed tick before they are
issued, so next-tick requests cannot acquire a reset service clock. Reload, failure
and Stop revoke pending module work before behavior shutdown. Cached immutable templates remain independent of module grants; only
their owner closes the shared loader and joins its jobs.

AI controller catalogs are explicit inert host inputs. Empty or incomplete catalogs
produce the existing typed missing-descriptor outcome for enabled controllers;
no controller, schema or decision implementation is discovered from payload bytes.

Native Navigation currently has whole-Scene activation participants but no
structural transaction owner. Complete typed codec output remains available to
source/cooked Scene activation. Enabled Navigation in a structural prefab group
returns `scene.navigation.structural_unsupported` before any AI identity or
derived owner prepares or publishes. Disabled Navigation values remain intact.
This is an explicit current capability boundary, not a successful native NAV
spawn or a fallback.

### Exact committed bytes and load lifetime

The application `CommittedSceneAssets` capture reads and verifies one published
AssetCook generation once, within its existing aggregate-byte and artifact-count
limits. `Provider()` retains the existing decoded Scene/material payload meaning.
`ArtifactProvider()` instead retains the **original verified envelope bytes** by
ownership transfer from that same capture. Prefab whole-envelope digests are
checked against those bytes, never a re-encoding of decoded metadata. Neither
view reopens files or follows a later `current.json` generation after capture.

Editor and headless composition explicitly create a separate prefab
`AssetLoadService` against that envelope view, using their already running owned
JobSystem. Grants without a selected committed capture are rejected; the raw
Scene loader is never substituted. All role factories and accepted work borrow
these loaders from the app resources. Shutdown revokes gameplay scopes and pending
groups, closes providers and retires Scene owners before draining loaders; the
capture and jobs outlive that drain. Copied immutable template leases remain
readable after provider retirement without retaining Scene admission.

The all-owner integration fixture now publishes real AssetCook generations and
routes exact captured envelopes into real native Physics, AI and module Gameplay
admission. Separate app regressions cover decoded-vs-envelope identity, cancelled
loads, retained generation replacement and corrupt-publication rejection. Actual
Editor Play tests exercise committed grant/deny, spawn completion and retained
client revocation on Stop. These are required execution targets, not claims that
source-only test authoring has qualified their behavior.

### Native component metadata in production composition

When no explicit component registry is supplied, `GameplayWorldComposition` owns
an inert copy of the admitted native module's frozen component metadata. Its
address remains stable from spawn-service construction through shutdown. The
copy is installed after module registration and before behavior activation, so
existing provider/permission admission still precedes native module loading.
Script-only compositions freeze an empty owned registry. Explicit caller-owned
registries retain their existing authority and lifetime requirements. Cooked
project component payloads are inspected against that frozen registry and copied
without rewriting their serialized bytes.

### Owner clock and retained cancellation failures

Gameplay callers use `GameplayPrefabContext::CurrentTick()` to select an eligible
spawn/despawn safe point; module-local callback counts are not the Scene clock,
particularly after reload. Zero denotes bootstrap before simulation, so a caller
uses tick one in that case. Wrong-lane, revoked and retired Scene queries fail
without dereferencing a retired host. This is an additive method with no context
layout or native SDK ABI change. The real native fixture exercises a second
module activation at a nonzero host tick while preserving resident AI tasks.

Each admitted operation prepares an owned cancellation diagnostic before queuing
worker work. Noexcept service shutdown releases pending leases and marks the
observation cancelled without allocating; `Failure()` then returns that retained
diagnostic when no earlier operation/Scene error exists. Observations remain safe
after the service and module retire.

### Explicit Null simulation

The shared application Scene host keeps gameplay and prefab fixed-tick admission
independent from simulation availability. With an explicitly selected Null Physics
composition, it omits both native Physics and Character stages and publishes no
synthetic native tick. Direct Physics capabilities remain unavailable; enabled
Physics or Character producers still fail native activation. Canonical composition
continues to advance the real paired owners in Character-before-Physics order.

### Editor registry principals

`GameplayPrefabBinding::behaviorAllowlist` captures explicit registered behavior
identities when one trusted host owns the complete frozen project registry. An
absent list retains the module-prefix policy; a present empty list admits no
behavior. The Editor host captures the exact frozen registry before callbacks and
revokes this snapshot before reload. It does not rename project behavior types or
infer authority from their source files. Existing module-based callers preserve
their default policy. Rebuild compiled callers for the additive binding field and
the non-installed `PrefabServices(registry)` host signature.

### Editor custom component metadata lifetime

Editor Play captures an owned copy of the frozen project component registry before
Scene preparation. The prefab service borrows this stable snapshot, including on
behavior-only reload, until the Scene host retires; caller-owned metadata need not
remain unchanged after Start. This keeps the Scene's cooked component schema
authority fixed for its epoch. Changing a component contract requires a fresh Scene
and matching cook. Unknown or incompatible cooked component schemas reject the
whole group instead of being dropped. The non-installed host callback now receives
both the behavior registry and this component snapshot.
