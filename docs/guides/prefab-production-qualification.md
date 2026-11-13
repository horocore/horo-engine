# Prefab production qualification

This ledger tracks HORO-1007 / #1007 / [PFB-008] against stacked base
`3df476ff430a2ad28cd62a8a0c1780ae0fea71bc` on
`feat/HORO-1007_prefab_production_qualification`. Source inspection and newly
written assertions below are implementation evidence. Execution results must be
recorded separately before an acceptance criterion is considered qualified.

## Issue and implementation inventory

Jira and GitHub identities/statuses were read on 11 November 2026. GitHub #1076
is closed/completed while HORO-1076 remains In Review. HORO-1077, HORO-1078,
HORO-1080 and HORO-1081 remain In Review with open GitHub issues. The parent and
remaining children remain To Do in Jira and open in GitHub.

| Jira / GitHub / alias | Existing implementation and executable evidence | Added qualification or remaining acceptance evidence |
| --- | --- | --- |
| HORO-1076 / #1076 / PFB-008.1 | `PrefabDiagnostics.cpp`, `PrefabDiagnosticsTests.cpp` cover stable descriptors, typed foreign causes, identity, exact bounded context and Build Output projection. | A new regression covers deep resolver failures crossing the message budget. Long composition/source-chain messages now summarize omitted intermediate entries while retaining endpoints and the originating error. Native execution is pending. A closed GitHub issue does not resolve the Jira status discrepancy. |
| HORO-1077 / #1077 / PFB-008.2 | `PrefabMetricsTests.cpp`, `PrefabMetricCookHostTests.cpp` cover bounded tags, real parse/resolve/cache/cook, zero-allocation disabled producers, terminal receipt and retired telemetry generations. | Run the existing metrics and host targets. Logical span events qualify only the implemented telemetry boundary. |
| HORO-1078 / #1078 / PFB-008.3 | `PrefabDebugSnapshot.cpp`, `PrefabDebugSnapshotTests.cpp`, `ScenePrefabDebugSnapshotTests.cpp` cover immutable owned evidence, provenance, cancellation and explicit zero/exact-prefix truncation. | Run both model and scene targets. |
| HORO-1079 / #1079 / PFB-008.4 | `PrefabDiagnosticsTests.cpp` projects bounded source hints to Build Output. `SourceFileOpenService` and workspace source operations own containment, routing and localized failures. | New source-service test checks Prefab routing/focus, a moved source hint and missing handler. Current-revision AssetId/member/property/edge navigation remains unqualified. |
| HORO-1080 / #1080 / PFB-008.5 | Frozen `tests/fixtures/prefab-reference-corpus`, `PrefabReferenceCorpusTests.cpp`, `ScenePrefabExpansionQualificationTests.cpp`, `PrefabSceneCookQualificationTests.cpp` cover deterministic graph/override/nested/cook/runtime semantics. | Reuse the committed corpus and independent semantic oracles. |
| HORO-1081 / #1081 / PFB-008.6 | `PrefabParserFuzzTests.cpp` and `PrefabParserPersistenceFuzzTests.cpp` cover deterministic malformed recipes, shrinking/replay, native recovery publication and exact retained state. | Run both dedicated CTest entries. See `tests/unit/runtime/prefab/PrefabParserFuzzMatrix.md` for input and recipe bounds. |
| HORO-1082 / #1082 / PFB-008.7 | `PrefabLimitProfile`, existing graph work exhaustion, parser byte admission, override bounds and Scene expansion boundary tests. | New shared graph stress and nested fanout tests exercise exact admission, overflow, ordering and retained results. A deterministic in-flight cancellation test records allocation requests/bytes and enforces fewer requests than uninterrupted resolution plus a 250 ms cancellation-return ceiling. Execution and peak allocated bytes remain unverified. |
| HORO-1083 / #1083 / PFB-008.8 | Prefab document/session/workspace tests cover separate histories, explicit external conflict decisions, recovery, preview source drift and joined worker shutdown. | New deferred-stage test covers edit/reload/close before completion. Publication reentrancy now also probes reload/close/save. Full cross-document apply/rebase transaction races depend on their owning product contracts. |
| HORO-1084 / #1084 / PFB-008.9 | Cook generation/template provider, release executor/package and project migration tests already exist. | New host test removes authored scene/root/nested sources, reloads the published scene and verifies a failed recook retains that generation. OS, GUI and distribution execution remains a matrix obligation. |
| HORO-1085 / #1085 / PFB-008.10 | Existing reference corpus, Gameplay host, Editor product, cook/release and migration suites exercise real subsystem entry points. | New pipeline qualification measures real parse/expand/cook and committed-spawn samples against explicit allocation and latency ceilings. Native results, larger-workload Release measurements and integrated create-through-package evidence remain required for final acceptance. |

Open predecessor status alone does not establish a missing implementation.
The base contains `a9bda3bfa` (HORO-1031 canonical source layers), `b9168b03d`
(HORO-1035 immutable value diffs) and `239423a6c` (HORO-1069 dependency-local cook
cache reuse). Their actual callers and tests are part of this qualification.

## Bounded new workloads

| Workload | Admission and failure oracle | Execution conditions |
| --- | --- | --- |
| Shared graph | 64 prefab roots each reference the same 128 resources, giving 192 nodes and 8192 edges. Multi-root closure admits exactly 128 unique runtime resources and rejects capacity 127 with `DependencyClosureCapacityExceeded`. Reversed source capture preserves canonical nodes/edges and invalidation returns all 64 roots. | Cold immutable graph capture and closure queries. No expansion cache is used. |
| Nested fanout | One root mounts 64 occurrences of a two-object leaf, giving 129 expanded objects from two source assets. Object limit 129 admits all occurrence identities. Limit 128 rejects three attempts with `ObjectCountExceeded` while the earlier candidate stays readable. Pre-cancelled resolution returns `Cancelled`. | Cold source resolver, nested depth policy 1, no expansion cache. |
| Variant chain | A ten-source chain admits eight inheritance edges and rejects the ninth with `HierarchyDepthExceeded`. Retained and subsequent eight-edge resolutions still succeed. | Canonical source resolver at the engine hard variant depth of eight. |
| Work counter | A `size_t` maximum charge rejects with `WorkBudgetExceeded` while preserving the complete remaining budget. Zero-work charges succeed before and after exact exhaustion. | Operation-local checked arithmetic with no source/cache dependency. |
| Deep failure diagnostic | Eighteen nested sources exceed the sixteen-edge depth policy. The originating `HierarchyDepthExceeded` must still translate into a structured diagnostic and Build Output record within 512 message bytes, with root and last admitted source IDs retained. Long source/composition paths explicitly report omitted intermediate entries. | Immutable graph/source input, no cache, native execution pending. |
| In-flight cancellation | A single 256-object source is resolved normally while measuring owner-thread C++ allocations. An allocation observer requests cancellation halfway through that measured request count during a second cold resolution. The result must be `Cancelled`, allocate fewer times than the uninterrupted baseline and return within 250 ms of cancellation. The retained complete candidate and a subsequent resolution must remain valid. | Instrumentation disabled, cold source resolver. Output includes source bytes, object count, baseline/interrupted cumulative allocation requests/bytes and cancellation microseconds. These counts exclude native C allocation, other threads and process RSS. Native execution pending. |
| Canonical pipeline samples | Sixteen samples per phase invoke the real parser, source resolver, template cooker and runtime spawn/Scene commit owners. Each sample must succeed within 250 ms, 4096 owner-thread C++ allocation requests and 4 MiB cumulative requested bytes. Output includes actual build configuration/OS, phase/cache condition, two objects per operation, input bytes, maximum allocation counts/bytes/largest request and median/maximum microseconds. | Parse/expand/cook use the committed two-object `multi.prefab`, a two-node captured graph with no edges and no artifact/expansion cache. Cook target is the fixture's `linux-x64` contract. Spawn uses one warmed immutable template with two behavior-bearing entities and two numeric fields each. Its sixteen committed groups grow the real Scene from one to 33 entities. This is a small-cohort gate; larger-workload Release measurement remains pending. |
| Deferred editor stage | A real RuntimeSceneService host deliberately defers commit. Edit, external reload and close each retire the pending host before release. A later completion poll cannot publish a preview or fixed update. | Deterministic owner-thread lifecycle interleavings, no wall-clock sleep or scheduling lottery. |
| Source-free cooked reload | Eight scene placements expand to 41 entities. The real host publishes the generation before the authored scene and both prefab sources are removed. Reloaded canonical scene bytes remain identical and failed recook retains the selector. | Native files, real source capture/cook/generation decoder, cold first cook. |

These workloads bound generated data explicitly. Object/edge counts and typed
overflow checks establish structural admission. The in-flight cancellation case
adds a measured latency and cumulative C++ allocation gate whose values must be
captured on each actual platform/build invocation. Allocation peaks remain a
separate measurement obligation. The pre-cancelled case proves admission
cancellation only.

## Execution matrix

All configure, build, CTest and analysis-owned native build work must acquire
`scripts/dev.py`'s `heavy_build_lock` using this repository's shared Git common
directory. Builds use `--parallel 1`, `CMAKE_BUILD_PARALLEL_LEVEL=1` and
`CTEST_PARALLEL_LEVEL=1`. A dev.py command already holding the lock is invoked
once without another wrapper lock. Give the main CI validation priority.

| Composition / boundary | Targets or real entry points | Evidence required |
| --- | --- | --- |
| Editor-disabled headless Debug | `HoroPrefabTests`, `HoroPrefabReferenceCorpusTests`, `HoroPrefabParserFuzzTests`, `HoroPrefabMetricsTests`, `HoroSceneRuntimeConversionTests` | Configure with GUI/render/input/audio adapters disabled, build and execute registered CTest entries. |
| Native editor model/services | `HoroSceneDocumentTests`, `HoroSourceFileOpenServiceTests`, `HoroPrefabParserFuzzPersistenceTests` | Run document, source-open, recovery and deferred-stage cases. The persistence entry selects the existing document executable. |
| Native cook and cooked load | `HoroPrefabSceneCookHostTests`, `HoroPrefabTemplateCookGenerationTests`, `HoroPrefabTemplateProviderTests`, `HoroPrefabSpawnServiceTests` | Cold and warm cache evidence, corrupt/stale/cancelled generation retention, cooked loading, spawn receipt and teardown. |
| Product Gameplay/Editor hosts | `PrefabGameplayHostTests.cpp`, `EditorPrefabProductTests.cpp` in their registered host compositions | Native module/schema wiring, actual Scene publication, Stop/reload capability revocation and shutdown. |
| Release/package | Release executor cases in `PrefabSceneCookHostTests.cpp` | Both static-only and explicit runtime-template selections produce a verified pinned-generation release candidate. |
| Migration | Prefab cases in `ProjectMigrationTests.cpp` | Source identity adoption precedes Scene references. Dry-run leaves input unchanged. Malformed/cancelled publication retains recoverable source evidence. |
| GUI/display/backend | Supported graphical host and backend lanes | Manual interaction and compatible backend/display evidence. Headless model execution cannot fill this row. |
| Supported OS paths | Windows, Linux and macOS lanes | Native containment, Unicode, spaces, permissions and case behavior on each actual OS. |
| Performance Release | Real parse, expansion, cook and spawn entry points | Record CPU/OS, compiler, build mode, source bytes, graph shape, object count, cache state, instrumentation state, allocation count/peak, latency distribution and accepted numeric thresholds. |

Unavailable OS/device/backend execution is recorded as skipped with the missing
capability. Missing product contracts or measurement gates remain incomplete.

## Concrete remaining dependencies

`SourceOpenRequest` currently carries path/origin/mode/line/column. It has no
AssetId, source revision, prefab-local member/property or dependency-edge selection
proof. The existing generic opener can safely reject missing hints and route
supported Prefab documents. It cannot prove a moved AssetId's current revision or
select that member. No revisioned project-wide Prefab usage/repair owner was found
in the current `include/`, `src/editor/` and `src/application/` sources. The missing
HORO-1064 find-usages/repair contract therefore remains relevant to HORO-1079.

HORO-1083 still needs complete apply/rebase multi-owner transaction paths from its
owning predecessors. The implemented Save/recovery/session/preview boundaries
remain independently testable. HORO-1084/HORO-1085 also require the supported
packaged-player/OS execution and measured performance gates. Existing headless
release-stage tests alone cannot establish that full distribution matrix.

## Local validation record

A persistent goal was created on 11 November 2026 for the complete HORO-1007
parent and all ten children. Its complete objective remains in scope. GitHub and Jira status/identity
reads were repeated, and no remote delivery branch or pull request was found.
The previously idle state came from the main CI heavy-work reservation. An
independent follow-up found and fixed the overlong deep-failure message path and
added in-flight cancellation qualification. Those C++ changes are also pending
native execution and final preflight.

On 13 November the user resumed the complete scope and authorized normal branch
push and draft PR delivery. Policy V2 replaces the earlier named-grant hold with
nonblocking admission through the same canonical physical heavy-build lock.
The current worktree was rebased onto published parent
`3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`. One analysis-worker conflict was
resolved by preserving the parent's existing `--parallel 1` implementation and
updating the regression to require one worker even when the environment requests
two. No sibling checkout was edited.

The local helper now uses `heavy_build_lock(..., blocking=False)`. That additive
option preserves the default blocking behavior and the unverified-descendant
cleanup guard. Real OS-lock regression tests prove contention rejects immediately
and permits retry after release. The foreground batch checks its actual lock
handle, restricts itself to one allowed logical processor, requires 4 GiB available
memory and uses the existing owned process-tree runner. Two nonblocking admission
attempts returned `LOCK_BUSY` before any heavy child was created. No waiter was
left running.

Current-parent Python validation passed 65 quality-preflight, Prefab CI and
migration-generator cases. The local-cache suite passed 27 tests with one
host-permission skip for directory symlink creation. Its real compiler/ccache
consumer case was deselected because that workload also needs physical heavy-lock
admission. The two new nonblocking lock regressions passed independently.

Initial worktree was clean and detached at
`f51e0c0f44cd0f4c7931d288a5dde794e3316dfd`. The dedicated branch was created
without modifying the main checkout. Its six delivery commits were subsequently
rebased without conflicts onto the verified local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`. At that point origin still referenced
the initial base. Final preflight must use the new local parent SHA.
Initial Windows Debug configure
failed during the compiler ABI probe with MSVC C1902, before any Prefab test ran.
The isolated local runner will use CMake's Embedded MSVC debug information format
on retry. The queued retry was cancelled before configure/compiler/CTest children
started to release the heavy-build queue for main Windows CI.

`git diff --check` and clang-format dry-run passed after the test edits.
`python -m pytest -q --tb=short tests/python/test_prefab_hierarchy_ci.py
tests/python/test_project_migration_generator.py
-o cache_dir=build/prefab-qualification/pytest-cache` passed all nine tests.
Native regression execution is pending successful nonblocking physical lock admission. Newly
written C++ assertions are unverified.

The offline quality doctor reported Sonar CLI 1.10.0, Codacy CLI 0.25.0 and VS Code
1.145.3. A second doctor invocation with the tool PATH corrected found CMake
4.3.1 and Ninja 1.13.2. Its cloud configuration read failed with exit 2 and asked
for Codacy authentication verification. The doctor JSON files are under the
local quality cache, with basenames `1794377800123593300-f1528d9c.json` and
`1794380035194771600-239acb59.json`. Both are incomplete prerequisite reports.
Sonar IDE C++ execution, CLI secrets and Codacy changed-file analysis remain
pending the final worktree-aware preflight against the stated base. The parent
capability remains incomplete while these execution and product gaps remain.

The published parent already enforces a single compiler worker during analysis
context preparation. The rebase preserves that implementation. Mocked compiler
command cases cover Ninja/Make with absent, single-worker and higher inherited
environment caps, while requiring the actual build command to remain single
worker. The adjusted regression passed on the published parent as part of the
65-case Python run. Native Sonar, secrets and Codacy preflight remain pending
physical admission. Their completion has not been inferred from Python results.
