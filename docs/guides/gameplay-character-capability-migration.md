# Gameplay Character Capability Migration

This change moves the merged SDK10 Navigation native Gameplay boundary to SDK15
and preserves its context while integrating Character. `GameRuntimeContext` appends an owned Character holder and the behavior
backend vtable adds a Character accessor. Native modules and generated descriptor
bundles must rebuild against the matching engine/compiler/platform/configuration
`gameplay-sdk15` fingerprint. Incompatible descriptors are rejected before factories.
The default gameplay SDK keeps only forward declarations; consumers explicitly
request its existing `Physics` component and link `GameplayCharacterIntegration`.

## Host Composition

Create `GameplayCharacterContext` with explicit module permission and exact active
Physics/Character/Scene evidence. The factory borrows the selected world only for
admission. The resulting holder owns no world or Scene. Compose:

```cpp
GameModuleHost host{GameModuleHostOptions{
    capabilities, GameModuleRuntimeServices{.physics = physics, .character = characterAnchor}}};
```

The non-default options constructor preserves `GameModuleHost({})` and
`GameModuleHost({}, {})` source compatibility. Legacy Physics constructors are thin
forwarding paths to the same options authority. A host anchor may issue fresh
Character children for sequential module generations; the legacy Physics holder
still needs replacement after its own retirement. Never reuse a retired child.

`BehaviorRuntime::Create` appends an optional Character holder. Application
`GameplayWorldComposition` appends independent default-denied permission and an
explicit optional Character-world admission argument. Existing omitted compositions
return typed unavailable access and never create or tick a replacement world.
Pass the loaded module's child into a behavior runner; runtime/VM teardown closes
only its descendants. Character/Physics owners still retire in their declared order.

## Callers And Validation

Affected callers are native module Start/PrepareReload/Stop, GameModuleHost loading
and retirement, both BehaviorRuntime creation routes, native behavior backend
access, Lua program replacement/VM lifetime, application world composition and the
external Physics gameplay SDK fixture. All generated public-header consumers and
explicit SDK consumers must build with the new layout. Regression coverage includes
principal/dependency admission, isolated child and sibling cancellation, bounded
delegation, real queued-command closure, exact unsigned correlation, synchronous
Lua replacement, retained runtime leases and actual native-image unload followed
by final client and weak-registry release. Local and hosted execution results are
reported separately; authored coverage is not evidence that a test ran.

## Independent Layout Epoch

SDK15 combines the merged Navigation SDK10 layout with Character. Parallel AI
SDK16 and UI SDK17 describe different layouts, not compatible alternatives. After
another branch merges, assign a new epoch (18 or later) to the combined layout,
update fingerprint/matrix/consumer fixtures coherently, rebuild modules and repeat
exact-main validation. Taking the greatest old number does not identify the union.
Retain a coherent previous engine, module, generated bundle and project backup for
rollback. Do not edit fingerprints or force-load incompatible code.

SDK15 also replaces SDK12's module-emitted Physics batch and Prefab operation
allocation with immutable private host-bound issuers. These opaque cross-image
state contracts changed even though public handle sizes remain unchanged. All
SDK12 modules and descriptor bundles must be rebuilt; accepting an SDK12 binary
would retain incompatible control blocks or private dispatch layouts. The host
owns batch cancellation descriptors, operation cancellation sources and retained
queue/weak-entry destruction. Runtime UI SDK17 is a separate layout; any combined
layout requires a fresh epoch18 or later and complete consumer revalidation.

## Windows Allocation Qualification

The exact preparation failure sweep is unsupported with MSVC Debug iterator
proxies and is reported skipped by Catch's exit4 / CTest SKIP_RETURN_CODE4.
Windows Release must discover and execute that same case with no skip, failure or
error; ordinary Character Debug cases remain scheduled. The verifier rejects
zero/missing/extra/disabled cases and unexpected instantiated variants.

The Windows runner uses MSVC14.44 (VS17.14). Microsoft's pinned
[VS17.14 vector implementation](https://github.com/microsoft/STL/blob/vs-2022-17.14/stl/inc/vector#L759-L765)
allocates a container proxy in its noexcept move constructor. The actual prepare
path moves CharacterControllerRegistry into CharacterWorld::Impl, moving
GenerationalSlotStorage::entries_ inside two noexcept constructors. Injecting a
bad_alloc at that proxy allocation terminates rather than reaching Prepare's
catch. No iterator setting, production allocator or deadline is weakened.

The private issuers are immutable host-constructed `std::function` values. Their
callable manager and destructor originate in the host image, and the host-created
state/control block destroys them. No module supplies or replaces these routes.
This bounded admission path accepts the callable representation's possible small
allocation; no frame-path or performance improvement is claimed.

The registry-contention regression uses the existing owner-thread `ScopedObserver`
to pause at the first real stale-descriptor error allocation while the registry
lock is held. It does not inject an exception through a standard-library noexcept
constructor. The producer must return typed RejectedBusy without allocation before
the reader resumes; the reader must return HandleStale, and admission must recover
after lock release. This ordinary Debug case remains enabled on Windows. The
separate preparation rollback failure sweep remains mandatory in Release.
