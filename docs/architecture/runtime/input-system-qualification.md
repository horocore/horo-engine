# Input System Qualification

This matrix tracks all eight INP-001 children against the integrated capability.
The delivery base is `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`.
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
| Text/IME delivery and window-local adapter filtering | `HoroInputTests`, `HoroInputSdlTests`, `HoroRuntimeUiInputTests` |
| Tick-assigned commands, zero-tick presentation frames, catch-up and recorded command replay | `HoroInputTests`, including `GameplayInputFrameTests.cpp` |
| Strict schema integers/chords, bounded reads, last-valid profile retention and failed publication | `HoroInputTests`, including `InputTests.cpp` |
| Project/global/project-user precedence, scoped keyboard/pointer/gamepad commits and new-session reload | `HoroInputTests`, including `InputTests.cpp` and `InputRebindingTests.cpp` |
| Runtime pause/editor capture, focus/modal/device cancellation and allocation-free normal polling | `HoroInputTests`, including `InputRebindingTests.cpp` |
| Unicode/space paths, project identity containment and escaping directory symlinks | `HoroInputTests`; symlink creation capability is reported by the host |
| Canonical and raw virtual devices, player assignment, hot-plug generations, focus loss and trigger normalization | `HoroInputTests`, `HoroInputSdlTests` |
| Unknown-controller Runtime UI navigation, numbered fallback glyphs and optional package presentation | `HoroRuntimeUiInputTests` |
| Optional SDL mapping upgrade, finite rumble requests, unsupported/stale devices and focus cancellation | `HoroInputSdlTests` using SDL virtual-device callbacks |
| Embedded Settings migration and canonical user-profile corruption | `HoroEditorSettingsStoreTests` |
| Reachable Input Mapping panel, hidden capture withdrawal, narrow/localized/dark/light/disabled presentation | `HoroGlobalDockPanelRenderTests`, `HoroEditorWorkspaceControllerTests` |
| Public contracts and downstream collection projection | `HoroInputPublicHeaderConsumer`, `HoroRuntimeUiNavigationPublicHeaderConsumer`, existing InputCollection and XR projection consumers |

## Execution Evidence

Local C++ execution is pending the shared Windows build slot. The initial
sandboxed configure failed its MSVC compiler probe with C1902. Its helper exited.
The subsequent waiting helper was stopped before starting a compiler. Further
heavy work uses the repository's shared lock with one build/test worker and
verifies the physical lock handle before starting child build processes.

The focused Python grouping/preflight suites passed: 91 tests.
English and Turkish catalog key structures were compared locally and match.
`git diff --check` passed during implementation. Final post-edit results belong
in the delivery validation summary together with the Sonar IDE, CLI secrets and
Codacy report paths and completion states.

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
