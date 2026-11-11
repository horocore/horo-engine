# Prefab production qualification

This ledger tracks HORO-1007 / #1007 / [PFB-008] against stacked base
`f51e0c0f44cd0f4c7931d288a5dde794e3316dfd` on
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
| HORO-1076 / #1076 / PFB-008.1 | `PrefabDiagnostics.cpp`, `PrefabDiagnosticsTests.cpp` cover stable descriptors, typed foreign causes, identity, exact bounded context and Build Output projection. | Retain the merged implementation. A closed GitHub issue does not resolve the Jira status discrepancy. |
| HORO-1077 / #1077 / PFB-008.2 | `PrefabMetricsTests.cpp`, `PrefabMetricCookHostTests.cpp` cover bounded tags, real parse/resolve/cache/cook, zero-allocation disabled producers, terminal receipt and retired telemetry generations. | Run the existing metrics and host targets. Logical span events qualify only the implemented telemetry boundary. |
| HORO-1078 / #1078 / PFB-008.3 | `PrefabDebugSnapshot.cpp`, `PrefabDebugSnapshotTests.cpp`, `ScenePrefabDebugSnapshotTests.cpp` cover immutable owned evidence, provenance, cancellation and explicit zero/exact-prefix truncation. | Run both model and scene targets. |
| HORO-1079 / #1079 / PFB-008.4 | `PrefabDiagnosticsTests.cpp` projects bounded source hints to Build Output. `SourceFileOpenService` and workspace source operations own containment, routing and localized failures. | New source-service test checks Prefab routing/focus, a moved source hint and missing handler. Current-revision AssetId/member/property/edge navigation remains unqualified. |
| HORO-1080 / #1080 / PFB-008.5 | Frozen `tests/fixtures/prefab-reference-corpus`, `PrefabReferenceCorpusTests.cpp`, `ScenePrefabExpansionQualificationTests.cpp`, `PrefabSceneCookQualificationTests.cpp` cover deterministic graph/override/nested/cook/runtime semantics. | Reuse the committed corpus and independent semantic oracles. |
| HORO-1081 / #1081 / PFB-008.6 | `PrefabParserFuzzTests.cpp` and `PrefabParserPersistenceFuzzTests.cpp` cover deterministic malformed recipes, shrinking/replay, native recovery publication and exact retained state. | Run both dedicated CTest entries. See `tests/unit/runtime/prefab/PrefabParserFuzzMatrix.md` for input and recipe bounds. |
| HORO-1082 / #1082 / PFB-008.7 | `PrefabLimitProfile`, existing graph work exhaustion, parser byte admission, override bounds and Scene expansion boundary tests. | New shared graph stress and nested fanout tests exercise exact admission, overflow, ordering, retained results and pre-cancellation. Mid-work cancellation latency and peak allocated bytes still need measurements. |
| HORO-1083 / #1083 / PFB-008.8 | Prefab document/session/workspace tests cover separate histories, explicit external conflict decisions, recovery, preview source drift and joined worker shutdown. | New deferred-stage test covers edit/reload/close before completion. Publication reentrancy now also probes reload/close/save. Full cross-document apply/rebase transaction races depend on their owning product contracts. |
| HORO-1084 / #1084 / PFB-008.9 | Cook generation/template provider, release executor/package and project migration tests already exist. | New host test removes authored scene/root/nested sources, reloads the published scene and verifies a failed recook retains that generation. OS, GUI and distribution execution remains a matrix obligation. |
| HORO-1085 / #1085 / PFB-008.10 | Existing reference corpus, Gameplay host, Editor product, cook/release and migration suites exercise real subsystem entry points. | Run the supported journeys below. Integrated create-through-package evidence and measured parse/expand/cook/spawn allocation/latency ceilings remain required for final acceptance. |

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
| Deferred editor stage | A real RuntimeSceneService host deliberately defers commit. Edit, external reload and close each retire the pending host before release. A later completion poll cannot publish a preview or fixed update. | Deterministic owner-thread lifecycle interleavings, no wall-clock sleep or scheduling lottery. |
| Source-free cooked reload | Eight scene placements expand to 41 entities. The real host publishes the generation before the authored scene and both prefab sources are removed. Reloaded canonical scene bytes remain identical and failed recook retains the selector. | Native files, real source capture/cook/generation decoder, cold first cook. |

These workloads bound generated data explicitly. Object/edge counts and typed
overflow checks establish structural admission. Allocation peaks and mid-operation
cancellation latency require separate observed measurements. The pre-cancelled
case proves admission cancellation only.

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

Initial worktree was clean and detached at the stated base. The dedicated branch
was created without modifying the main checkout. Initial Windows Debug configure
failed during the compiler ABI probe with MSVC C1902, before any Prefab test ran.
The isolated local runner will use CMake's Embedded MSVC debug information format
on retry. The queued retry was cancelled before configure/compiler/CTest children
started to release the heavy-build queue for main Windows CI.

`git diff --check` and clang-format dry-run passed after the test edits.
`python -m pytest -q --tb=short tests/python/test_prefab_hierarchy_ci.py
tests/python/test_project_migration_generator.py
-o cache_dir=build/prefab-qualification/pytest-cache` passed all nine tests.
Native regression execution is pending the main CI heavy-work release. Newly
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

The base revision's analysis compiler-context preparation also hard-codes
`cmake --build --parallel 2`. That invocation must obey this task's explicit
single-worker requirement before the native preflight can execute. No analysis
build was started during main CI's reserved heavy-work interval.
