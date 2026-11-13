# Input System Qualification

This matrix tracks all eight INP-001 children against the integrated capability.
The delivery branch forked from `3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`.
On the resumed audit, the published PR target advanced to
`139f8a5ed63ca8698bb3852c9e0cb1eff421dc8d`; the existing delivery history is
preserved, and validation selects `origin/codex/editor-improvements` explicitly.
Source/test coverage and executed qualification are recorded independently.

## Full Capability Audit

The 2026-11-11 audit read the live GitHub parent/sub-issue relationships and Jira
summaries/statuses. A closed issue, existing source or clean worktree does not
qualify the current integrated build. No issue or workflow state was changed.

| Child | Existing implementation or decision evidence | Remaining qualification |
|---|---|---|
| INP-001.1 / #1834 / HORO-1790 | `input-layer-ownership.md` records the ratified collector/snapshot/router, context, modal and bus boundaries; original delivery `286d9003c` | Preserve these contracts through the integrated regression pass |
| INP-001.2 / #1835 / HORO-1791 | Typed conflict validation in `Input.cpp`, required-action checks and Settings authoring rejection | Run current map/profile/conflict tests after final edits |
| INP-001.3 / #1836 / HORO-1792 | Central `EditorModalHost`/`GuiScreenHost` dispatch gating and context/capture cancellation tests | Run current editor/runtime modal, focus and capture matrices |
| INP-001.4 / #1837 / HORO-1793 | Focused text delivery, composition retirement, SDL window filtering and text-input-area hooks | Run headless adapter suites; native IME and supported-OS interaction evidence remains external |
| INP-001.5 / #1838 / HORO-1794 | Recorded command/cadence tests; workspace `OnInputSnapshot` captures routed values and fixed callbacks consume tick commands | Run current replay/catch-up and workspace lifecycle tests |
| INP-001.6 / #1839 / HORO-1795 | Shared typed capture, complete scoped per-action edits, layered recovery and atomic persistence | Compile and run current core, Settings and Input Mapping regressions |
| INP-001.7 / #1840 / HORO-1796 | Generation-safe collector/SDL lifecycle, mixed ownership, canonical/raw devices, mapping/glyph fallback and rumble capability boundary | Run current core/SDL/Runtime UI suites; physical-device evidence remains external |
| INP-001.8 / #1841 / HORO-1797 | This matrix, public consumers, allocation/lifecycle coverage and ACC-001 readiness evidence | Complete integrated execution, final local quality checks and remaining OS/device evidence |

GitHub children .1 through .5 were closed at audit time. Jira .1/.2 were Done,
while .3/.4/.5 remained In Review. Both systems still had .6/.7/.8 open or To Do.
These workflow differences are recorded as delivery metadata, not test evidence.

The source audit found no `DataBus`, `EngineDataBus` or `EditorDataBus` reference
in `src/runtime/input` or its public `Input.h` contract. `HoroInput` links
Foundation and the private JSON parser; it has no editor dependency or bus
injection point. The normative collector prohibition and dependency/public-header
reviews are the enforcement approach; this audit does not claim a dedicated
automated bus-exclusion test. The workspace fixed callbacks consume only the
already captured `GameplayInputFrame`; no new raw-device access was introduced.

## Executable Coverage

| Contract | Executable evidence |
|---|---|
| Frame-local press/release, short taps and ownership-loss neutralization | `HoroInputTests`, including `InputTests.cpp` |
| Context priority, focus, pointer capture and central modal ownership | `HoroInputTests`, `HoroEditorModalHostTests`, `HoroGuiScreenHostLifecycleTests` |
| Text/IME delivery and window-local adapter filtering | `HoroInputTests`, `HoroInputSdlTests`, `HoroAndroidInputTests`, `HoroRuntimeUiInputTests` |
| Tick-assigned commands, zero-tick presentation frames, catch-up and recorded command replay | `HoroInputTests`, including `GameplayInputFrameTests.cpp` |
| Strict schema integers/chords, bounded reads, last-valid profile retention and failed publication | `HoroInputTests`, including `InputTests.cpp` |
| Project/global/project-user precedence, scoped keyboard/pointer/gamepad commits and new-session reload | `HoroInputTests`, including `InputTests.cpp` and `InputRebindingTests.cpp` |
| Runtime pause/editor capture, focus/modal/device cancellation and allocation-free normal polling | `HoroInputTests`, including `InputRebindingTests.cpp` |
| Unicode/space paths, project identity containment and escaping directory symlinks | `HoroInputTests`; symlink creation capability is reported by the host |
| Canonical and raw virtual devices, player assignment, hot-plug generations, focus loss and trigger normalization | `HoroInputTests`, `HoroInputSdlTests` |
| Unknown-controller Runtime UI navigation, numbered fallback glyphs and optional package presentation | `HoroInputTests` for vocabulary/provider contracts; `HoroRuntimeUiInputTests` for adapter routing |
| Optional SDL mapping upgrade, finite rumble requests, unsupported/stale devices and focus cancellation | `HoroInputSdlTests` using SDL virtual-device callbacks |
| Embedded Settings migration and canonical user-profile corruption | `HoroEditorSettingsStoreTests` |
| Reachable Input Mapping, hidden capture/text-focus withdrawal, narrow/localized/dark/light/disabled presentation | `HoroGlobalDockPanelRenderTests` including `FocusedPanelLifecycleTests.cpp`; `HoroEditorWorkspaceControllerTests` |
| Public contracts and downstream collection projection | `HoroInputPublicHeaderConsumer`, `HoroRuntimeUiNavigationPublicHeaderConsumer`, existing InputCollection and XR projection consumers |

## Execution Evidence

Local C++ execution is pending the shared Windows build slot. The initial
sandboxed configure failed its MSVC compiler probe with C1902. Its helper exited.
The subsequent waiting helper was stopped before starting a compiler. Further
heavy work uses the repository's shared lock with one build/test worker and
verifies the physical lock handle before starting child build processes.

The initial focused Python grouping/preflight suites passed: 91 tests.
English and Turkish catalog key structures were compared locally and match.
`git diff --check` passed during implementation. Final post-edit results belong
in the delivery validation summary together with the Sonar IDE, CLI secrets and
Codacy report paths and completion states.

On 2026-11-13 the delivery commits were rebased onto the published base above.
The local cache, developer runner/process, quality-preflight and CI grouping
Python suites passed with 214 tests and 13 platform/capability skips. The command
was `python -m pytest -q --tb=short tests/python/test_local_build_cache.py
tests/python/test_dev_runner.py tests/python/test_dev_process.py
tests/python/test_quality_preflight.py tests/python/test_ci_workflow_grouping.py`.
Two tests explicitly cover exclusive lock release and nonblocking admission.
`heavy_build_lock(repo, blocking=False)` preserves the same physical OS lock and
cleanup checks while allowing a busy batch to return immediately; existing
callers retain blocking behavior by default.

The first resumed C++ batch returned Windows lock contention (error 33) without
starting a child build or creating a background waiter. Named scheduling grants
are no longer a prerequisite; the physical repository lock and resource guards
remain mandatory. This contention result provides no C++ test or quality result.
The guarded full Sonar IDE, CLI secrets and Codacy invocation was also deferred
at the same physical lock (error 33), before any provider ran. Its state is
incomplete: no submitted-file list, C++ sensor execution or current provider
diagnostics exist yet. A draft review may expose this work to remote CI while
these local requirements remain open; it cannot count them as successful.

The hosted Codacy check passed for draft PR #3550 at head
`8b0f9ed44d30de787af67efff6aaed180dcf97d5`. A separate branch-specific v3 API
audit matched 46 changed files to that exact analyzed commit and retained 26
file issues, including contextual length/complexity warnings and a subprocess
audit error in the real-compiler cache fixture. That fixture invokes only
PATH-resolved compiler/cache tools with a fixed argv and owned temporary files;
the audit finding is retained without suppression or server disposition.
Six paths were not mapped: `apps/HoroEditor/app/HoroEditorApp.cpp`, both editor
localization JSON catalogs, `scripts/dev.py`, `src/CMakeLists.txt`, and
`tests/CMakeLists.txt`. Their metrics are unknown, not zero. Null metrics on
mapped files remain unknown as well. Duplication occurrences and unchanged
partners were retained in the local API report; 8 occurrences in the Input
header belong to 2 returned clone groups, not 8 groups. No review threads were
present at that head.

The changed Input Mapping, Settings save, canonical glyph and SDL gamepad
dispatch functions received length warnings. Their responsibilities were split
into focused helpers in the subsequent local candidate; compilation and fresh
local/server analysis of that candidate remain required. A successful hosted
check is separate from an issue-free file audit and from the pending full local
preflight. The raw API evidence is retained at
`build/input-delivery/codacy-cloud13.json` outside Git.

Hosted CI compiled the core Input targets successfully on Windows/MSVC at
`8b0f9ed44d30de787af67efff6aaed180dcf97d5`: the `ci-windows-debug`
preset builds `HoroCiWindowsChecks`, including `HoroInputTests`,
`HoroAndroidInputTests`, `HoroInputSdlTests`, `HoroRuntimeUiInputTests` and
`HoroInputPublicHeaderConsumer`. The completed Windows job succeeded; its log
records 85 passing `HoroInputTests` cases, 9 Android adapter cases, 12 SDL adapter
cases and 20 Runtime UI Input cases. These 126 cases have no failed or skipped
log entries. This evidence excludes GUI targets and subsequent local fixes.
The Android package workflow also succeeded at that published head;
package success does not substitute for the targeted Input test results.

The same head's hosted Sonar coverage build failed in the new Settings
regression: it referenced the retired `BuildEditorAppearanceConfigurationSchema`
helper. The local candidate now uses `CreateEditorConfigurationSchema` with
default editor settings, matching the current declaration and neighboring tests.
The completed Linux and macOS jobs failed at the same retired helper; downstream
macOS and Sonar jobs therefore lacked complete producer inputs. The local fix
still requires compilation and test execution. Two other CI jobs failed
while downloading Lua 5.4.8 with an SSL connection reset, before source compilation;
no TLS verification or dependency policy was changed to bypass those failures.

## Reproducible Targeted Qualification

These commands describe the required runs, not successful execution. On the
shared Windows host, the verified physical repository heavy-build lock and
resource guards are prerequisites; a named scheduling grant is unnecessary.
Keep one build/test worker. Dependency source
overrides may reuse admitted source caches, but compilation databases and build
outputs belong to this worktree.

First configure the headless composition with the SDL adapter, then build and
execute its affected consumers:

```sh
cmake -S . -B build/skeleton -DBUILD_TESTING=ON -DHORO_BUILD_EDITOR_GUI=OFF -DHORO_BUILD_INPUT_SDL3=ON -DHORO_BUILD_RENDER_OPENGL=OFF
cmake --build build/skeleton --parallel 1 --target HoroInputTests HoroAndroidInputTests HoroInputSdlTests HoroRuntimeUiInputTests HoroEditorSettingsStoreTests HoroInputPublicHeaderConsumer HoroRuntimeUiNavigationPublicHeaderConsumer
ctest --test-dir build/skeleton --parallel 1 --no-tests=error --output-on-failure -R '^(HoroInputTests|HoroAndroidInputTests|HoroInputSdlTests|HoroRuntimeUiInputTests|HoroEditorSettingsStoreTests|HoroInputPublicHeaderConsumer|HoroRuntimeUiNavigationPublicHeaderConsumer)(::|$)'
```

Reconfigure for the editor composition before its integration pass:

```sh
cmake -S . -B build/skeleton -DBUILD_TESTING=ON -DHORO_BUILD_EDITOR_GUI=ON -DHORO_BUILD_INPUT_SDL3=ON -DHORO_BUILD_RENDER_OPENGL=ON
cmake --build build/skeleton --parallel 1 --target HoroEditor HoroGlobalDockPanelRenderTests HoroEditorWorkspaceControllerTests HoroEditorModalHostTests HoroGuiScreenHostLifecycleTests HoroEditorSettingsStoreTests HoroXRActionBindingTests
ctest --test-dir build/skeleton --parallel 1 --no-tests=error --output-on-failure -R '^(HoroGlobalDockPanelRenderTests|HoroEditorWorkspaceControllerTests|HoroEditorModalHostTests|HoroGuiScreenHostLifecycleTests|HoroEditorSettingsStoreTests|HoroXRActionBindingTests)(::|$)'
```

Catch tests are registered with `Target::case` names; ordinary consumer tests
use the target name. The regular expressions deliberately admit both. An empty
selection is an error. Native Android-device execution is separate from the
headless Android adapter contract suite above.

After final source fixes, run `python3 scripts/analyze.py check --base
origin/codex/editor-improvements --compile-commands build/skeleton/compile_commands.json`
inside the verified physical lock after resource admission. Use the worktree's actual
VS Code profile/extension overrides when required. Review freshness, submitted
and skipped files, sensor execution and errors; report Sonar IDE, CLI secrets and
Codacy independently. An incomplete/authentication result is not a clean gate.

## Platform And Device Matrix

| Environment | Required evidence | Owner / current status |
|---|---|---|
| Windows headless | Core input, persistence, modal/text/replay and SDL virtual-device suites | Input delivery owner; execution pending shared build slot |
| Windows editor | Headless ImGui interaction/render tests and native visual/interaction pass | Editor/Input delivery owner; execution pending |
| Windows physical controller | Native connect/disconnect, common-controller mapping and physical rumble cancellation | Input/platform maintainer; no physical controller was detected on this host |
| Windows native IME | Native composition/candidate placement and focus transitions in an interactive editor | Desktop platform maintainer; external interactive evidence pending |
| Linux | Headless/backend suites plus native controller and IME interaction | Linux CI/platform maintainer; external execution pending |
| macOS | Headless/backend suites plus native controller and IME interaction | macOS CI/platform maintainer; external execution pending |

SDL virtual devices qualify the native adapter's event/mapping/callback path.
Physical controller and native IME rows require their own execution records.
GPU smoke results require explicit compatible-device execution.

## ACC-001 Remapping Readiness

ACC-001 consumes stable action/control identities, typed capture outcomes,
keyboard/pointer/gamepad replacements, required-action validation, actionable
conflicts, durable user scopes and canonical text fallback. Display/vendor
labels stay presentation data. The executable profile/capture matrices above
exercise those contracts through production Input entry points.
