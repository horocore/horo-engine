# Navigation bake and cook qualification

The `[NAV-003.11]` qualification uses the production `NavigationBakeService`,
pinned Recast builder, native durable files, verified AssetCook generation reader
and real navigation query provider. It adds no alternative cooker or publication
authority. Each native scenario owns its operations, scheduler, service, immutable
input and unique directory with spaces and Unicode in its name.

## Reproducible matrix and thresholds

| Contract | Regression and threshold |
| --- | --- |
| Cold determinism | `Native bake qualification reproduces cold artifact bytes and rejects noncanonical closure order`: three fresh owners with distinct empty caches each rebuild all four tiles. Full encoded artifact bytes, manifest digest and every tile content identity must be exactly equal. Native query must cross the first border of the four-tile floor (x=4 to x=12). Reversed and rotated closure order must fail admission before invoking the builder; the canonical sorted closure is then submitted. |
| Durable write failure | `Native bake qualification preserves verified bytes through output write failure and recovers`: an injected filesystem write error while cooking an edited border must fail the operation, retain the prior in-memory lease, preserve the native current selector and every verified old artifact byte, and keep the old path query reachable. Removing the fault must publish the edited, disconnected floor while the retained generation remains readable. |
| Candidate output quota | `Native bake qualification refuses an undersized output profile without replacing the verified generation`: a fresh owner restricted to one candidate byte must fail rather than truncate or promote. The previously verified generation and all artifact bytes remain intact. Reopening with the original quota must publish the edit successfully. |
| Scheduler reservations | `Bake qualification bounds simultaneous jobs and resident memory and aggregate scratch reservations`: 24 bounded tile callbacks plus three surrounding stages must execute and drain exactly once under separate concurrency and resident-memory profiles. Atomic active/peak counters must stay within the job limit and the declared 30-byte resident reservation per active callback. A profile below the aggregate 960-byte tile scratch request must reject all callbacks with `TemporaryStorage` as its limiting resource. |
| Incremental edits | Existing `Incremental production cook rebuilds both sides of an edited border and reuses remote content identities`: exactly two adjacent tiles rebuild and two remote tiles retain identity; reopened service reuses all four. |
| Lifecycle and cancellation | Existing `Latest request replaces pending work and shutdown cancels unadopted native baking`, plus the bake-job cancellation, stale-publication, drain-timeout and scheduler-shutdown regressions retain owned work and authoritative commit truth. |
| Malformed/cache/lock failures | Existing bake-input count/work/owned-byte checks, `Production cache reuse rejects valid foreign source envelopes and unchanged producer digests cannot hide edits`, `Malformed current authority fails incremental publication and preserves the last valid lease`, and `Locked incremental publication preserves every unrelated artifact and rejects another writer` fail closed under their owning typed contracts. |

Scratch is an **aggregate** reservation for the whole attempt; resident memory is
a **simultaneous batch** reservation. The counters qualify declared scheduler
reservations, not process RSS or native allocator high-water marks. The output
quota qualifies owned candidate/artifact bounds. No physical-memory benchmark or
universal cross-toolchain bit identity is implied. Exact artifact identity is
required only for the same pinned provider/schema/settings/numeric fingerprint;
a changed provider fingerprint requires recook and separate qualification.

The write fault represents a bounded storage-write failure without filling the
host disk. It does not qualify physical disk exhaustion, power loss or fsync
behavior beyond the native publication regressions.

## Running the qualification

Configure a headless testing composition with the Recast bake provider enabled,
then build the existing targets and run the focused tag:

```sh
cmake -S . -B build/skeleton -DBUILD_TESTING=ON -DHORO_BUILD_EDITOR_GUI=OFF
cmake --build build/skeleton --target HoroNavigationBakeServiceTests HoroNavigationRuntimeTests --parallel 2
build/skeleton/tests/HoroNavigationBakeServiceTests '[qualification]'
build/skeleton/tests/HoroNavigationRuntimeTests '[qualification]'
ctest --test-dir build/skeleton -R 'Native bake qualification|Bake qualification|Incremental production cook' --output-on-failure
```

These files already belong to the discovered Catch2 test targets. No optional GPU
context is needed. The job reservation target is also available in a
Recast-disabled runtime composition; native service cases require the builder.
Hosted Linux/macOS full Debug discovery runs these cases. Both existing targets
also belong to the Windows reduced capability suite, so its `ci-windows` discovery
runs the qualification alongside their existing regressions. Check exact-head
Windows execution separately; compilation alone is not runtime evidence.

Record exact head, platform/toolchain/provider fingerprint, executed case count,
commands and failures with each result. Authored regressions and a green formatter
are not evidence that this matrix has run.
