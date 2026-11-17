# Durable Gameplay Scene restoration

Gameplay restoration prepares a new BehaviorRuntime against the actual unpublished Scene. The existing SceneRestoreBundle and StagedRestoreTransaction remain the only publication transaction. Preparation may construct a changed or resurrected behavior inventory; it does not borrow old instances or restrict restoration to the currently live inventory.

Ordinary Play startup retains its existing order: the Scene candidate owns a constructed inactive runner, publication installs that owner, and ActivatePublished runs ordinary OnCreate/OnEnable with real published service scopes. Durable candidates reject that ordinary activation entry point.

Before a durable Save under the explicit no-candidate-services profile, the host calls ValidateKnownNoServiceRestoreRequirements on the actual active runtime. It rejects known missing prehooks and all non-None/unknown service requirements before invoking the capture writer. It does not run PrepareInactive or restore callbacks and cannot attest interfaces on a future prepared result. Initial source binding and all later actual-result checks remain required.

For durable loading, each actual behavior factory object implements IBehaviorRestorePreparation. Its declared service requirements are checked against actual prepared scopes. PrepareInactive initializes only transient inactive state and runs before durable materialization. The actual registered persistence source implements IBehaviorCandidatePersistenceSource and binds through the installation-owned GameplayPersistenceAdapter to a revocable target. The actual prepared native result must implement IInactiveBehaviorPersistenceState. MaterializeInactive applies its fields to those actual unpublished instances before the aggregate reference graph is built and before native FixupRuntimeReferences. Publish transfers visibility/ownership only; it cannot postpone field application or replay initializer callbacks.

## Lifecycle schema

The required participant `horo.gameplay.lifecycle.v1` uses participant schema 1 and record `2dfe15e0-b72b-43e6-b508-9b8ca8a07301`. Its existing canonical codec representation is a UInt32 version followed by a sequence of records. Fields 1–8 are persistent entity UUID bytes, entity generation UInt64, behavior instance UInt64, behavior type UTF-8, created Bool, enabledCallbackActive Bool, started Bool, and current enabled Bool. Entries are ordered by entity, instance and type; per-object duplicate instance IDs on different entities are supported. duplicate full attachment identities, missing or changed attachments, invalid flag relationships, unknown schema/fields and trailing bytes fail before publication. The profile admits at most 16,384 attachments, 8 MiB encoded bytes, 16 MiB decoded memory and 32 MiB read work.

The flags come from the actual engine runner, not authored defaults or a project restore hook. A disabled attachment stays disabled. A saved started=false attachment receives its ordinary first OnStart on its next enabled fixed update; started=true never replays OnStart. No ordinary OnCreate or OnEnable runs after durable fields are materialized. A save without this required lifecycle participant cannot use this new durable Scene profile; the archive compatibility owner must explicitly migrate older schemas or reject them. No historical archive flags are invented.

## References and ownership

Optional reference inventories come from the actual native prepared receipt. They use SDK/Save value identities only, are bounded before host copies, and feed the same Scene-owned reference graph. The source attachment is checked against the actual candidate entity mapping. Duplicate, foreign-owner, missing-source, wrong-type and unresolved required references fail before the gate. Fixup receives only that participant's immutable resolved results. Native code must copy stable values and must not retain borrowed views or engine pointers. Service references require actual pinned service ownership; absent service scopes grant no authority.

Binding(type,map) and BindingForActive(type,map) derive a bounded immutable attachment identity projection from the actual bundle/current canonical map. Each visit rechecks the actual entity/instance/type inventory and current runner/module admission. Unknown structural identities fail before capture instead of creating IDs.

One existing registration owns one private SourceSlot. Capture adapters keep that slot through Save → Load → Save; candidate publication swaps its source without a second registry, postcommit rebinding or state copying. Issued targets borrow weak module admission and a checked runner generation/epoch, preventing ownership cycles and reuse after Stop or same-Scene reload. Actual runtimes, installations and native receipts retain code pins. Native sources and factories are destroyed before their module library unloads.

Reloadable Scene policy requires actual native IPublishedGameplayPreparationRetirement before the gate. After old Scene/runtime retirement and outside the publication gate/mutex, the host may release only positively proven transient Gameplay receipts and their participant snapshot pins. Active Scene/core/Physics/identity/reference/service owners remain retained. A cleanup exception or allocation failure preserves the committed Load result and published Scene, retains unretired pins, and records a diagnostic for explicit retry. Reload stays busy until the proof succeeds. Snapshot/capture registries and editor packet leases must also be released before the existing module reload transaction can unload the old generation.

QuiesceForReload and ResumeAfterReload replace the runner in the same Scene-owned slot through that existing reload authority. The runtime-only reload snapshot carries the exact EntityRef in addition to instance/type/payload/started. It is not a save wire or native SDK exported structure. Lifecycle flags are matched by actual entity, attachment ID and type before any new-generation callback; equal counts alone are insufficient. Old target handles remain revoked after replacement.

## SDK and validation scope

The optional restore interfaces are separate contracts; existing IPersistenceSource/IPreparedPersistenceState vtables and boundary version 25 are preserved. New host/module consumers must be built from the matching headers because engine/private owner layouts and integration contracts change. This is not a claim that arbitrary old C++ module binaries can participate in durable restoration.

The native module regressions use the actual dynamic module, registration, installation, Scene candidate, canonical capture, reference graph and existing gate. The external draft has not yet been compiled or executed; root-controlled serial qualification is required before acceptance.

## Scene identity continuity

Before tearing down a settled Play Scene service, the controller observes its owner-lane NextRuntimeId cursor and carries that exact value into the next service. Complete private Scene aggregates consume IDs even when later publication is rejected; an early preparation failure does not. The cursor does not allocate, reserve, or issue an identity. Zero means exhausted, so hosts preserve zero instead of wrapping or inventing a fresh seed.
