# Developer Environment

## Purpose

This document defines the canonical local development environment for Horo
Engine. The goal is to get a new contributor from clone to a passing build and
passing tests in one deterministic path, regardless of host operating system.

## Supported Hosts

| Host OS | Primary Compiler | Secondary Compiler | Notes |
|---|---|---|---|
| macOS 14+ | Clang (Apple/Xcode) | Clang 17+ (Homebrew) | Metal-capable GPU recommended for GUI tests |
| Linux | GCC 13+ | Clang 17+ | Ubuntu 24.04 LTS is the reference distribution |
| Windows 11 | MSVC 2022 | Clang-CL 17+ | Latest Windows SDK required |

All hosts must support C++20.

## Required Tools

Every developer machine needs:

- CMake 3.28 or newer
- Ninja 1.11 or newer
- Python 3.11 or newer
- Git 2.40 or newer
- A supported C++ compiler (see table above)
- ccache (macOS/Linux) or sccache (Windows)

Optional but recommended:

- clang-format 17+ (formatting is checked in CI)
- clang-tidy 17+ (optional local static analysis)
- RenderDoc or Xcode GPU Frame Capture

## Python Dependencies

All project-owned Python entry points, including `scripts/dev.py`,
`scripts/ci.py`, and `scripts/dependency-state.py`, use
`scripts/requirements.txt` as their single third-party dependency declaration:

```text
pytest==8.4.2
```

Hashing, JSON parsing, argument parsing, and filesystem access in
`dependency-state.py` use the Python standard library (`hashlib`, `json`,
`argparse`, and `pathlib`) and therefore require no additional package. Any
future non-standard import in any project script must be added to
`scripts/requirements.txt` in the same change.

CMake and Ninja are system tools and should be installed through the system
package manager or official installers (see [Required Tools](#required-tools)).
On systems where they are unavailable, the Python wrapper packages can be used
as a fallback:

```bash
python3 -m pip install "cmake>=3.28" "ninja>=1.11"
```

The `setup` command verifies CMake, Ninja, the declared Python packages, and all
project-owned Python entry points. It also runs repository-fixture smoke checks
for the dependency fingerprint and resolved-manifest verifier so a missing
runtime import fails during setup rather than during a later CI-equivalent
check. To install the Python packages manually:

```bash
python3 -m pip install -r scripts/requirements.txt
```

On systems where `pip` is not available, use the system package manager or a
virtual environment.

## Git Hooks

Optional local Git hooks may invoke `python3 scripts/dev.py hook pre-commit` and
`python3 scripts/dev.py hook pre-push` from the repository root. These commands
run staged formatting and CI-parity checks respectively; they never install or
modify your hooks. Keep any existing personal hooks unchanged.

The former `scripts/open-pr.sh` command is now `python3 scripts/dev.py open-pr
<title> <body-file> <base> <head> <milestone> [project]`. It mutates GitHub only
when explicitly invoked.

## One-Liner Setup

`python3 scripts/dev.py setup` configures and smoke-builds `HoroFoundation`
using the installed native toolchain. It does not install system packages.
Use `--dir`, `--target`, `--jobs` and `--no-cache` to select an isolated build;
`python3 scripts/dev.py doctor` diagnoses missing prerequisites. Install approved
native dependencies through the platform's setup guidance before invoking it.

Successful `setup` verifies the selected smoke-build target, not the full editor
or every supported platform configuration.
Local setup intentionally validates the normal PCH-enabled developer path.
Compiler-cache CI uses `HORO_ENGINE_ENABLE_PCH=OFF`; reproduce that
configuration locally with:

```bash
cmake --preset debug \
  -DHORO_ENGINE_BUILD_TESTS=ON \
  -DHORO_ENGINE_ENABLE_PCH=OFF
```

## Manual Setup

If the script cannot be used, follow these steps manually.

### macOS

```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install CMake, Ninja, Python dependencies
brew install cmake ninja ccache

# Configure
cmake --preset debug \
  -DHORO_ENGINE_BUILD_TESTS=ON \
  -DHORO_ENGINE_ENABLE_PCH=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

### Linux (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y cmake ninja-build ccache build-essential python3-pip

cmake --preset debug \
  -DHORO_ENGINE_BUILD_TESTS=ON \
  -DHORO_ENGINE_ENABLE_PCH=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

### Windows

Install Visual Studio 2022 with "Desktop development with C++" workload.
Install sccache via cargo or chocolatey.

```powershell
cargo install sccache
# or
choco install sccache

cmake --preset debug-msvc \
  -DHORO_ENGINE_BUILD_TESTS=ON \
  -DHORO_ENGINE_ENABLE_PCH=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
```

## First Build And Test

After setup:

```bash
# Build all default targets
python3 scripts/dev.py build

# Run the fast test lane
python3 scripts/dev.py test

# Run specific test binaries
python3 scripts/dev.py test -- test_scene_project_model test_asset_import
```

`dev.py test` and test-building `check` commands reconfigure through the
canonical [`tests` build profile](./build-system.md#build-profiles) when
necessary, so they do not assume test binaries already exist. A profile selects
which targets are built; it is applied on top of the host preset (`debug` on
macOS/Linux or `debug-msvc` on Windows) and sets
`HORO_ENGINE_BUILD_TESTS=ON`. Developers invoking CMake directly must configure
with that option before building or running test targets.

Switching between `setup`, `build`, and `test` may reconfigure the same preset's
build directory because the default editor profile keeps tests disabled while
the tests profile enables them. This is expected. The first profile switch can
trigger additional build work; subsequent runs are incremental once the profile
stamp and compiler cache are warm.

Expected first-build time depends on cache state and hardware. A warm ccache
build on a modern machine should complete in under two minutes for the fast
lane.

## IDE Configuration

### Visual Studio Code

Recommended VS Code extensions:

- Clangd (provides language server features; CLion uses its own indexer)
- CMake Tools
- Python
- Doxygen Documentation Generator

Workspace settings for `.vscode/settings.json`:

```json
{
    "cmake.useCMakePresets": "always",
    "cmake.configureOnOpen": true,
    "files.associations": {
        "*.h": "cpp"
    }
}
```

CMake Tools must use the checked-in configure, build, and test presets. Variant
selection is not used when preset integration is enabled. Run
`scripts/dev.py setup` before the first IDE configure so the selected preset's
build directory contains the host-appropriate compiler launcher; persistent
developer-specific overrides belong in the ignored `CMakeUserPresets.json`,
not in shared VS Code variant settings.

### CLion

1. Open the project root.
2. CLion should auto-detect `CMakePresets.json`.
3. Select the `debug` preset.
4. Add `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
   to the CMake options if not already present.
5. Set the test target to a fast unit test target such as `test_scene_project_model`
   for quick iteration.

### Xcode

Generate an Xcode project with the dedicated multi-config preset:

```bash
cmake --preset xcode-debug
open build/xcode-debug/HoroEngine.xcodeproj
```

`xcode-debug` uses its own `build/xcode-debug` directory and must not reuse the
Ninja `build/debug` tree. CMake's compiler-launcher integration is supported by
the Makefile and Ninja generators, not the Xcode generator, so this preset does
not promise ccache acceleration and must not set
`CMAKE_C_COMPILER_LAUNCHER` or `CMAKE_CXX_COMPILER_LAUNCHER`. Ninja remains the
preferred generator for CI and daily command-line builds.

### Visual Studio

Use the `debug-msvc` preset:

```powershell
cmake --preset debug-msvc
```

Open the generated solution under `build/debug-msvc/`.

## Daily Development Workflow

### Pull latest changes

```bash
git pull origin main
python3 scripts/dev.py build
python3 scripts/dev.py test
```

### Work on a feature

```bash
# Create a branch following the naming convention
git checkout -b feat/HORO-XXX_short_topic

# Iterate
python3 scripts/dev.py build
python3 scripts/dev.py test -- test_<module>

# Format changed files before committing
python3 scripts/dev.py format --staged
```

### Verify before push

```bash
python3 scripts/dev.py check
# Runs: format check, architecture dependency check, header check, vendor check, fast tests
```

## Project Package Restore

When opening a project on a new machine or after pulling dependency changes,
restore project packages before building:

```bash
horo project restore
```

This command:

1. reads `.horo/packages.json` and `.horo/packages.lock`
2. downloads missing packages into the global package cache
3. verifies hashes, signatures, and manifests
4. mounts read-only package assets
5. registers game library modules
6. prompts for trust when code or editor contributions are present

CI and `scripts/dev.py setup` run the same restore path. Package artifacts do
not need to be committed to the project repository as long as every dependency
has a portable, verifiable source.

Commands:

```bash
horo project restore          # restore all project packages
horo package verify           # verify cached packages without changing state
horo package cache list       # list cached packages
horo package cache clean      # remove unused or invalid cache entries
```

## Cache Activation

The setup script enables ccache on macOS/Linux and sccache on Windows.

On macOS/Linux, verify the ccache process statistics and configured launchers:

```bash
ccache --show-stats
grep -E "CMAKE_(C|CXX)_COMPILER_LAUNCHER" build/debug/CMakeCache.txt
```

Disable or re-enable ccache for the `debug` preset:

```bash
cmake --preset debug \
  -DCMAKE_C_COMPILER_LAUNCHER= \
  -DCMAKE_CXX_COMPILER_LAUNCHER=

cmake --preset debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

On Windows PowerShell, use the `debug-msvc` build tree and sccache:

```powershell
sccache --show-stats
Select-String `
  -Path build/debug-msvc/CMakeCache.txt `
  -Pattern 'CMAKE_(C|CXX)_COMPILER_LAUNCHER'

cmake --preset debug-msvc `
  -DCMAKE_C_COMPILER_LAUNCHER= `
  -DCMAKE_CXX_COMPILER_LAUNCHER=

cmake --preset debug-msvc `
  -DCMAKE_C_COMPILER_LAUNCHER=sccache `
  -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
```

Run two equivalent builds before interpreting a zero hit count; the first build
normally populates the cache. The `xcode-debug` preset does not use a compiler
launcher, so ccache statistics are not expected for Xcode-generated builds.

## Development Logging

Local debug builds compile `Trace` and default to `Debug`. Focused categories
can be enabled without turning every subsystem to its most verbose level:

```bash
HORO_LOG_LEVEL=info \
HORO_LOG_LEVELS=asset.import=trace,pipeline.build=debug \
python3 scripts/dev.py run editor
```

Tests accept a default level through the wrapper:

```bash
python3 scripts/dev.py test --log-level debug
HORO_LOG_LEVEL=trace python3 scripts/dev.py ui test --windowed
```

When diagnosing a directly invoked binary:

```bash
build/debug/bin/HoroEditor \
  --log-level info \
  --log-level renderer=debug
```

Repository-local development and CI logs use an explicit directory under the
active build/output tree. Packaged applications use platform user-log
directories. CLI and Python tools keep declared command output on stdout and
write logs to stderr.

See [Observability Architecture](../observability/observability.md) for category syntax,
compiled/runtime levels, persistent locations, MDC, retention, and diagnostic
bundles.

### Development Metrics And Profiling

CPU, memory, and frame metrics are enabled by default in development profiles.
Run a focused profiler capture only when timeline detail is needed:

```bash
# Record a 20-second CPU/GPU/jobs capture.
python3 scripts/dev.py run editor -- \
  --profile-capture 20 \
  --profile-channels cpu,gpu,jobs,counters

# Reproduce a memory issue with allocation callstacks from process startup.
HORO_PROFILE=on \
HORO_PROFILE_CHANNELS=cpu,memory \
HORO_PROFILE_MEMORY=callstacks \
python3 scripts/dev.py run editor

# Keep one hour of low-rate CPU/memory history.
HORO_METRICS_SAMPLE_MS=1000 \
HORO_METRICS_HISTORY_SECONDS=3600 \
python3 scripts/dev.py run editor
```

Normal metrics remain bounded and low-cost. Memory allocation callstacks and
detailed profiler channels are explicit because they can materially affect
timing, memory use, and capture size.

## Common Issues

### FetchContent fails behind a proxy

Set the standard Git environment variables before running CMake:

```bash
export HTTPS_PROXY=http://proxy.company:8080
export GIT_SSL_NO_VERIFY=1   # only in controlled corporate environments
```

### Stale CMake state

```bash
# macOS/Linux: selects debug by default
python3 scripts/dev.py configure --fresh

# Windows PowerShell: selects debug-msvc by default
python scripts/dev.py configure --fresh

# Explicit preset override, for example Xcode
python3 scripts/dev.py configure --fresh --preset xcode-debug
```

The helper resolves the platform-default preset before invoking CMake. The first
command is equivalent to `cmake --fresh --preset debug` on macOS/Linux, while
the Windows command is equivalent to
`cmake --fresh --preset debug-msvc`. An explicit `--preset` always takes
precedence.

### Test binary crashes on GUI-related tests in headless environment

Tests that exercise windows, input routing, focus, or display integration
require a real or virtual display. On Linux:

```bash
xvfb-run python3 scripts/dev.py test -- test_editor
```

On CI, the GUI workflow uses a real display driver. The null renderer can cover
tests that do not require window-system behavior, but it does not replace a
display for GUI tests.

### Windows sccache cache miss on every build

Ensure `SCCACHE_DIR` is on the same drive as the build tree and that the
directory is not inside OneDrive or another sync folder.

## Environment Variables

| Variable | Purpose |
|---|---|
| `FETCHCONTENT_BASE_DIR` | Override where vendor dependencies are cloned |
| `CCACHE_DIR` / `SCCACHE_DIR` | Override compiler cache location |
| `HORO_RENDERER` | Force a renderer backend (`opengl`, `null`) |
| `HORO_ASSET_ROOT` | Override default asset search path |
| `HORO_LOG_LEVEL` | Set the process-wide minimum log level |
| `HORO_LOG_LEVELS` | Set comma-separated category-prefix level overrides |
| `HORO_LOG_FORMAT` | Select `pretty` or `json` console formatting |
| `HORO_LOG_DIR` | Override the persistent log directory |
| `HORO_LOG_CONSOLE` | Select `auto`, `on`, or `off` console logging |
| `HORO_LOG_FILE` | Enable or disable persistent file logging |
| `HORO_LOG_SOURCE` | Select `auto`, `on`, or `off` source-location output |
| `HORO_METRICS` | Enable or disable the bounded metrics runtime |
| `HORO_METRICS_SAMPLE_MS` | Configure the process metric sampling interval |
| `HORO_METRICS_HISTORY_SECONDS` | Configure bounded in-memory metric history |
| `HORO_PROFILE` | Enable an explicit profiler capture |
| `HORO_PROFILE_CHANNELS` | Select CPU, GPU, jobs, counters, or memory channels |
| `HORO_PROFILE_OUTPUT` | Select an approved profiler capture output root |
| `HORO_PROFILE_MEMORY` | Select memory tags, allocations, or callstacks |

### Headless Testing With The Null Renderer

Set `HORO_RENDERER=null` to run renderer-independent tests without a GPU:

```bash
HORO_RENDERER=null python3 scripts/dev.py test
```

The null renderer is used automatically for compatible CI jobs when no display
is available. Tests of window creation, input, focus, clipboard, or other
display-system behavior are not compatible with this mode; local GUI tests
still require a display or `xvfb-run` on Linux.

## Container / Remote Development

A devcontainer configuration is provided in `.devcontainer/` for VS Code remote
containers.

Open the project in VS Code with the Dev Containers extension installed. VS Code
will detect `.devcontainer/devcontainer.json` and prompt to reopen the folder
in a container. If the prompt is dismissed, open the Command Palette and run
**Dev Containers: Reopen in Container**.

The repository does not document an internal extension command ID as a shell
interface. Those IDs and their arguments are not a stable VS Code CLI contract.

The devcontainer pre-installs the Linux toolchain. Cache warming is optional
and never restores an arbitrary "recent" artifact. After toolchain detection,
the container computes the normalized compiler identity, CMake major/minor
version, and dependency fingerprint with the same scripts used by CI. It may
then request the artifact from the latest successful protected-default-branch
`devcontainer-cache-seed` job for that exact tuple. The artifact name is:

```text
devcontainer-cache-seed-v1-linux-<arch>-<compiler-identity>-<cmake-major-minor>-<dependency-fingerprint>
```

Before extraction, the setup helper validates the artifact schema, producer
workflow and run, OS and architecture, full compiler identity, CMake
major/minor version, dependency fingerprint, and SHA-256 digest of every
payload. It restores ccache and FetchContent data only when all fields match.
After configure, it runs the resolved dependency verification described in
[Build Cache](./build-cache.md#layer-2-fetchcontent-cache) before compilation.

An unavailable, expired, mismatched, or invalid artifact is discarded in full.
Setup prints the reason and continues with empty local caches; it never performs
a partial restore. The seed is only a performance hint and cannot weaken
immutable dependency pins or resolved-revision verification. See
[Build Cache](./build-cache.md#devcontainer-cache-seeds) for the artifact
contract.

## Update Toolchain

When the project bumps the minimum compiler or CMake version, run:

```bash
python3 scripts/dev.py setup --force
```

This re-runs toolchain detection and reconfigures from scratch.

## Related Documents

- [Build System](./build-system.md): CMake architecture, presets, and targets.
- [Build Cache](./build-cache.md): ccache/sccache and FetchContent caching.
- [Testing Architecture](./testing-architecture.md): how to run and write tests.
- [Quality And CI](./quality-and-ci.md): CI gates and quality expectations.
- [Observability Architecture](../observability/observability.md): logging configuration,
  context, storage, and diagnostic files.
