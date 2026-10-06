# Incremental Scene Cell Cook

Link `HoroEngine::SceneCellPayload` and include
`Horo/Runtime/Scene/IncrementalSceneCellCook.h`. The new header belongs exclusively
to that existing target; no public dependency edge changes. Existing direct
`CookRuntimeSceneCellPayload` and `QueueRuntimeSceneCellPayload` callers remain
valid and require no signature migration.

A host owns one `IncrementalSceneCellCook` on its tooling owner thread. Capture the
complete desired `SceneCellCookInput` set, current topology, verified artifact
catalog with direct dependency edges, settings SHA-256 and cache revision. Call
`Cook` with positive graph/schema/key/storage ceilings and per-cell payload limits.
A complete successful report contains immutable payload leases in canonical cell
order and actual cooked/reused counts. Publish through the existing deferred Scene
boundary with the current streaming fence and publication authority. Do not use
the cache revision as a live streaming generation or publication permission.

Dependency artifacts are captured after the Assets owner cooks and verifies them.
The cache includes transitive identities, revisions and digests, and rejects
missing/cyclic graphs. It does not replace dependency scheduling or extend the
existing Assets CacheKeyV1 contract. Settings must cover target/profile and every
byte-affecting host cook option. All actual typed cell content is hashed internally.

Failure preserves the prior cache revision/set. Replace by supplying the new
complete desired set; omission evicts cache ownership. Stop admission with
`Shutdown` before destroying the owner. Existing immutable leases stay valid;
callers remain responsible for live Scene publication/eviction and their leased
memory. Cache state is disposable and process-local; clean cooking is authoritative.

`HoroRuntimeSceneCellPayloadTests` covers selective source/transitive dependency
invalidation, clean baseline/key equivalence, stale/invalid/capacity/unsupported
failure, cancellation, replacement, leased shutdown and production deferred Scene
publication. `HoroSceneCellPayloadPublicHeaderConsumer` verifies header ownership.
