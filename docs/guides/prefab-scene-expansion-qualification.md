# Scene expansion and resolution qualification

This qualification uses the production Prefab resolver, Scene conversion,
owner-thread expansion cache, Foundation jobs and `PrefabSceneCookHost`. Fixtures
create validated immutable documents and actual canonical project files; no
resolver or conversion mock supplies the expansion result.

## Acceptance coverage

| Requirement | Executable coverage |
| --- | --- |
| One/many instances and hierarchy | Scene reference cases use 1, 8 and 32 outer placements, each with two occurrences of one nested two-object source. Host cases cook actual projects with 1 and 8 placements. Parent identities, placement/root transforms and behavior values are checked. |
| Internal references and ID pressure | Production identity remapping rewrites entity and behavior reference sites across distinct nested scopes; cross-instance targets fail. Sparse slots include `UINT32_MAX`; scene tests occupy a real deterministic generated ID and repeat a later placement ID. Neither collision can yield a runtime candidate. |
| Bounds | Exact project bounds admit six-object, four-level nested expansions. Lowering either bound by one fails. Separate fixtures admit the hard 256-object and 16-level ceilings and reject the next lower policy. Host scene capacity failure preserves the previously published generation. |
| Source moves | Republishing the same AssetId under a path containing spaces and non-ASCII text preserves the complete runtime definition and cooked bytes. Immutable retained candidates remain admissible when semantic inputs match. |
| Stale snapshots | Changed nested source revisions invalidate retained projection and completed owned-job publication. Cook-host registry changes at publication reject the generation. |
| Repeated/cold/hit equivalence | All entity fields, scene ID/revision, dependencies and canonical cooked bytes are compared. Scene cache tests prove actual retained-lease hits. Production cook repeats on one host, checks artifact cache hits and identical published scene payload/manifest digests. |
| Dependencies | Host reference scenes include a nested source's resource dependency. Published runtime output retains exactly the mesh AssetId/type and excludes source-prefab artifacts. |
| Transactionality/lifecycle | Later missing sources, malformed nested files, exact-bound failures and identity collisions reject complete candidates. Real owned jobs cover late cancellation, document close, replacement/retry and shutdown. Cook cancellation after staging preserves the old current generation and its bytes. |
| Runtime source boundary | Tests reload canonical cooked scene bytes without a resolver, re-encode identically, and verify no source paths, prefab placement/source fields or editor display names enter runtime bytes. |

Internal reference requests exercise the existing typed remap contract and are
checked against the entity IDs produced by actual containing-scene conversion.
The current Scene source schema has no persisted arbitrary reference-site
catalog; this qualification does not invent one or claim such a catalog is
implemented. Existing component/provider schema obligations remain unchanged.

## Validation

Build and run `HoroSceneRuntimeConversionTests` and
`HoroPrefabSceneCookHostTests`; new cases carry `[qualification]`. Include
`HoroPrefabTests`, `HoroPrefabSceneExpansionTests` and existing public consumers
in the targeted regression pass. The final changed-file preflight must run in
the delivery worktree against the actual target, with its own compilation
database. Report Sonar IDE C++ execution, secrets and Codacy separately, including
errors/skips and unavailable metrics.

These are source-level qualification cases, not a claim that validation has
already passed. The PR validation record owns actual commands, results and
report paths. Linux headless execution establishes only that composition;
unrun Windows/macOS or native GPU tests are not inferred from it. No public API,
persisted schema or runtime lifecycle contract changes are introduced.
