# Gameplay runtime persistence migration

SDK boundary 8 combines `GameRegistrationContext::events` and
`GameRegistrationContext::persistence`. The two independently developed boundary-7
layouts are not interchangeable. Rebuild native modules and generated bundles
against the current SDK; boundary-6 and boundary-7 artifacts are
rejected before project factories run. No authored scenes or durable archive wire
formats change.

`Horo/Gameplay/PersistenceSource.h` is the project gameplay SDK callback contract.
`PersistenceRegistration.h` declares the explicit registration transaction.
`Horo/Gameplay/SaveGameplayPersistence.h` adds the host-side integration for
behavior instances, module-global state, services, and sessions. It is owned by
`HoroGameplayRuntime` and composed through the existing canonical participant
registry and `SaveParticipationClient`; no gameplay module is registered by
constructing a descriptor.

Project code that previously relied on scene authoring fields, native memory
copies, or hot-reload snapshots for durable state must provide an
`Horo::Gameplay::IPersistenceSource`. Its capture callback returns owned canonical bytes
at the save safe point. The host wraps the bytes with the stable module identity,
exact module version, and owner kind, then copies them into a bounded canonical
record. Archive workers see only the detached record. The source must encode
stable semantic values, not pointers, handles, jobs, queues, or allocator state.

During `IGameModule::Register`, project code supplies `PersistenceRegistration`
through `context.persistence`. Behavior records name an existing generated
`BehaviorTypeId`, service records name an existing `GameplayServiceId`, and
module-global/session records carry no other owner identity. The host freezes
these declarations before service activation and module `Start`; missing owners,
foreign module identity, duplicate participant/record ownership and malformed
metadata reject startup without invoking capture or restore. The source owns the
runtime state it exposes and must not borrow a shorter-lived behavior or service.

The composition root calls `LoadedGameModule::AcquirePersistence(participant)`
to create an adapter with the loader's exact generation lease, then registers its
`CanonicalStateParticipantDescriptor` through the existing
save participation client, and retains the adapter for restore construction.
For restore, it passes validated archive record bytes to `StageRestore` and
supplies the resulting receipt to `StagedRestoreTransaction`. Decode and
compatibility checks run before inactive preparation. Publication is a no-fail
transfer at the aggregate lifecycle commit. Closing participation removes new
registration authority; already accepted capture snapshots and restore receipts
retain the module lease until their work retires.

Acquired adapters, pinned registry snapshots and restore receipts prevent native
reload retirement. `PrepareReload` closes new acquisition and returns
restart-required until those owners retire. Destructors of prepared state and
sources run before the module lease can release the code image. Host composition
closes participation and retires operation owners before retrying reload or
shutdown; it does not force unload across these leases.

`HoroEngine::SaveApi` now owns the existing `SaveErrors.h`, `SaveIdentity.h` and
`SaveParticipantRegistry.h` contracts and their implementations. GameplayApi's
exported SDK depends on this Foundation-only target, avoiding a dependency on
archive, compression, storage or scene implementations. Existing Runtime callers
receive SaveApi transitively and keep the same header paths and symbols.
`GameplayPersistenceAdapter` remains in GameplayRuntime. Public-header consumer
targets cover each owning boundary automatically, and native-module fixtures
exercise the exported GameplayApi dependency closure.

The initial compatibility policy is exact: participant schema, record identity,
module identity, module version, and owner kind must match. A saved record whose
module adapter is absent cannot be silently omitted from a restore plan;
`StagedRestoreTransaction` rejects unregistered staged participants. A newer
module must register a deliberate migration before old bytes can be loaded.
Optional participants may be absent from a save only when their registered
descriptor and canonical snapshot explicitly allow omission. Hot-reload state
is session migration, never an implicit durable participant.

## Callback exception translation

Capture and restore preparation build owned callback and allocation failure
results before invoking project code. Local `noexcept` guards move the matching
prepared result on standard, foreign or allocation exceptions. Error translation
therefore performs no allocations during unwinding, and result moves are checked
at compile time. Public signatures, error codes and module-generation lifetime
ordering are unchanged. Preparing fallback results is fallible setup outside the
callback guard; capture retains the existing outer host allocation boundary.
