# Decision graph compilation and cooking

`DecisionGraphCompiler` consumes captured, immutable behavior-tree and hierarchical state-machine source. It resolves the actual generated native/script contracts from `DecisionNodeGeneration`, the declared blackboard schemas, every explicit subplan reference, executable topology and storage before returning a `CookedDecisionPlan`. Compilation does not invoke provider factories. A missing implementation, incompatible descriptor/schema/capability, cycle, unreachable graph/state, unknown control or exceeded bound produces diagnostics and no executable candidate. The source remains available for inspection.

Controls are explicit `DecisionControlKind` values on the canonical source records. Intrinsics have no provider type identity; tasks, conditions and services use the real generated descriptor. Display names, arbitrary numeric ranges and provider type names never select a control operation. `Unspecified` preserves legacy inspectable source but cannot be compiled for execution. Utility graphs are an unsupported M4 family and are rejected; no BT fallback is used.

Each provider task has an authored persistent `TaskId`. An HSM source also declares its runner's persistent `executionTask`. Entry/update/exit actions carry their own provider node uses alongside those phase task definitions. The compiler never manufactures task definitions, provider IDs, asset identities or a second node registry.

## Execution ownership

The host instantiates a complete plan on its existing decision owner thread with an actual agent/operation context, a current blackboard binding and an exclusively reserved task-slot range. It then calls `DecisionGraphInstance::Step` at committed increasing ticks with a frozen `BlackboardSnapshot` and bounded events. No scheduler, Scene authority or `AIDecisionSystem` is created by this API.

The full closure uses one admitted agent schema. Every compiled member retains the same immutable schema lease. Before evaluation, the runner compares the snapshot's actual retained schema lease and all agent/schema/instance binding fences. Merely matching an ID and version cannot admit a different schema object. A cooked file carries no recovered runtime authority: loading requires the host's actual current schemas and executable generations.

Every call occurrence receives independent state, including repeated calls to the same asset from parallel branches and calls in mutually exclusive HSM states. Plan bodies are shared immutable data. All possible occurrences are provisioned before evaluation. A child advances at most once per committed tick; same-tick internal revisits observe its cached result. Re-entry resets that occurrence's bounded subtree while preserving nonwrapping task generations.

BT calls use the existing BT traversal. HSM calls follow the existing hierarchy lifecycle:

1. Entry actions run ancestor first; the called child starts at the state's update boundary.
2. Update actions run before the child step. Child outcome guards are evaluated before transition selection.
3. Normal exits cancel and clean the child before that state's exit action, descendant first.
4. An authored terminal leaf completes entry, then publishes its declared success/failure without update or outgoing transitions.
5. Owner cancellation/revocation cancels and cleans children without invoking new state actions.

HSM actions are synchronous one-shot intents. Their actual task lifecycles must terminate and clean up in that invocation; `Running` is rejected and cancelled rather than leaving unowned work. BT task leaves retain the existing cooperative task/continuation lifecycle. Failed actions terminate the logical runner; the runner does not claim to undo already emitted gameplay intents.

## Storage and limits

Hard ceilings are 128 graph assets, 4,096 source nodes across the closure (1,024 per graph including action uses), 16 call levels, 256 call occurrences, 16,384 combined kernel-depth units and 1,048,576 reserved task slots. Product limits may lower these values. Core storage is bounded at 64 MiB; declared provider/quota storage at 256 MiB. Source and cooked transports are limited to 16 MiB, diagnostics to 4,096, and the cook adapter's retained prepared payloads to 64 MiB.

Storage reports account for retained core container extents, immutable plan/source/call tables, independent occurrence adapters, actual kernel state and traversal scratch. BT slot spans follow actual task positions; a control-only local plan may require zero task slots. HSM occurrences reserve a runner slot plus their action-provider slots. Combined depth and storage are checked over the whole occurrence tree, so repeated parallel calls cannot share one mutable allocation or escape the aggregate budget.

Provider storage has separate guarantees:

- `UnknownNative` reports an unknown native provider count; it does not certify that provider's private allocations.
- `NativeDeclared` records an explicitly trusted provider declaration. It remains a declaration, not an allocator quota.
- `EnforcedVm` is admitted only through the actual Lua program's private generation-capture seam. It binds the captured Lua allocator quota and bounded engine projection/property storage. Generic/native registrations cannot claim this authority by setting an enum.

`CoreBound` permits explicitly unknown native storage. `IncludeTrustedDeclarations` requires declarations or enforced VM quotas. `EnforcedTotal` requires the declared logical core extent and enforced VM quotas, and rejects native unknown/declaration categories. Its name does **not** mean a process/RSS cap. Allocator headers, shared-ownership control blocks, compiler stack-frame layout, shared code images and caller-owned immutable leases are outside these logical extents. There is no process-total certification mode; a host requiring that guarantee must reject the unsupported request rather than map it to these policies. Arbitrary native callback allocation, blocking and execution cost remain the native provider's responsibility.

## Source and cooked transport

`DecisionPlanCodec::EncodeSource` writes canonical typed source; `DecodeSource` reads bounded records without partial output or runtime authority. Both source and cooked formats start at version 1. Counts, identities, closed enums, property representations, finite scalar values, versions, complete consumption and canonical ordering are checked. No raw object pointers, virtual tables, native heap state or VM objects are serialized.

The cooked envelope commits the canonical source, actual descriptor/schema/capability environment, runtime layout and complete storage declaration. Its SHA-256 identity is content addressed. `DecodeCooked` validates the envelope and recompiles the typed records once against the actual current environment, then compares the full canonical identity. Recomputed transport hashes cannot bless a forged layout. A different runtime layout/environment is rejected. This is load-time work; ticks do not read source files or parse editor graph JSON. Cross-build/layout compatibility must be admitted by this check, not assumed from a filename or target label. Migration/replay and editor palette work are separate contracts.

`DecisionPlanStore` publishes only complete admitted executable candidates on its owner thread. Invalid candidates leave its prior plan unchanged. Existing instances retain their exact plan/provider leases until cancellation and cleanup; publishing a candidate does not migrate running state.

## Real asset cooking

`DecisionAssetCookHost` composes the existing `AssetCookService`. The canonical source types are `core.behavior_tree` and `core.state_machine`, normally stored as `.horo_bt` and `.horo_sm` with ordinary committed `.horo` identity sidecars. Each source file contains one canonical graph envelope. Its 64-bit graph ID is decoded from that real file; the 128-bit `AssetId` comes from the actual validated source/sidecar pair. No numeric conversion invents a mapping.

The host request supplies the actual immutable file capture, registry, target, executable/schema environment and existing `validateHostInputs` callback. That callback must retain/revalidate the real project read, schema/settings and package/reload authority through joined publication. The adapter additionally verifies actual files and executable generation admission. It does not substitute a filesystem digest for a project lease or claim an operating-system atomic snapshot.

Preparation resolves each root's complete declared graph closure before output work. Captured source paths replace untrusted diagnostic filenames with honest coarse file provenance. Strategies retain only immutable prepared payloads and declare actual dependency `AssetId`s. Cache validation requires exact prepared bytes; settings commit prepared output identities, including byte-affecting diagnostic paths. Whole-capture cache identity is retained. The adapter rejects an unrelated caller key projection and composes optional existing non-decision cooker contributions through the ordinary sealed catalog.

Generation staging, cache admission, the writer lease and pointer replacement remain exclusively owned by `AssetCookService`. Preparation failure, invalid source, cancellation before replacement, provider retirement, source drift or failed publication cannot replace the previous complete generation. Post-commit outcomes follow the existing durable publication receipt semantics; this adapter adds no second transaction or rollback authority.

## Qualification

The full native macOS Debug batch passed 33 authored test functions and 5,490 assertions with no failures, errors or skips. The compiler/runtime/codec/provider binary executed 41 cases/sections and 4,932 assertions; the real-file cook binary executed 9 cases/sections and 558 assertions. All 20 new production implementation files compiled. The cases exercise actual generated native images, isolated Lua VMs and native source/sidecar/cache/publication files, including accepted-task cleanup after the native owner is destroyed.

This qualification covers the focused feature batch. It does not claim hosted CI, a broad engine suite, Windows/Linux execution, native GPU behavior or visual editor acceptance.
