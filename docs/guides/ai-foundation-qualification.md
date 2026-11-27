# AI foundation lifecycle qualification

This qualification joins the delivered GAI-001 identity, blackboard, task,
scheduler, scene and canonical-state contracts. It uses the real scene runtime,
Foundation JobSystem and owning AI implementation. It does not introduce a
backend, SDK generation or alternate state authority. The commit-call migration below
removes a platform-specific allocation without changing runtime component layouts.

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

## Prepared commit ownership migration

`BlackboardInstance::CommitAtBlackboardSync` now accepts `BlackboardWriteBatch&&`
instead of constructing a by-value batch. The original defaulted vector move
allocates a container iterator proxy on MSVC Debug before entering the commit
body, violating the prepared-commit zero-allocation qualification despite the
pre-sized value and authorization scratch stores. The measured Windows failure
was one allocation; the zero-allocation assertion remains unchanged.

A private rvalue-qualified `ConsumeForCommit() &&` transfers the reset obligation
to a noncopyable/nonmovable stack-only guard through guaranteed prvalue
construction. The guard borrows the caller's batch on the owner lane; neither
it nor the vector is move-constructed. Its lifetime remains wholly inside
`CommitAtBlackboardSync`, and staged values stay alive through validation and
observer publication. At call exit the guard invalidates the binding and base
revision and clears all writes on every exit, including stale, invalid and reentrant rejection. Staged
authorizations are released at that boundary. The caller retains an empty
vector's capacity until its normal destruction; no new vector or proxy is
constructed during commit. Consumed batches reject further staging. Successful
publication still owns any authorizations copied into active values.

All existing direct callers already pass rvalues and need no source rewrite:
`AiRuntimeCapability::CommitBlackboardWrite`, scene perception publication, and
AI blackboard, decision-wake, perception, behavior-tree, state-machine and
foundation qualification tests/support. Rebuild the owning AI library and its
consumers because the C++ member signature changes; there is no SDK component
layout change. The outer public capability still accepts a batch by value, so
this qualification does **not** claim that capability transport or detached
batch preparation is allocation-free. Regression coverage checks consumption
on successful, invalid, stale and reentrant exits, authorization release on
rejection, and unchanged values, revisions and observer notifications.

## Offline evaluation manifests

Use `qualify_ai_evaluation.py` to export metadata from an existing headless
`evidence.json` produced by an explicit `--ai-replay-output` invocation. The
exporter reads files locally, starts no capture and makes no network requests.
Use only authorized evidence. A reviewed local approval must bind the SHA-256
of the exact replay bytes to the intended qualification tuple. The approval is
an operator assertion and belongs in the same access-controlled workflow as
the source evidence.

```json
{
  "schemaVersion": 1,
  "purpose": "offline-evaluation",
  "replaySha256": "<SHA-256 of evidence.json>",
  "expiresOnUtc": "<YYYY-MM-DD>",
  "qualification": {
    "provider": {"version": "1.0.0", "sha256": "<provider artifact SHA-256>"},
    "model": null,
    "platform": {"os": "windows", "architecture": "x64", "version": "10.0.26100"},
    "workload": {"version": "1", "sha256": "<scenario artifact SHA-256>"}
  }
}
```

Set `model` to the same version/hash shape as `provider` when evaluating a
learned model. The date is exclusive at midnight UTC and is copied to the
export. Keep the source and export in storage whose owner expires both by that
date. The exporter rejects expired approvals and has no authority over files
already retained elsewhere.

```bash
python scripts/qualify_ai_evaluation.py evidence.json --approval approval.json --output evaluation-manifest.json
```

The destination must be new and its parent directory must exist on a filesystem
that supports hard links. The exporter stages the redacted manifest beside the
destination, rechecks the approval before publishing, and preserves any existing
file at that path. Omit `--output` to stream the redacted manifest to stdout.
Both modes suppress publication if the replay approval is withdrawn, changed,
or expired by the final approval check.

Pass `--qualification-artifacts <directory>` to also check the provider, model
and workload files against the approved SHA-256 values. Store each file under
its lowercase digest, without an extension. A null `model` needs no file. The
checker rejects missing, empty, symlinked, mismatched or oversized files, and
files replaced during verification. The limit is 512 MiB per file. The manifest
still records operator-declared versions.

Before admitting a stored manifest for evaluation, check it against the
original approval using the current UTC date.

```bash
python scripts/qualify_ai_evaluation.py evaluation-manifest.json --approval approval.json --verify-retained
```

The check returns zero for a schema-valid manifest whose approval remains
current through verification. It rejects expired
approvals, mismatched replay digests or qualification tuples, and extra fields
outside the redacted schema.
When using a manifest as reproducible evidence, also pass `--source-replay evidence.json`.
This verifies the original replay bytes against the approved digest and
reproduces the redacted counts and frame provenance for comparison. The
verification emits neither raw replay values nor regenerated evidence.
The artifact check is also available with `--verify-retained` and
`--read-retained` when the original replay is supplied.

To compare two recordings under the same provider, model, platform and
workload tuple, use both current approvals and a trusted build of `horo-engine`.

```bash
python scripts/qualify_ai_evaluation.py recorded.json --approval recorded-approval.json \
  --compare-replay replayed.json --comparison-approval replayed-approval.json --engine /path/to/horo-engine
```

The native comparator checks each approved digest against the bytes it
actually compares. The tool writes a redacted result with both digests, the
qualification tuple and the earlier expiry. A match exits zero. Divergence
emits a result with status `diverged` and exits one. Invalid, incompatible or
expired inputs exit one without emitting a result. As with the native
comparator, a match applies to the recorded coverage. The optional artifact
directory can verify the tuple's provider, model and workload files.
Both approvals are rechecked before native comparison and before publishing
its result. Removing either approval or changing its approved content stops
publication.

Consumers that need the stored manifest as input can use `--read-retained`.
This emits the redacted manifest only after checking the current UTC expiry,
matching the original approval, and reproducing its evidence from the approved
source replay. Consumers can use the verified output directly without opening
the file again after a separate check.

```bash
python scripts/qualify_ai_evaluation.py evaluation-manifest.json --approval approval.json --read-retained --source-replay evidence.json
```

For retention automation, use `--audit-retention` with the same manifest and
original approval. It returns a schema-versioned JSON record with `status`
(`current` or `expired`) and `expiresOnUtc`. A matched record switches to
`expired` at the start of its expiry date in UTC. Missing, malformed or mismatched
inputs produce a nonzero exit and no status record.

```bash
python scripts/qualify_ai_evaluation.py evaluation-manifest.json --approval approval.json --audit-retention
```

The storage owner can explicitly purge an expired pair from a directory it
controls exclusively. Stop concurrent writers before invoking the purge. Pass
the manifest and source replay as distinct files in the same directory.

```bash
python scripts/qualify_ai_evaluation.py evaluation-manifest.json --approval approval.json --purge-expired --source-replay evidence.json
```

The purge checks the UTC expiry, matches the approval, and reproduces the
manifest from the source before removal. The source goes first so that an
interruption leaves the verified manifest available for retry. If the source
is already absent, retry verifies the expired manifest against the approval
and removes it. The approval remains available for audit. The purge affects
the specified directory entries and does not erase copies or backups.
A successful purge emits a schema-versioned status record without replay data
or paths. Check `sourceRemoved` to distinguish a removed replay from one that
was already absent at the supplied path.

The exporter emits schema 1 with decision outcome counts and exact source, scene,
canonical AI and plan digests. It omits IDs, paths, observation records and
Blackboard values. A replay window requires consecutive ticks for the same
Scene and subject. The native per-frame limit applies to each compact UTF-8 frame,
within the separate total window limit. The versioned provider, model, platform
and workload tuple
is marked operator-declared. Use separate reproducible qualification runs to
establish support for failure, bias, replay and offline behavior.

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
