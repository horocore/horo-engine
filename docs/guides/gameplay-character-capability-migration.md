# Gameplay Character Capability Migration

This change moves the standalone current-main native Gameplay boundary from SDK9
to SDK12. `GameRuntimeContext` appends an owned Character holder and the behavior
backend vtable appends a Character accessor. Native modules and generated descriptor
bundles must rebuild against the matching engine/compiler/platform/configuration
`gameplay-sdk12` fingerprint. Incompatible descriptors are rejected before factories.
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

SDK12 is the independently developed Character layout. Navigation SDK10 and AI
SDK11 describe different unmerged layouts, not compatible alternatives. After
another branch merges, assign a new epoch (13 or later) to the combined layout,
update fingerprint/matrix/consumer fixtures coherently, rebuild modules and repeat
exact-main validation. Taking the greatest old number does not identify the union.
Retain a coherent previous engine, module, generated bundle and project backup for
rollback. Do not edit fingerprints or force-load incompatible code.
