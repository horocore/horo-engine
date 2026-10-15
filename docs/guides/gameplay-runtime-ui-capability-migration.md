# Gameplay Runtime UI capability migration

HORO-753 / #753 / [RUI-006.7] adds explicit gameplay and Lua access over the
canonical Runtime UI owners. This is the combined native SDK19 layout preserving main Character SDK15 and Navigation SDK10.

## Composition and ownership

The application creates `RuntimeUiService` by moving its actual
`UiSceneReconciliation` registry into the facade. Admit complete `UiHotReload`
publishers through that service, then issue an exact `RuntimeUiGrant`: module ID,
Scene ID and activation generation, instance, canvas, independent permission bits
and at most sixteen writable provider incarnations. The application alone owns
the registry/factory and any cancellation source supplied to `Issue`.

Pass the issued client through `GameplayWorldSelection::runtimeUi`,
`GameModuleHostOptions{capabilities, GameModuleRuntimeServices{.physics = physics, .runtimeUi = runtimeUi}}`
or `BehaviorRuntimeServiceBindings::runtimeUi` at the behavior factory. Existing Physics
constructors forward into the same options authority; `{}`, `{{}}` and `{{}, {}}`
callers retain their meaning. There is no ambient lookup or module registration
side effect. Absent native `GameRuntimeContext::runtimeUi` means the caller returns
`UiErrors::CapabilityUnavailable`; Lua returns the same code/domain as a nil/error
pair. Unsupported transport values/cutoffs return `CapabilityUnsupported`.

Each native module, behavior runner and Lua VM derives a separate child. Public
`Derive()` accepts no caller cancellation token, allocator or callback. It inherits
immutable parent cancellation and is bounded to eight scopes in the chain. Its
immutable private issuer and all scope/client control blocks are created in the
host image. Module-facing context replacement does not replace the host's
independent authoritative holder. Revoke before retirement callbacks, release
holders before image unload, and retain provider/module callback leases for each
synchronous borrowed dispatch. Queued terminal errors are owned `Error` values,
not descriptor pointers; no module callback is retained by a queue.

Successful source reload revokes old VM scopes before their next callback. Failed
source replacement preserves old admission. Successful UI replacement invalidates
exact generation leases; failed preparation/commit preserves the old generation.
The service delegates aggregate Scene transitions to the canonical registry.
Presentation leases can retain retired immutable data; `CollectRetired` remains
available after `Shutdown`, and `CanReclaim` reports actual remaining leases.

## Safe points and finite work

`OwnerCommands` applies screen/action admission at the lifecycle cutoff.
`BeforeVariableUpdate` applies provider deltas and explicit provider withdrawal
through the existing binding/layout owners. Providers must already be stopped
before withdrawal. Required bindings become unavailable and optional bindings use
the canonical fallback policy; the application retains required-UI failure policy.

There are 256 weak scope entries, sixteen command slots and sixty-four copied
events per scope, sixty-four canvas dispatchers and 128 pending action attributions.
A terminal command reserves its slot until `Poll` consumes it. Requests are
scope-local monotonically increasing IDs and are never reused. Actions are
attributed by exact instance/canvas/request identity. Revocation is rechecked
before borrowed consumer dispatch. Full event mailboxes fail before any consumer
side effect. Reentrant owner operations fail `CapabilityBusy`; shutdown requested
inside a borrowed callback is completed only when that callback returns.

Provider updates contain at most sixteen property changes and 4096 aggregate
variable bytes. Binding reads copy at most 4096 variable bytes. No provider,
renderer, native input, mutable Scene or VM pointer crosses the client boundary.
The scalar lookup/screen-admission regression measures allocations; no general
performance claim follows from it.

## Closed Lua transport

The sandbox exposes `ctx.ui`; each method uses the same host-issued client:

| Method | Inputs and result |
| --- | --- |
| `find(id)` | Exact 32-hex authored element ID; opaque copied resident handle. |
| `read(id)` | Exact binding ID; opaque closed binding value with `:unpack()`. |
| `screen_guard()` | Opaque exact copied stack guard. |
| `screen(kind, route, guard)` | Canonical operation kind and optional 32-hex route/opaque guard; opaque request. |
| `publish(provider, major, minor, fingerprint, expected, revision, changes)` | One-based granted provider index; complete schema/revision evidence; dense array of `{zeroBasedProperty, bindingValue}` pairs; opaque request. |
| `source(element, interaction)` | Opaque resident element and exact interaction revision; opaque copied action source. |
| `action(source, kind, action, operation, payload)` | Canonical command kind, 32-hex action, family-specific operation and dense bounded action values; opaque request. |
| `poll(request)` | Nil while queued; opaque retained terminal record on completion; nil/error for stale/revoked admission. |
| `next_event()` | Nil when empty or opaque retained copied event. |

All unsigned 64-bit scalar evidence uses exact decimal strings; nonnegative Lua
integers are accepted as inputs. Binding tags follow `UiBindingValue` variant order
(0 through 12); action tags follow `UiActionValue` (0 through 8). Text factories
copy bounded bytes. Non-finite values and out-of-range float narrowing are rejected.
Opaque handles compare by typed copied evidence and cannot manufacture authority.
No raw table metamethod is invoked while reading payloads; sparse/extra keys fail.

A terminal record's `:unpack()` returns the native variant index followed by:

- Screen: outcome, rejection, revision, operation ownership, operation sequence,
  operation kind, optional opaque route incarnation.
- Binding: update revision, targets changed, required unavailable, content revision.
- Action: sequence and ownership.
- Error: nil plus the canonical code/domain table.

An event's `:unpack()` returns target handle, kind, sequence, logical x/y,
position-valid flag and optional closed eight-field gesture tuple. Both observation
records can be unpacked repeatedly, so projection failure does not consume their
owned value. All potentially allocating Lua output storage/method/stack admission
is reserved before native queue admission or consume-on-observation. Success then
moves only owned native values into initialized userdata. Garbage collection never
sees uninitialized nontrivial storage.

Lua allocation runs under the existing metadata boundary's protected-call pattern:
nontrivial owners live outside `lua_pcall`; inner marshallers borrow through POD
captures. Native exceptions unwind normally after the protected call; all owners
leave scope before a final `lua_error`. Per-allocation tests cover context creation,
binding projection, provider decode/request reservation and terminal/event reserve,
including Lua's allocation retry. Full runtime source reload and VM shutdown use
the actual behavior compiler/registry/runner. Tests require execution evidence;
authored tests or compile-only checkpoints do not establish runtime qualification.

## SDK and rebuild

Rebuild all native modules, generated descriptor bundles, SDK consumers and host
artifacts against SDK19 and its exact `gameplay-sdk19` fingerprint. The combined layout
preserves main Character SDK15 (including Navigation SDK10) and adds UI SDK17.
`GameRuntimeContext` appends UI after Physics/Navigation/Character. Host behavior factories accept designated `BehaviorRuntimeServiceBindings` fields;
no empty positional Character argument is needed when absent.
Host service aggregates preserve Physics/Character/Navigation and append UI; use
designated fields. Old SDK10/15/17/18 layouts are rejected before factory activation.
AI18/20 identify independent layouts; a future combined UI+AI layout requires a fresh
epoch and complete validation, never the maximum prior number.

`find_package(HoroEngineGameplay REQUIRED COMPONENTS RuntimeUi)` imports the
existing Runtime UI target plus its reviewed Assets/runtime/text-shaping/Unicode
static closure. The current target includes HarfBuzz and Horo's private Unicode
library, so the optional component is larger than a values-only target. It does
not import/select renderer or input backends. The default Gameplay SDK remains
Foundation/NetworkApi/SaveApi/GameplayApi only. Physics and RuntimeUi can be requested
together without duplicate common-target ownership. The external SDK fixture
compiles a real module and the host observes its retained client after actual
native image unload; metadata import success alone is not that proof.

Rollback requires restoring the previous complete engine, SDK, rebuilt native
module artifacts and matching manifests together. Never edit fingerprints to
admit old binaries. Source rollback removes UI context/constructor use and optional
component requests; no second compatibility authority is retained.

The private Navigation context marshaller participates in the same protected
context-construction boundary. It borrows a VM-owned cancellation token created
outside the Lua C frame. GC metatables are completed before userdata ownership;
Navigation userdata records whether placement construction completed, and UI
transport default construction is statically required to be nonthrowing. A prior
OOM cannot leave a partially initialized metatable reused without its GC handler.
Per-allocation regressions retry context construction in the same VM and release
both UI and Navigation holders on shutdown; terminal/event unpack failures retain
their copied observation for a successful retry.

Runtime UI service command and lifecycle methods remain mutable-only. Keep a mutable
application service reference for admission, issuance, queue application, dispatch,
reload/transition/provider publication, collection, and shutdown; a const service
reference grants observations only. The pimpl representation does not make those
commands logically const. The architecture contract records the eleven exact
S5817 exceptions and paired compile-time authority checks. Lua allocator opaque
pointers follow `lua_Alloc`; Runtime UI callback exception containment remains
`noexcept` with errors propagated only after native catch frames unwind.

## Host behavior factory migration

The independent Physics, Navigation, Character and UI arguments of both
context-bearing `BehaviorRuntime::Create` overloads are replaced by the inert
`BehaviorRuntimeServiceBindings` value:

```cpp
BehaviorRuntime::Create(scene, registry, limits,
    {.physics = physics, .navigation = navigation, .character = character,
     .runtimeUi = runtimeUi});
BehaviorRuntime::Create(sceneService, registry, limits,
    {.physics = physics, .navigation = navigation, .character = character,
     .runtimeUi = runtimeUi}, prefabs);
```

Prefab admission stays separate and SceneService-only. The existing three-argument
bare-Scene convenience overload retains its explicit withheld Physics context;
missing fields in a supplied bindings value retain the previous explicit absent
scope semantics. Factories still validate Physics/Character Scene identity before
registry acquisition, then Navigation/UI identity, UI child derivation,
Physics-dependent Character child derivation and final activation in the same
order. Existing scope moves and rollback/revocation remain unchanged. Copying the
value shares holders; it does not derive or select services.

Affected callers are `GameplayWorldComposition`'s bare-Scene and SceneService
factories, Lua Physics/Character/UI lifecycle fixtures, Gameplay Character context
fixtures and Gameplay Navigation host fixtures. Simple editor and core-runtime
callers of the three-argument overload require no source changes. Host applications
using positional services must migrate to designated fields and rebuild/relink:
the two context-bearing C++ host factory symbols change, with no compatibility
shim retaining a second source of activation truth. The public prefab consumer
checks both new factories, rejects old positional signatures through dependent
concepts, and proves bare-Scene creation cannot accept prefab scope.

This repair does not change the native module ABI. `BehaviorRuntime.h` belongs to
`HoroGameplayRuntime` in `cmake/HoroPublicHeaderOwnership.cmake`; that host target is
absent from every build-tree gameplay SDK export in the root `CMakeLists.txt`.
The exported core `HoroRuntime` target is a distinct owner. The default SDK exports
Foundation/NetworkApi/SaveApi/GameplayApi, with separate optional Physics,
Navigation and RuntimeUi components. Their export sets do not expose this host
factory or its header. `GameRuntimeContext`, behavior/backend vtables, native
module descriptors, their field order and the SDK fingerprint definition are
byte-identical to the SDK19 repair head. SDK19 therefore remains the reserved
NAV+Character+UI module boundary; this source migration requires host callers to
rebuild, rather than inventing a new native layout epoch.
