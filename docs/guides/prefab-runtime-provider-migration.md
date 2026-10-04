# Runtime prefab provider integration

This describes the HORO-1038 integration under development. Passing provider or
generic transaction tests alone does not establish complete Physics/AI/Gameplay
group participation.

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
and initializer surface remains the separate HORO-1039 boundary.

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
rollback, publication and removal of generation-qualified bindings. A provider-driven
application aggregate remains separate acceptance work.

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

The AI group cases and public consumer run in the Linux skeleton suite. Full
application composition and a provider-driven Physics/AI/Gameplay aggregate rollback
regression remain necessary before this ticket can claim complete acceptance.

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
reject unexpected source changes before mutation. These checks qualify the bounded
foundation, not full integrated owner behavior or complete HORO-1038 acceptance.

## Quality repair migration

The provider observation/cancellation operations, Gameplay runner dispatch and
AI safe-point operations now expose const-qualified wrapper methods. They still
mutate their explicitly owned or borrowed runtime state on the documented owner
lane; const qualification does not grant concurrent access. Existing ordinary
call expressions remain valid, but affected C++ consumers and member-function
pointers must rebuild against the new declarations. Scene group admission is
borrowed by const reference and copied into the owned command buffer.

The non-installed Gameplay structural composition factory now borrows a raw
pointer slot by reference. A host updates that slot only at drained lifecycle
boundaries and keeps the slot and pointed-to composition alive through each
transaction. It does not transfer ownership or capture a stale pointer snapshot.

`DecodeCookedArtifactBytes` accepts immutable cache byte leases directly. The
existing integer-buffer decoder forwards to that same implementation without
copying the encoded input. Both entry points retain identical envelope bounds,
identity, digest and malformed-input checks. Public header ownership and target
dependencies remain unchanged. Native detached candidates explicitly delete copy
operations so no second object can inherit their rollback obligations.
