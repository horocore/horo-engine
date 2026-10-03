# Internal Module Descriptor Contract

## Purpose

This document defines the metadata boundary used to describe built-in Horo
modules before an application host composes or activates them. The contract is
owned by `HoroFoundation` so runtime, backend, application, presentation, and
headless targets can describe themselves without depending on a concrete host.

The C++ contract is `Horo/Foundation/ModuleDescriptor.h`.

## Inert Descriptor Boundary

`ModuleDescriptor` is owned value metadata. Constructing, copying, moving, or
validating one must not register services, inspect global state, select a
backend, allocate a runtime object, or invoke a lifecycle callback. A descriptor
may name paired activation/deactivation entry points, but only a later
host-owned composition stage may call them.

Runtime UI binding provider type schemas follow the same rule under
[ADR-079](../../adr/079-runtime-ui-binding-provider-schema-identity-and-lifetime.md).
A module descriptor may declare a bounded provider contribution, but parsing or
validating it cannot activate an instance, publish a registry entry, inspect Runtime
UI or grant read/write capability. The composition root validates/publishes the
batch and retains module callback/registry leases through revocation and unload.

Production modules use the same shape regardless of target role:

| Target role | Descriptor representation |
|---|---|
| API/model | Stable module ID, contract version, provided capabilities |
| Runtime/application service | Required modules and capabilities, budgets, observability, owned error domains |
| Concrete backend | Backend-specific module ID behind backend-neutral capability IDs |
| GUI/CLI/MCP adapter | Optional presentation module requiring application capabilities |
| Headless/null implementation | Ordinary provider of the same backend-neutral capability |

Module IDs and capability IDs are canonical lowercase namespaced identifiers.
Budget and observability IDs additionally live below their owning module ID.
IDs remain stable when CMake target names or source-tree locations change.

## Version And Dependency Rules

Every descriptor owns a semantic contract version. A dependency names a module,
the minimum accepted contract version, and whether absence is required or
optional. An optional dependency creates an ordering edge when present; it does
not make the graph invalid when absent. A present dependency below the declared
minimum is invalid regardless of optionality.

Capability requirements are separate from target dependencies. At least one
selected module must provide every required capability. Each selected provider
precedes the consumer in the validated graph; later composition policy decides
which approved capability bindings are injected.

## Validation Boundary

`ValidateModuleGraph` runs before registration or initialization and returns a
typed `Result`. It rejects:

- malformed descriptors and unpaired lifecycle callbacks;
- duplicate module identities or descriptor-local entries;
- malformed or conflicting error-domain ownership and duplicate error codes;
- missing required modules and incompatible dependency versions;
- missing required capabilities;
- cycles introduced by module or capability edges.

Successful validation returns a deterministic provider-before-dependant order.
Independent modules are ordered by stable module ID, so the result does not
depend on descriptor input order.

Validation does not prove trust, permissions, feature flags, or host resource
policy. Those remain later host-owned gates.

## Composition-Time Registration

`Horo/Foundation/ModuleHost.h` defines the host-owned registration and
activation stage that consumes this descriptor contract. A composition root:

1. Registers each selected descriptor explicitly with `ModuleHost::Register`.
   Registration is inert: it performs only local metadata checks and never
   invokes a callback or inspects the full graph.
   The composition root may also pass owned settings metadata through the
   `ModuleHost::Register` overload. This validates setting ownership and
   conflicts immediately without changing `ModuleDescriptor` construction or
   validation semantics.
2. Calls `ModuleHost::ActivateRegistered`, which validates the complete graph,
   builds an immutable candidate error-code registry, then activates modules in
   the validated provider-before-dependant order,
   passing a composition-root-supplied dependency bundle through
   `ModuleActivationContext::Bindings`. Modules receive only approved bindings;
   there is no runtime discovery.
3. On activation failure, the host deactivates every module started by that
   activation call in reverse order, so a failed composition leaves no partially
   active set from that call while preserving modules active before it. Modules
   reached by the failed attempt enter one terminal state. A graph rejected before
   activation leaves registrations intact so composition can be retried after
   correcting the descriptor set.

Registry validation occurs before lifecycle callbacks. If it fails, the host
keeps all affected modules `Registered` and preserves its previously published
registry snapshot. A successful incremental activation publishes a new snapshot;
active readers may retain the old snapshot safely.

Headless compositions stay headless by construction: they simply do not
register GUI-only descriptors, so GUI modules are neither activated nor linked
into the composition path. `DeactivateAll` provides idempotent reverse-order
teardown; attached module instances are released with their activation
contexts.

## Supported Host Profiles

`apps/common/HostModuleComposition.h` is the non-installed contract shared by
the two application composition roots. It translates the modules actually
linked into each executable into the descriptor graph consumed by `ModuleHost`:

| Host profile | Required participation | Optional selection |
|---|---|---|
| `horo-engine` | Foundation, Application, CLI host | None in the current implementation |
| `HoroEditor` | The linked application, runtime, scene, asset, input, gameplay, editor, extension, GUI, and renderer-neutral modules | Exactly one compiled interactive renderer and viewport adapter; OpenTelemetry when linked |

The profile is validated and activated before the host creates platform windows
or presentation resources. An impossible profile therefore fails before any
module activation, and a valid profile produces the same provider-before-
dependant order from the same selection. Concrete renderer choice remains in the
`HoroEditor` composition root; feature modules receive only the resulting
backend-neutral services.

## Module Lifecycle And Shutdown

`ModuleHost` owns the explicit per-registration state machine:

```text
Registered -> Activating -> Active -> CancellationRequested -> Draining -> Stopped
                      \-> Failed
Registered -----------------------------------------------> Stopped
```

`Stopped` and `Failed` are terminal for an identity registered with one host.
Graph validation rejection does not advance `Registered`; an activation callback
failure marks the failing module `Failed`, stops modules already started by that
attempt, stops unattempted registrations in the rejected attempt, and preserves
modules that were active before the call.

Each `ModuleActivationContext` owns a cooperative cancellation source and a
callback-admission gate. Asynchronous module callbacks acquire a move-only
`ModuleCallbackLease` before retaining activation-scoped bindings. Shutdown:

1. requests cancellation and closes callback admission for every active module;
2. visits modules in reverse validated activation order;
3. waits for every admitted callback lease to be released;
4. invokes the optional module drain callback;
5. invokes deactivation and releases the activation context and attached instance.

This order keeps providers alive while dependants drain, prevents new callbacks
from entering after shutdown begins, and ensures activation-scoped binding borrows
end before their context is released. A callback that does not cooperate delays
shutdown; the host must not trade a bounded wait for releasing a dependency that
an admitted callback can still borrow. Repeated shutdown is a no-op after the
first terminal transition. The host destructor is a final safety net that runs
the same idempotent shutdown path.

## Ownership And Migration

The caller owns the descriptor collection passed to validation. The returned
graph owns copies of module IDs and does not borrow descriptor storage. Callback
function addresses, when present, must remain valid for the later composition
lifetime; the descriptor does not own callback state.

Error-domain contributions borrow descriptor pointers only until registry
construction. The registry owns copies of descriptor text and module ownership
identities. Modules must not rely on registry indices or numeric allocation;
textual domain/code pairs are the stable serialization contract.

Existing built-in module-specific descriptors may remain at ABI or package
boundaries. Composition adapters translate their stable metadata into this
internal contract rather than making `HoroFoundation` depend on gameplay,
renderer, GUI, platform, or transport types.

## Verification

Regression coverage must prove deterministic ordering, missing requirements,
version incompatibility, duplicate identities, cycles, optional absence, paired
callbacks, error-domain ownership/duplicate rejection, transactional registry
publication, and the invariant that validation never invokes lifecycle callbacks.

## Related Documents

- [System Design](./system-design.md)
- [Ownership And Resource Lifetime](./ownership-and-resource-lifetime.md)
- [Error And Diagnostics](./error-and-diagnostics.md)
- [Runtime Lifecycle](../runtime/runtime-lifecycle.md)
