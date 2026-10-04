# CI layout

## Pull requests and main

`ci.yml` uses four runner jobs: one tooling prerequisite and one native job on
Linux, macOS and Windows. `sonar.yml` uses one additional Linux runner for
instrumented coverage and cloud analysis.

| Workflow / stage               | Work performed                                                               | Configuration                                          |
|--------------------------------|------------------------------------------------------------------------------|--------------------------------------------------------|
| CI / Tooling                   | C++ formatting and Python tooling tests                                      | Python 3.12                                            |
| CI / Debug                     | Configure once, build, run the platform test group                           | Full Linux/macOS Debug; focused headless Windows Debug |
| CI / Release audio             | Build and run the seven real-time audio executables and callback lock policy | Headless Release on the same platform runner           |
| CI / Navigation without Recast | Compile public consumers and run the diagnostic consumer                     | Linux headless Debug, Recast Detour OFF                |
| CI / Network with GNS | Build the transport and public-header consumer; test connections, DNS, delivery and lifecycle | Headless Debug on all three existing platform runners |
| CI / Network without GNS | Verify production targets and FetchContent dependencies are absent from the configure | Linux headless, GNS OFF |
| Sonar / Coverage               | Instrumented build, selected C++ tests, Python coverage, fastcov conversion  | Linux Debug with GCC coverage flags                    |
| Sonar / Analysis               | Analyze compile commands and coverage; wait for the quality gate             | SonarCloud server analysis cache                       |

Debug already builds and tests the audio group. Release separately verifies
behavior with `NDEBUG`; coverage separately uses instrumented compiler outputs.
These configurations share dependency sources and compatible compiler entries.
The normal Debug build covers navigation with Recast enabled, so only the OFF
composition needs another configuration.

The network ON/OFF profiles run when network or build files change. Selection is
computed once by the tooling job from the event's commit SHAs, including renamed
paths. The [comparison API](https://docs.github.com/en/rest/commits/commits#compare-two-commits)
returns at most 300 changed files. Manual runs execute the profiles too; an unavailable or truncated change list
conservatively runs the checks. GNS ON retains the former independent headless
composition without GUI, physics or navigation. GNS OFF checks the current CMake
target graph and FetchContent population state, so cached sources from the ON
build do not create false failures. These stages add no runner jobs.

Windows still runs focused contracts: audio, CLI/platform/update, VFX, cinematic,
prefab/assets, SDL input/runtime UI, extension ABI, MCP session and save paths.
The full Windows suite remains disabled. The former ABI, MCP and save workflows
are included here, sharing the same Debug build and runner. Linux/macOS continue
to run the general suite, excluding GUI and the tooling tests already run in the
prerequisite job.

Sonar's CTest coverage group replaces direct repeated executable calls. The
editor suites previously selected explicitly remain included, and the UI
automation executable retains its non-native filter. Python tooling runs once
under coverage.py in this workflow.

## Where definitions live

- `CMakePresets.json`: build options, output directories and CTest selections.
- `tests/cmake/HoroCiSuites.cmake`: focused build targets and CTest group labels.
- `.github/actions/setup-build-tools`: shared platform setup.
- `.github/scripts/`: GitHub-specific coverage, scanner and Windows symbol helpers.
- `scripts/`: developer tools and reusable coverage conversion utilities.

No shell dispatcher is needed to interpret a workflow stage. For example:

```bash
HORO_COMPILER_LAUNCHER=ccache cmake --preset ci-linux-debug
cmake --build --preset ci-linux-debug
ctest --preset ci-linux-debug
```

Presets require CMake 3.25 or later. Native test artifacts contain one JUnit file
per configuration, with every CTest case name. A failing test stage fails the
job; other already-built test configurations still run unless cancelled.
The required `Test · Linux / GCC`, `Test · macOS / Clang` and `SonarCloud` check
names remain stable.

## Cache and concurrency

PRs restore the latest compatible completed main compiler checkpoint. Only main
writes caches; a running main build does not replace the last saved checkpoint.
Build caches are saved before tests, so test or analysis failures do not discard
completed compilation work. The existing native Debug and Sonar cache namespaces
and build directories remain unchanged, preserving available Linux/macOS main
checkpoints. The newly combined Windows profile needs its first main checkpoint.
GNS dependencies use a separate source-cache selection that can restore the exact
base sources first. This lets main save the larger selection even when its base
source cache was already complete. Both selections share the compiler checkpoint;
incomplete optional configuration never saves a partial dependency selection.
Superseded PR runs are cancelled; an active main run finishes its checkpoint.

This layout replaces nine CI runner jobs, up to three additional focused
Windows jobs, and four conditional network jobs with four CI jobs. Fewer queued jobs and repeated builds are a
structural improvement; elapsed-time savings need hosted measurements.

## Workflows with a separate purpose

- `ci-ui.yml` is scheduled/manual/release GUI qualification with display setup.
  Its legacy `debug`/`debug-msvc` presets and `HoroEditorUiTest` target are absent
  from the current tree; that migration needs separate UI qualification work.
- Release workflows package and publish deliverables.
- `cancel-closed-pr.yml` cancels remaining PR runs after merge or closure, using
  the trusted base workflow without checking out PR source.
