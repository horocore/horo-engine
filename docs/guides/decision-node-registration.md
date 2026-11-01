# Decision-node contributions

Gameplay modules contribute decision nodes through the existing generated
gameplay bundle. `HORO_AI_NODE(MyNode, "4101")` discovers a native implementation
without a project-maintained registration list. The quoted decimal identity is
the existing `DecisionNodeTypeId`, independent of C++ names and source order.
`MyNode` implements `IBehaviorTreeExecutor`, accepts a call-scoped property span
in its constructor, and provides `static DecisionNodeContract DescribeDecisionNode()`.
Its constructor must copy any values it retains. The generator rejects malformed,
duplicate, zero and overflowing identities before publishing output.
The annotation supplies its actual compiler file/line as descriptor provenance
and fills omitted blackboard requirement provenance from that same annotation.
Missing or malformed source locations fail the canonical descriptor admission.

The owned contract contains the canonical `DecisionNodeDescriptor`, task,
condition or service role, stable typed ports, typed property defaults and the
existing `AiCapabilitySet`. Blackboard requirements use the existing schema
validator. Unknown implementations can retain their inert contract with an empty
factory, but cannot create an executable adapter. This does not mutate authored
graph data or turn unknown/opaque properties into executable values.

The exact gameplay SDK boundary and build fingerprint are now **24**. The runtime
bundle has bounded node records alongside behavior records; an old ABI is rejected
before those records are read. The exported gameplay SDK includes the actual
Foundation-backed AI library required by its public node contracts. It introduces
no GUI, physics solver or editor dependency.

## Acquisition and execution

`LoadedGameModule::AcquireDecisionNodes()` copies bounded metadata and retains
the actual loaded generation's admission alias. It never borrows an unpinned
project callback. A generation can create an `IBehaviorTreeExecutor` only on its
owner thread while admission remains open. Creation checks exact provider/version,
capability needs, blackboard bindings and property types before returning the
complete adapter. A failed factory rolls back already-created candidates.

Use the `BehaviorTreeExecutionPlan` overload for an executable tree: only tasks,
conditions and service attachments receive provider instances. Sequence, selector
and other core controls remain flat runtime records. The `DecisionAssetPlan`
overload binds a provider-only metadata plan, useful before executable topology is
available. Neither overload parses editor JSON or creates a scheduler. Existing
`BehaviorTreeInstance` and `AIDecisionSystem` continue to own evaluation and task
transitions.

The adapter rejects recursive/off-owner calls and new work after revocation.
It retains running operation identities for exactly-once abort and cleanup,
then invokes the actual generation-owned destructor. The application must retain
its module/retirement owner until `TryRetire()` succeeds. Native reload and package
retirement remain busy while executable generation leases survive. Releasing a
copied metadata value grants no authority; retaining an executable generation
deliberately retains its native/VM owner. Executors must be destroyed on their
owner thread.

`DecisionNodeGeneration::Compose()` combines a bounded set of acquired leaf
generations in stable type order. Duplicate types fail as ambiguous. It retains
every selected owner and observes each owner's revocation. Nested compositions
and nested provider parents are rejected, bounding admission checks. Package
version resolution stays with the existing lock resolver, before this operation.

## Lua nodes

`LuaDecisionNodeProgram::Compile()` generates the same contract from a source
table. The caller supplies the canonical sidecar/selection type, provider and
descriptor version; the script cannot replace them. One source generation owns
its admission signal, and each executable node owns a separate budgeted VM.
Only the existing supported Lua adapter is used.
Compilation also requires the actual captured chunk/source `SourceLocation`.
That identity is passed to Lua's loader and copied into the descriptor and its
requirements. Zero line/column report unavailable precision; the adapter does
not invent per-field source lines. Verified packages supply their declared
logical script path with that honest coarse location.

```lua
return {
  role = "condition",
  requirements = {{ key = "10", kind = 0 }}, -- existing Boolean kind
  check = function(self, context)
    return context.blackboard["10"]
  end
}
```

Task nodes provide `start`, `resume`, `abort` and `cleanup`; service nodes provide
`service`. Callback arguments are the instance table and a detached context.
Context contains exact decimal-string `node`/`tick` identities, typed property
values, schema-authorized frozen blackboard values, and `status` for terminal
callbacks. Unsigned identities remain strings to preserve all 64 bits. Entity
references remain inert scene/slot/generation values; asset references are exact
16-byte strings; world coordinates are three signed millimeter integers.
Collections are bounded flat arrays. There are no runtime pointers or mutable
document reads. This adapter supplies observations, not a new blackboard writer.

Tasks return the existing numeric `AiTaskState` values 1–4; conditions return a
boolean. Malformed metadata, sparse lists, unknown fields, enum wraparound,
non-finite values and wrong callback results fail with typed errors. Source is
bounded to 256 KiB. VM memory is at most 16 MiB and each callback has at most
1,000,000 instructions; project limits may be lower. Marshalling uses protected
raw Lua operations so allocation/VM errors cannot jump over live C++ owners.
Detached blackboard projection storage is bounded and reserved during instance
creation. Retiring a program closes new calls while its existing VMs remain alive
for abort, cleanup and destruction.

## Verified packages

`GameplayPackageModuleHost::AcquireDecisionNodes()` selects an exact live package,
module and activation generation. It returns generated nodes with the actual
locked package version and artifact digest. Missing, denied, stale or off-owner
selections fail; discovery order never selects a competing version.

`CompileDecisionNode()` reads only a declared `.horo_script` from that exact
verified module inventory. It delegates compilation to `GameplayLua` and retains
the actual native activation as the Lua provider parent. Disable/reload closes
callbacks, and the package cannot unload or delete staged resources until the
programs and executors release their real generation leases. Neither an extension
manifest role nor a generic script invocation registration substitutes for a
node implementation. Node descriptor versions and package versions are distinct.

The compiler/cooked-plan work in HORO-1342 remains separate. This feature does not
claim automatic authored graph lowering, subgraph execution or the future node
palette/editor integration.

## Focused validation

`HoroDecisionNodeRegistrationTests` exercises core admission and an actual
generated native module. `HoroLuaDecisionNodeTests` exercises real sandboxed Lua
execution and failure bounds. The `[packages][decision-node]` group in
`HoroPackageLifecycleTests` exercises verified archive bytes, locked attribution
and activation retirement. `tests/python/test_gameplay_descriptor_generator.py`
covers the actual producer. Final qualification results are recorded after the
complete source batch is built; early native-only results do not qualify Lua or
package integration.
