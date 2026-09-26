# Prefab Foundation Contract Qualification

This qualification covers the implemented authoring foundation, not deferred cooked
artifacts or dynamic runtime spawn. The authoritative prefab identity is the Asset
Registry sidecar `AssetId`; project paths are advisory. The authoring document uses
the unified project version and retains unknown project-owned component bytes.

## Acceptance matrix

| Contract | Executable evidence | Limit / qualification boundary |
| --- | --- | --- |
| Identity and deterministic round-trip | `HoroPrefabTests`: canonical serialization, equivalent-order bytes, opaque payloads, and `Prefab canonical source and graph retain identity across registry path changes` | Canonical bytes and graph closure remain stable for unchanged semantic input; path changes cause a new registry revision, not a new identity. |
| Sidecar rebuild and move/rename | `HoroAssetRegistryTests`: `Prefab Registry Enforces Typed Sidecars And Preserves Identity Across Moves`, duplicate IDs, case-folded extensions, and malformed sidecars | Rebuild uses the typed sidecar; stale path lookups do not become authority. |
| Bounded parsing and hostile input | `HoroPrefabTests`: serialization malformed/duplicate/UTF-8/version/depth/byte cases; document hierarchy, payload, policy, and limit tests | Source JSON is capped at 32 MiB and depth 32; validated policy may only lower the hard ceilings. Failure publishes no document. |
| Graph revision and lifecycle | `HoroPrefabTests`: dependency graph and closure stale/conflict/capacity cases; source resolver publication gate; scene identity remap | A graph is pinned to one registry publication and source revision. A stale candidate cannot be merged with a newer registry snapshot. |
| Migration and rollback | `HoroProjectMigrationTests` and `HoroProjectMigrationIntegrationTests`: prefab identity adoption, preserved unknown bytes, future/conflicting source rejection, malformed/orphaned sidecar, project-open failure without authoritative mutation. Existing `HoroProjectMigrationTransactionTests` covers journal recovery of verified originals after a failed publication. | Rejection preserves project, prefab, sidecar, and scene bytes and creates no migration history. Post-publication recovery uses the project-level migration transaction; there is no prefab-specific rollback authority. |
| Steady lookup and finite work | `HoroAssetRegistryTests`: 1,000 pinned `Find`/`FindByPath` pairs with zero observed heap allocations; `HoroPrefabTests`: exact graph work-budget boundary and exhaustion | Registry lookup is the frame-hot operation. JSON parse, graph construction, closure, and migration are load/tooling operations and may allocate within their explicit source and work ceilings; no zero-allocation claim is made for them. |
| Cross-platform path and byte behavior | `HoroAssetRegistryTests`: real sidecar move/rename, uppercase extension and malformed-source cases; `HoroPrefabTests`: opaque `00 7f ff` byte retention and UTF-8 rejection | Tests use portable project-relative paths and byte arrays; host filesystem case behavior is not used to define identity. |

## Platform evidence

The [CI workflow](https://github.com/horocore/horo-engine/actions/workflows/ci.yml)
tests the latest PR head on Linux/GCC and macOS/Clang. Its focused
`Prefab Foundation · Windows / MSVC` job builds and runs the prefab and Asset
Registry suites on Windows. Check the PR's current-head checks before marking any
platform qualified; this document intentionally contains no frozen `Pending` or
unverified `passed` status. The project-migration integration suite runs in the
Linux/macOS full-test matrix, not in the focused Windows job.

## Local commands and deviations

```bash
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --target HoroPrefabTests HoroAssetRegistryTests HoroProjectMigrationTests HoroProjectMigrationIntegrationTests --parallel 2
ctest --test-dir build/skeleton -R '^(HoroPrefabTests|HoroAssetRegistryTests|HoroProjectMigrationTests|HoroProjectMigrationIntegrationTests)::' --output-on-failure -j 2
```

No local platform result substitutes for another platform's hosted check. Deferred
cook/spawn rows in the [architecture validation matrix](../architecture/runtime/prefab-architecture.md#validation-matrix)
remain owned by their capability tickets; this foundation does not claim them.

On 2026-09-26, the Linux x86_64 GCC 15.2.0 local skeleton configure and four
target build above succeeded with `--parallel 2`. The stated CTest filter ran
86 registered tests with `-j 2`; all passed, including the new qualification
case. `HoroProjectMigrationTransactionTests` was inspected for rollback evidence
but was not in this local build/test selection. Hosted Linux/macOS full-suite and
focused Windows results must be checked at the exact PR head.
