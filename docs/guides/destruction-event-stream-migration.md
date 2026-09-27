# Destruction event stream and application adapter migration

`HoroDestructionRuntime` now owns a finite, scene-scoped committed-fact journal;
`HoroDestructionApplication` owns safe-point fan-out through cooked bindings. The new
public headers are `DestructionEventStream.h` and `DestructionEventDispatcher.h`,
respectively. No prior public event stream exists to preserve or deprecate.

The aggregate owner creates one stream per `DestructionWorldId`. During transition
planning it calls `DestructionEventDispatcher::Preflight` for required destination
capacity and `DestructionEventStream::Reserve` to validate and copy the complete
planned fact batch before commit. A cancelled or failed transition discards the
move-only reservation, calls `CancelRequired` on the preflighted adapters, and
publishes no fact. **Only after** the
complete Scene/Physics/Render aggregate root has become queryable does it call
`Publish` with the exact current source generation and committed revision. `Reserve`
must receive the canonical pre-commit revision for that source and generation;
the planned facts must carry its exact successor. The finite journal does not
keep a second per-source revision table: retained facts can wrap or interleave,
so they cannot establish source monotonicity. This
is an explicit owner precondition, not something an event journal can infer from a
standalone `DestructionDamageRuntime` value. The DFR-003.4 aggregate owner is not yet
present in this checkout and must wire this seam when it lands; this change does
not claim that production transition publication already occurs.

An application session constructs the dispatcher from a cooked, immutable finite
binding table. It supplies borrowed adapters only at preflight and the application
safe point, never at destruction commit. A required destination must reserve work
before source commit; its later admission failure faults/pauses delivery while the
fact remains in the journal. Optional cosmetics may fail or be omitted in headless
mode without changing canonical state. Requests carry copied values and an exact
occurrence/binding/destination/layer identity for destination-owner deduplication.
The dispatcher itself retains no native handles or adapter pointers.

On `Gap`, consumers must capture a complete canonical snapshot and install its
associated cursor through `Reconcile`. They must not infer missed gameplay output
from the ring, use the optional data bus as an authority, or replay saved facts on
late join. Replacement uses new world/destructible and binding generations;
shutdown closes admission while the existing journal prefix remains readable.

Consumers linking only `HoroDestructionRuntime` see the journal but not the
application adapter contract. Application composition links
`HoroDestructionApplication`, whose only public dependency is the runtime contract.
