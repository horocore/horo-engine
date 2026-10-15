# AI foundation lifecycle qualification

This qualification joins the delivered GAI-001 identity, blackboard, task,
scheduler, scene and canonical-state contracts. It uses the real scene runtime,
Foundation JobSystem and owning AI implementation. It does not introduce a
backend, runtime API, SDK generation or alternate state authority.

## Acceptance and proof boundaries

| Requirement | Executable evidence | Scope |
| --- | --- | --- |
| Concurrent writes preserve owner authority | Two Foundation jobs stage separately owned batches from the same scene revision. Worker access to live state is rejected; the owner commits one batch and rejects the stale competing batch with the stable diagnostic. | Actual public scene capability; concurrency never means racing mutable scene storage. |
| Save/restore preserves values and retires transient work | Capture a non-default canonical value; cancel detached restore without mutation; commit a valid restore; reject old task completions and old write batches while admitting a new task. | Actual RuntimeScene and AiSceneRuntime, not a serialization substitute. |
| Destroyed agents cannot retain task or blackboard access | Destroy the actual entity at the Scene structural safe point, retire its exact AI owner, reject old task/write/read admission, and preserve the other live agent. | Public host composition calls both existing owning safe-point authorities explicitly. |
| Reload and shutdown invalidate old publications | Publish a different real scene incarnation; reject the old weak capability and late completion; shut down the replacement. | Public scene capability and actual scene-owned task lifecycle. |
| Bounded population and fixed-tick work | Admit 32 real agents, bind the scene scheduler over Foundation JobSystem, exhaust cooperative per-agent slices, enforce global evaluation/work/command limits, reject repeated allowance renewal and a 33rd agent, and cancel registrations on retirement/shutdown. | Finite functional stress; not a frame-time or throughput benchmark. |
| No owned observer survives publication teardown | Production `PrepareStructuralAgents` creates the actual `AgentRuntimeState` and its blackboard. Start the production-owned task, register a callback on that board, observe one publication, then invoke `ShutdownStateContents`. Verify cancellation, empty owner storage, expired snapshot, stale batch/token rejection and no callback in the next incarnation after releasing the borrowed context. | Owning internal lifecycle proof; not claimed as public observer-registration E2E. |
| Storage respects declared bounds | Exercise 128 keys, 128 observers, coalesced repeated staging and a complete commit. Measure owner-thread C++ allocation requests only around the prepared commit. Reject key/observer overflow and a collection beyond its 16-element inline bound without changing active canonical state. | Actual production-owned blackboard; zero commit allocation does not claim whole-process zero allocation. |
| Invalid input has stable diagnostics | Duplicate identities, invalid enum values, wrong defaults, stale batches/handles and capacity overflow assert the existing AIErrors families and preserve admitted state. | Boundary-specific typed diagnostics, not generic failure checks. |

The cases deliberately distinguish immutable authored data, detached candidate
data, scene-owned active state and copied snapshots. Cancellation and terminal
cleanup remain owner operations. No worker callback receives mutable scene or
blackboard storage.

## Owning internal observer boundary

The public `AiRuntimeCapability` supports task lifecycle and blackboard value
operations, but it does not expose observer registration. Adding a public getter
or test-only capability would widen that contract merely to inspect private
storage. Instead, the test composition provides a dedicated, non-installed
`HoroAISceneQualificationInternal` interface. It stages exactly the existing
`AISceneRuntimeDetail.h` into a build-only include directory and links its real
`HoroAISceneIntegration` owner. That header's remaining closure consists of
declared public dependencies. No repository-wide or source-directory include
root is published; no new API, friend, hook or production method is added.

The internal cases construct owned agents through the existing production
`Detail::PrepareStructuralAgents` and start their tasks through
`Detail::StartOwnedTask`. They invoke the highest available complete-publication
cleanup boundary, `Detail::ShutdownStateContents`, rather than manually resetting
a blackboard. The production call graph is:

```text
AiSceneRuntime shutdown / replacement / candidate retirement
  -> ShutdownStateContents
     -> CancelOwnedWork for every AgentRuntimeState
        -> cancel and retire task generation; claim and complete cleanup
        -> BlackboardInstance::TeardownAtBlackboardSync
        -> release owned task and blackboard storage

AiSceneRuntime::RetireOwnerAtSafePoint / DisableAtSafePoint
  -> unregister the exact scene-owned scheduler decision
  -> the same CancelOwnedWork boundary
```

The public scene destruction case explicitly composes entity destruction with
`RetireOwnerAtSafePoint`; it does not claim that RuntimeScene automatically
invokes AI retirement. Public scene destruction/capability tests and private
production-owner teardown tests provide complementary evidence. The latter does not manufacture a public
observer API, and independent BlackboardInstance tests are not substituted for
scene-owner cleanup evidence.

## Validation

Build `HoroAIFoundationQualificationTests`, then run its discovered CTests:

```bash
cmake --build build/skeleton --target HoroAIFoundationQualificationTests --parallel 1
ctest --test-dir build/skeleton -R '^HoroAIFoundationQualificationTests::' --no-tests=error --output-on-failure
```

The same executable belongs to the focused Windows build/CTest closure and the
normal headless/Sonar coverage registry. Broader qualification also runs the
existing AI, scheduler, Gameplay AI, scene and public-header consumer suites.
Record the exact source freeze, options, commands, discovered/executed counts,
skips and platform separately. Source review or successful configuration is not
execution evidence. A resource-limited or unavailable run remains incomplete.
