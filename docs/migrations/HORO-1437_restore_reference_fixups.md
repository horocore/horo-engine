# Restore references and composite Scene publication

HORO-1437 / #1437 / [SAV-004.7] adds the missing Scene-owned composition of
stable identity restoration, semantic reference fixups and the existing staged
save transaction. Application composition retains archive verification, migration,
world/account admission and worker ownership. This API does not create a second
slot loader or infer those checks from untrusted metadata.

## Ownership and phases

`HoroSaveApi` owns `SaveRestoreReferenceContext`, its stable result types, and the
existing `PersistentEntityId` / `SaveAssetId` aliases. The aliases retain their
original tag identity; `SaveReference.h` still exposes them through its existing
include. `GameplayApi` consumes this neutral result contract without depending on
Runtime or RuntimeScene.

`RuntimeScene` owns `PreparedRestoreReferenceGraph` and `SceneRestoreBundle`.
The graph resolves actual candidate entities/components, cooked Scene asset payloads,
explicitly active services and prepared participant receipts. Exact durable entity
incarnations, participant schemas, module/service generations and the current
candidate domain are checked. Required absence and stale evidence fail preparation.
Optional absence remains explicit; remapping requires an explicit same-kind policy.
Component/service type identities stay in the owning typed schema and cannot change
through remapping.

The host supplies archive-qualified bounded inputs, a sole-producer
`ISceneRestoreOwnerSource`, and live `ISceneRestoreGenerationAuthority` evidence.
The source retains worker/source/receipt lifetimes and its admission budgets while
pending; it transfers the original operation controller once. It must never construct
a replacement controller or release exclusivity while owned work is unfinished.
Before reporting pending it observes caller, parent and deadline cancellation through
that sole producer, returning the original terminal cause when cancellation wins.

The Scene service admits `QueuePreparationWithRestore` under its existing exclusive
one-pending-operation rule. Core overrides and component candidates touch only the
unpublished Scene. Gameplay decode/validation/instantiation/application finish before
the resolver is invoked. The complete graph is validated before fixup callbacks;
allocation prerequisites must be acyclic, while references between already allocated
objects may contain deferred cycles. Schema-assigned reference IDs flatten nested
payload fields without recursive graph traversal. Reference lookup, canonical order and
duplicate rejection use the pair of exact participant owner and schema-local identity;
independent owners may both declare identity `1`. Graph consumers pass the typed owner
to `Find(owner, identity)`; no global-ID lookup remains. Nodes/edges and component byte
storage have explicit hard limits.

Actual inactive Gameplay candidates receive `FixupRuntimeReferences` at the receipt
fixup phase. Actual component candidates receive their exact declared reference
subset through `FixupReferences`; each reference must belong to that component
source and participant. Call-scoped views cannot be retained. Owners copy stable
values into their prepared candidate and perform their own schema-specific fixup;
merely registering a target never substitutes for consuming the result.

At `CommitDeferredLifecycleChanges`, the service waits for every restore owner and
validates ordinary Scene candidates plus live restore generations. The original
`StagedRestoreTransaction::Activate(evidence, publication)` cancellation gate covers
all Gameplay, component and Scene ownership transfers. Publication contains no
fallible work or observer callbacks. Completion observers run after the whole new
aggregate is visible; the retired aggregate remains owned until those observers
return. Observer-triggered shutdown is deferred until the gate returns. Failed
preparation, cancellation, shutdown and stale generations retire unpublished work
and preserve the active aggregate.
The same private lifecycle guard protects preparation and rollback terminal observers:
reentrant mutation is rejected and shutdown waits until the executing owner stack
returns, so cancellation cannot destroy its own prepared bundle mid-call.

Dynamic spawned-prefab reconstruction belongs to SAV-004.4. This composition rejects
unsupported spawned provenance explicitly; it does not fabricate a prefab receipt.
Native/subsystem adapters must still prove their own worker, readiness and retirement
contracts before joining the aggregate.

## Caller migration

Existing `QueuePreparation`, participant-only `Activate`, non-reference Gameplay
sources and non-reference component adapters remain valid. New reference-bearing
schemas override the prepared-candidate fixup hook. The default hook accepts an empty
reference set and rejects declared references, preventing silent metadata-only
acceptance. Existing participant receipt implementations need no change unless their
schema owns references; they may override the new reference-aware receipt overload.

`SceneRestoreBundle::Create` remains the only construction path. Its public keyed
constructor exists solely for `std::make_unique` access; the private nonaggregate
key has a private default constructor, so external brace construction cannot forge
admission. Existing callers continue to use `Create` without migration. The linked
Scene restore public consumer verifies that default, aggregate and brace-key
construction remain unavailable.

Archive/session composition supplies the explicit Scene restore bundle and current
generation authority. It must continue to use the original producer and hold all
source/module/native leases through completion or rollback. A source reports pending
rather than returning placeholder receipts. It reports typed failures directly.
Initial load may have no active Scene; replacements require the exact expected
active Scene incarnation. No editor document, cooked asset or active component is
changed during preparation.

The ownership registry, SaveApi source target and both public consumer targets are
updated together. Affected production callers are the Gameplay persistence receipt,
the staged restore transaction and the Scene lifecycle service. Existing public
consumers remain part of validation.

## Regression evidence

`HoroRuntimeRestoreReferenceTests` covers production Gameplay adapter receipts,
Scene readiness while owners remain pending, valid deferred cycles, invalid allocation
cycles, deterministic ordering, explicit remapping, exact generations, required
absence rollback, legacy-owner rejection, real component payload fixups, real cooked
asset loading, active Gameplay services and lifetime pins, operation completion and
pending shutdown. The suite and affected existing Scene/save/Gameplay regressions
must pass before delivery. No test execution is claimed by this document alone.
