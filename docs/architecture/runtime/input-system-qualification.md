# Input System Qualification

This matrix tracks INP-001.6, INP-001.7 and INP-001.8 against the integrated
INP-001 capability. The delivery base is `5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`.
Source/test coverage and executed qualification are recorded independently.

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
