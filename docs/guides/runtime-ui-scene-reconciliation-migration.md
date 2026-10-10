# Runtime UI scene reconciliation migration

HORO-772 introduces `UiSceneReconciliation` as the semantic lifetime owner of
actual `UiHotReload` publishers. The service does not own Scene publication,
provider execution, player/viewport assignments, renderer resources or input
adapters. Composition is explicit and works in ModelOnly/headless hosts.

## Host composition and transaction

Create the registry with the game runtime's `UiOwnershipGeneration` and finite
active, retired and prepared-transition budgets. Admit each complete private
publisher at an ADR-073 lifecycle cutoff with exactly one GameInstance, Player,
Scene or Viewport owner. Player and viewport IDs are host-admitted generation
handles; `SceneRuntimeId` is the exact Scene-issued runtime incarnation. Route,
band, visibility, input audience and attachments never select lifetime.

An instance that reads Scene providers also declares `providerScene`, even when
its semantic owner is GameInstance or Player. All Scene providers within that
instance must belong to that exact scene; multi-scene data uses separate UI
instances. The host must stop old provider admission/producers as part of its
Scene transition barrier; UI stores retain copied values and authority leases,
never ECS pointers. Do not replace a scene simply by deleting every displayed
canvas or transferring it to a viewport owner.

Before committing the Scene transition, the host resolves complete new provider
snapshots and Assets-backed runtime owners for every surviving instance whose
`providerScene` matches the outgoing scene. It passes those generations plus
complete incoming Scene UI to `Prepare`. Required provider/schema/asset or
incoming UI preparation failures reject the transaction and retain every prior
publisher. No provider lookup, I/O, callback or Scene mutation occurs in UI.

`PrepareSceneRebind` accepts the **same** cooked asset, document revision and
registry publication, while requiring freshly reserved runtime element namespaces
and advancing tree/layout/interaction generations. It validates actual binding
contracts: stable targets, property schemas, direction, limits and fallback stay
compatible; active Scene providers use new exact incarnations; non-scene provider
identities, revisions, values and write grants remain unchanged. Existing
`UiHotReload` reconciliation preserves compatible control drafts, focus, routes
and scroll state; transient input/capture/pending actions never migrate. Controls
whose actual value-binding source changes reset through the existing reload rule.
The semantic UI instance and its immutable owner never migrate.

For unload, prepare detached stores through ordinary `Unregister` on their exact
Scene providers. Optional sources publish their authored fallback; required
sources become unreadable. `Prepared::Result().requiredUnavailable` reports that
condition before publication. The host must reject it when product activation
requires those sources; explicit unload may instead keep persistent UI alive in
its fail-closed unavailable state. After unload, reattachment is explicitly named
by a subsequent replacement; a new scene is never inferred by provider proximity.

At `ApplyQueuedOwnerThreadCommands` or `CommitDeferredLifecycleChanges`, call
`CanCommit` immediately before the host's no-fail Scene publication and `Commit`
without intervening UI mutation. Every source/candidate, cancellation and capacity
check precedes the first change. The service swaps complete persistent generations,
retires only exact outgoing Scene owners and admits incoming Scene UI as one
bounded transaction. Changes requested after the owner-command cutoff use the
deferred lifecycle cutoff. Callers must not commit Scene state from a UI action
handler or provider callback. Failed/obsolete candidates preserve last-good UI.
Repeated unload with no remaining owners/providers succeeds without removing
unrelated state.

Use `Publisher(instance)` only as a synchronous owner-thread borrow. Use `Acquire`
for immutable whole-generation lifetime pins. HUD/viewport adapters continue to
own their association gates and consume matching successful presentation receipts;
a new runtime tree never inherits old input eligibility. Viewport replacement
remains viewport-owned UI retirement plus explicit reconciliation of foreign
attachments, not a Scene transition.

## Retirement and shutdown

Successful cutoff work allocates no storage and invokes no external provider.
Retired publisher slots remain bounded; exhaustion rejects before publication.
`CollectRetired` runs outside frame work, drains old write-authority cancellation
and checks the existing typed producer/layout/render barriers. Outstanding leases
and prepared candidates prevent reclamation. No wait, fallback allocation or
force-free bypasses a barrier. Shutdown has additional reserved slots sufficient
for all active instances, closes admission and retires every semantic scope
idempotently before Renderer, Assets, Input and other dependencies disappear.
Hosts retain the service and dependencies until `CanReclaim` succeeds.

Instance slots are host-issued monotonically and never reused across failed
admission, retirement or restart within the same owner generation. Preserve
`LastIssuedInstanceSlot` when recreating a registry in that generation. Prepared
candidates pin registry storage but do not grant mutation after shutdown.

## Inert Scene identity target

`SceneRuntimeId` moves from `RuntimeScene.h` to the existing `SceneIdentity.h`,
with its namespace, name, representation and behavior unchanged. That header is
owned exactly once by the new `HoroSceneIdentity` interface target. The target
contains only inert identity values, no runtime implementation, services or
callbacks, and uses the standard staged public-header boundary. `HoroRuntime`
and `HoroRuntimeUi` publicly consume `HoroEngine::SceneIdentity`; existing Runtime
and Scene consumers keep their transitive include compatibility. Direct identity
consumers should link the narrow interface instead of the complete Runtime target.
The Physics gameplay SDK export includes `SceneIdentity` because its existing
Runtime closure now publicly depends on that inert interface.
This avoids a RuntimeUi-to-Scene implementation dependency and a dependency cycle.
The independent identity and UI public-header consumers exercise this migration.

## Verification

The dedicated regression target uses real cooked/provider-loaded documents and
actual retained tree, route, focus, binding, layout, clipping and control owners.
It covers semantic scope preservation, exact scene retirement, aggregate incoming
activation, same-document provider rebind, stale/missing candidates, required
unavailability, unload/reattach, namespace exhaustion/admission, shutdown and
allocation-free successful publication. Existing hot-reload tests remain the
regression authority for reload compatibility and lease drain.

Build `HoroRuntimeUiSceneReconciliationTests`, `HoroRuntimeUiHotReloadTests`,
`HoroRuntimeUiSceneReconciliationPublicHeaderConsumer` and
`HoroSceneIdentityPublicHeaderConsumer` (automatic compile-only boundary), and
`HoroSceneIdentityContractConsumer`, then run their executable CTest registrations.
Configure headless and supported renderer compositions affected by the identity
target migration. GPU smoke tests remain opt-in and require actual execution.
