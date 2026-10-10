# Prefab expansion cache and worker ownership

`PrefabExpansionCache` belongs to one owner thread. It memoizes immutable
`EffectivePrefabCandidate` leases, not live documents, runtime worlds or source
paths. A hit requires the root asset, scene occurrence, exact dependency graph
and source revisions, canonical source commitments associated with dependency
identities, registry revision and every field of `PrefabProjectPolicy` to match.
Placement transforms and containing-scene parents remain fresh Scene inputs.

Resolver construction computes each source's canonical digest and exact byte
length from its actual owned validated document, independently of claimed source
revisions. The readonly `CanonicalSourceCommitments()` span is identity-associated
and sorted by AssetId. Cache capture and owner admission reuse that immutable
evidence; source retirement cannot change it. Existing resolver callers require
no constructor or request migration.

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

`TakeCompleted` checks current document session, scene/revision, complete ordered
owned Scene values, source commitments, registry and settings before any
memoization/publication. Validation allocation failure is a typed failed attempt.
Authored comparison includes all eight object fields, all optional/vector/opaque
component values and every placement field. It does not rebuild JSON or trust
revision counters. Admission still enforces the prior complete Scene codec
validation and exact 16 MiB encoded-byte ceiling, plus the logical copy bounds.
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

Canonical source encoding uses a non-installed structural wire helper shared by
the Prefab and Scene source targets. Ordinary bounded-schema containers clean up
without allocation; only scalar escaping/number spelling uses the pinned JSON
codec. String scalars and field names first establish valid empty string storage,
then assign content: the pinned compatible-type constructor sets the string tag
before allocation and cannot safely unwind an allocation failure. Typed storage
construction preserves codec escaping while avoiding that incomplete state.
Recursive wire copies complete each alternative outside the destination variant
before moving it into place. This avoids the GCC 13 recursive-variant copy
unwinding path observed when a nested container allocation fails; copy assignment
first completes a replacement, preserving the previous destination on failure.
Prefab insertion ordering and Scene sorted-map ordering, indentation and
final newlines are preserved. Prefab serialization and resolver admission return
typed allocation failures. Scene owner admission translates encoding allocation
failure at its existing Result boundary without accepting a partial attempt.

The existing internal cook/editor DOM consumers explicitly bridge with `ToJson()`.
That bridge retains their old JSON-tree behavior and is **not** claimed OOM-safe;
neither cache capture nor owner Submit/TakeCompleted uses it. There is no installed
new JSON API, dependency patch, allocator replacement or changed persisted schema.

Regression coverage exercises real immutable resolver snapshots, actual Scene
conversion and Foundation jobs, including reordered/transitive dependencies,
same-revision source edits, every project-policy field, allocation failures,
late cancellation, source destruction, thread misuse, capacity admission and
close/replacement/shutdown. These tests must be compiled/executed under admitted
validation; their presence alone is not runtime evidence.
