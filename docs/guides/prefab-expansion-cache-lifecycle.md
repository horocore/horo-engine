# Prefab expansion cache and worker ownership

`PrefabExpansionCache` belongs to one owner thread. It memoizes immutable
`EffectivePrefabCandidate` leases, not live documents, runtime worlds or source
paths. A hit requires the root asset, scene occurrence, exact dependency graph
and source revisions, canonical source commitments associated with dependency
identities, registry revision and every field of `PrefabProjectPolicy` to match.
Placement transforms and containing-scene parents remain fresh Scene inputs.

The existing Scene schema has no instance override-set field. This cache does
not invent an override catalog or accept unmodeled overrides: currently admitted
authored values participate through the canonical source/Scene commitments. A
future override producer must extend the typed snapshot/key and its regression
coverage before using this cache; an independent revision counter is insufficient.

## Callers

The existing uncached Scene conversion overload remains a pure transactional
conversion. Its cache-aware overload accepts an explicit owner-thread cache.
Neither overload discovers an ambient service or schedules work.

`ScenePrefabExpansionOwner` owns one accepted JobSystem attempt. `Submit` copies
detached document/resolver/settings evidence; workers capture only that owned
envelope. Submit never grants blocking admission, even inside a host NonCritical
producer scope. Full queues return typed scheduler errors. Completion polling is
nonblocking; `Join` requires an explicit host-approved wait policy and deadline.

`TakeCompleted` checks current document session, scene/revision, complete
canonical Scene values, source commitments, registry and settings before any
memoization/publication. Validation allocation failure is a typed failed attempt.
After validation, cache capacity/allocation failure is best-effort memoization:
it cannot discard the complete definition. Each failed insertion preserves old
entries; earlier successful insertions may remain. Successful output still
requires a final parent-cancellation check after memoization and the containing
host's normal final publication fences.

Close calls `CloseDocument`; replacement calls `ReplaceScene`; teardown calls
`Shutdown` before destroying the scheduler. All cancel/join accepted work. An
interrupted drain keeps admission closed and ownership retained for retry. A new
session must not reuse the closed document incarnation. Destruction is a safety
teardown drain, not a normal frame wait. Scheduler lifetime exceeds owner lifetime.

The actual `PrefabSceneCookHost` owns this coordinator. Each cook replaces the
previous detached source session before preparation, closes it before asset
publication, and explicitly shuts down expansion at host teardown. Scene
preparation helps only its exact child under a bounded tooling wait; a failed
join cancels/drains the child rather than leaving it admitted for the next cook.
The host no longer has a `noexcept` constructor: coordinator construction can
allocate. Construct/use/destroy this host on its owner thread.

## Bounds and evidence

Entry count, retained logical owned storage and canonical key-source bytes have
finite explicit limits. Eviction removes only cache ownership; previously
returned immutable leases stay alive. `RetainedBytes` is owner-thread-only, not a
cross-thread telemetry accessor, and excludes allocator bookkeeping.

Regression coverage exercises real immutable resolver snapshots, actual Scene
conversion and Foundation jobs, including reordered/transitive dependencies,
same-revision source edits, every project-policy field, allocation failures,
late cancellation, source destruction, thread misuse, capacity admission and
close/replacement/shutdown. These tests must be compiled/executed under admitted
validation; their presence alone is not runtime evidence.
