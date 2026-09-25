# Build System

## Purpose

This document defines the CMake-based build system for Horo Engine. It covers
target organization, dependency direction, CMake presets, dependency
management, and the rules for adding new modules and targets.

The build system is designed to be:

- deterministic across platforms
- cache-friendly for local development and CI
- explicit about dependencies between modules
- easy to extend without breaking downstream targets

## CMake Presets

CMake presets live in `CMakePresets.json` and `CMakeUserPresets.json` (optional,
local-only). Presets encode the build type, generator, toolchain, and common
variables so that every developer and CI job uses the same configuration.

### Canonical Presets

CMake presets encode the build type, generator, toolchain, and common variables.
They are orthogonal to build profiles: a preset selects *how* to build, while a
profile selects *what* to build.

| Preset | Generator | Build Type | Purpose |
|---|---|---|---|
| `debug` | Ninja | Debug | Local development, default |
| `xcode-debug` | Xcode | Multi-config, Debug default | macOS IDE project generation |
| `debug-msvc` | Ninja + MSVC | Debug | Windows local development |
| `release` | Ninja | Release | Release builds, packaging |
| `release-msvc` | Ninja + MSVC | Release | Windows release builds |
| `sanitizer` | Ninja | Debug | ASan/UBSan validation |
| `coverage` | Ninja | Debug | Coverage instrumentation |

Configure a preset once:

```bash
cmake --preset debug
```

Then build:

```bash
cmake --build --preset debug
```

Generator families never share a binary directory. In particular,
`xcode-debug` writes to `build/xcode-debug` rather than overriding the
generator of `debug` in `build/debug`. The Xcode preset does not configure
`CMAKE_C_COMPILER_LAUNCHER` or `CMAKE_CXX_COMPILER_LAUNCHER`; CMake documents
compiler-launcher integration for Makefile and Ninja generators, not Xcode.

## Build Profiles

A build profile selects which targets participate in a build. Profiles speed up
the developer loop by avoiding unnecessary targets.

| Profile | Builds | Required CMake state | Typical Use |
|---|---|---|---|
| `editor` | `HoroEditor` and everything it depends on | `HORO_ENGINE_BUILD_TESTS=OFF` | Full editor development |
| `cli` | `horo-engine`, `horopak`, and their dependencies | `HORO_ENGINE_BUILD_TESTS=OFF` | CLI tooling and CI |
| `runtime-only` | Foundation, Assets, Scene, Render, Physics, Audio, Network, Pipeline | `HORO_ENGINE_BUILD_TESTS=OFF` | Runtime/engine work without editor or GUI |
| `tests` | All test targets and their production dependencies | `HORO_ENGINE_BUILD_TESTS=ON` | Running the test matrix |

Profiles are passed to `dev.py`:

```bash
python3 scripts/dev.py build --profile cli
python3 scripts/dev.py build --profile editor
python3 scripts/dev.py build --profile runtime-only --preset release
python3 scripts/dev.py build --profile tests
```

`dev.py` maps each profile to both a set of CMake target names and the CMake
cache inputs required for those targets to exist. Profiles are not CMake
presets; they are developer-facing shortcuts layered on top of presets.

Before building, `dev.py` compares the selected profile and its configure inputs
with a stamp in the preset's build directory. A missing or changed stamp runs
CMake again with the profile's explicit values, for example:

```bash
cmake --preset debug -DHORO_ENGINE_BUILD_TESTS=ON
```

The script must not treat an existing `build.ninja` or IDE project as proof that
the build directory is configured for the selected profile. Switching from
`cli` to `tests`, or back, reconfigures before target selection. The `test`,
`test --all`, and test-building `check` commands use the same tests-profile
configure path rather than assuming Catch2 and test targets already exist.

The CLI profile is the default for headless CI jobs. It excludes:

- `HoroEditor`
- `HoroEngine::Gui`
- `HoroEngine::Mcp`
- `HoroEngine::EditorModel`
- `HoroEngine::EditorServices`
- renderer backends not required by CLI tools

This keeps CLI pull requests fast and avoids pulling GUI stack dependencies
when they are not needed.

Local development defaults to the `editor` profile because most contributors work
on the editor and runtime together. Headless CI jobs default to the `cli`
profile to minimize build time for tooling-only changes.

Developer build profiles are not product roles. In particular, `runtime-only`,
`cli`, a Release configuration, headless execution or renderer omission does not
mean client, listen-server or dedicated-server. Under
[ADR-103](../../adr/103-network-project-configuration-and-build-profile-ownership.md),
the product release profile produces a typed `NetworkArtifactPlan` naming exact
ADR-102 modes, targets, private providers, platform variants and qualification
evidence. Configure/build/package consume that pinned plan and fail if required
content is unavailable; they never infer network capability from a renderer/device
tier or silently include a server/provider target.

### Adding A New Preset

New presets must be reviewed if they change compiler flags, generator, or
toolchain. Presets are not the place for target-specific toggles; use target
options or per-target compile definitions for those.

## Target Hierarchy

Targets are grouped into libraries and executables. Library targets follow the
module structure defined in [System Design](../foundation/system-design.md).

```text
apps/
    HoroEditor
    horo-engine
    horopak

mcp/
    HoroEngine::Mcp

extension/
    HoroEngine::PluginApi
    HoroEngine::PluginHost

editor/
    HoroEngine::EditorModel
    HoroEngine::EditorServices
    HoroEngine::EditorRenderExtraction
    HoroEngine::EditorViewportScene
    HoroEngine::EditorViewportOpenGL
    HoroEngine::EditorViewportMetal
    HoroEngine::Gui

application/
    HoroEngine::Application

pipeline/
    HoroEngine::Pipeline

render/
    HoroEngine::RenderApi
    HoroEngine::RenderFrontend
    HoroEngine::RenderBackendRegistry
    HoroEngine::RenderModuleAbi
    HoroEngine::RenderModuleHost
    HoroEngine::RenderOpenGL
    HoroEngine::RenderNull
    HoroEngine::RenderVulkan
    HoroEngine::RenderMetal
    HoroEngine::RenderD3D12

scene/
    HoroEngine::SceneModel
    HoroEngine::SceneRuntime

test/
    HoroEngine::TestSdk

physics/
    HoroEngine::Physics

audio/
    HoroEngine::AudioApi
    HoroEngine::AudioRuntime
    HoroEngine::AudioNull

network/
    HoroEngine::NetworkApi
    HoroEngine::NetworkRuntime
    HoroEngine::NetworkTransportNull
    HoroEngine::NetworkTransportGNS

asset/
    HoroEngine::Assets

platform/
    HoroEngine::Platform

foundation/
    HoroEngine::Foundation
```

Concrete renderer targets are equal sibling build products. Release profiles may
package each concrete renderer independently; building the editor core does not
require one installed product artifact to contain every renderer. Product
discovery and loading follow
[Renderer Distribution And Availability](../runtime/renderer-distribution-and-availability.md),
while development/test profiles may link concrete targets directly for fast
contract coverage.

Each target is declared in its own `CMakeLists.txt` with an explicit source
list. Globbing source files is forbidden.

```cmake
add_library(Application)
add_library(HoroEngine::Application ALIAS Application)

target_sources(Application
    PRIVATE
        src/OpenProjectUseCase.cpp
        src/SaveSceneUseCase.cpp
        src/BuildProjectUseCase.cpp
)

target_include_directories(Application
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Application
    PUBLIC
        HoroEngine::SceneModel
        HoroEngine::Assets
        HoroEngine::Pipeline
    PRIVATE
        HoroEngine::CompileOptions
        HoroEngine::Platform
)
```

Namespaced aliases are the stable link API used by other targets. Commands that
create, modify, install, or export a target use its real name (`Application`),
not its alias (`HoroEngine::Application`).

### Initial Editor Bootstrap Targets

The first graphical editor bootstrap is split across narrow targets rather than
one monolithic GUI library:

```text
HoroEngine::EditorModel         scene document, selection, and viewport state
HoroEngine::EditorServices      project/settings/localization/input orchestration
HoroEngine::EditorRenderExtraction generic mesh snapshot and picking extraction
HoroEngine::EditorViewportScene shared camera and scene-view conversion
HoroEngine::Gui                 ImGui screens, modals, panels, and shared controls
HoroEngine::EditorViewportOpenGL matching app-private OpenGL GUI/texture bridge
HoroEngine::EditorViewportMetal matching app-private Metal GUI/texture bridge
HoroEngine::EditorSourceEditor  embedded source editor adapter, LSP/AI presentation hooks
HoroEngine::EditorGraphEditor   embedded node graph editor adapter
HoroEditor                      executable composition root
```

`apps/HoroEditor/main.cpp` owns process entry only and delegates to
`src/editor/app`. It must not accumulate editor architecture.

The initial backend dependencies are SDL3, Dear ImGui, and an OpenGL loader only
if the selected platform/toolchain path requires one. These dependencies are
private to the smallest Horo-owned adapter target. No SDL3, OpenGL, or raw Dear
ImGui backend type may appear in public Horo headers.

Screen, modal, panel, and design-system code is owned by `HoroEngine::Gui`.
Model and services targets do not link Dear ImGui, SDL, OpenGL, or Metal.
Backend-specific GUI texture resolution remains inside the matching app-private
viewport bridge and is selected only by `HoroEditor` composition.

Embedded editor-widget dependencies are private to their adapter targets:

- `HoroEngine::EditorSourceEditor` may depend privately on Zep. Public Horo
  headers expose source-editor value types and controller interfaces, not Zep
  types. `goossens/ImGuiColorTextEdit` remains the fallback for narrow text/diff
  surfaces if Zep proves too invasive.
- `HoroEngine::EditorGraphEditor` may depend privately on `imgui-node-editor`.
  `imnodes` remains an allowed fallback for prototype or simple internal graph
  tools.

These adapters do not own source-file persistence, language intelligence,
behavior validation, shader graph validation, or graph compilation. They render
and translate user interaction into Horo-owned commands.

### Audio Backend Composition

[ADR-067](../../adr/067-platform-audio-backend-strategy.md) owns the planned
output-backend target roles and supported 1.0 matrix. Common Audio API, model, and
runtime targets expose only Horo contracts. WASAPI, Core Audio, PipeWire,
SDL3Audio, and NullAudio are separate concrete targets with private dependencies
and target-platform guards; no backend publishes Windows SDK, Apple framework,
PipeWire/SPA, or SDL audio headers or usage requirements.

Executable composition roots link only the backend peers admitted by that product
and target OS, then call explicit inert registration functions before startup
selection. Linking SDL3 for windowing/input does not activate or require the
SDL3Audio backend. Minimal/headless products may link NullAudio only or omit Audio
entirely. Static initialization never enumerates or opens devices, initializes a
native service, starts a thread/callback, or mutates ambient state.

## Dependency Direction

The build system enforces the dependency direction declared in
[System Design](../foundation/system-design.md):

```text
foundation
    |
platform, assets, scene-model, render-api, audio-api, network-api, plugin-api
    |
physics, audio-runtime, network-runtime, scene-runtime, render-frontend, pipeline
    |
application, editor-model, editor-services
    |
editor-ui, mcp, plugin-host
    |
apps
```

Rules:

- A target may only depend on targets at its own level or below.
- `PUBLIC` dependencies must be reflected in the target's public headers.
- `PRIVATE` dependencies must not appear in public headers.
- ImGui is private to `HoroEngine::Gui`.
- OpenGL, Vulkan, Metal, DirectX/D3D12, and native window-system headers are
  private to their renderer backends and platform adapter targets.
- Native audio, socket, TLS, and dynamic-library headers remain private to
  concrete backends and plugin host targets.
- Third-party dependencies are private unless present in a public contract.

### Header Visibility

The source layout and the compiler-visible layout are intentionally different.
Public headers keep their stable `Horo/...` include spelling in the source tree,
while CMake creates a separate build-tree include view for each owning target.
Linking one target therefore does not make unrelated Horo headers discoverable.

`cmake/HoroPublicHeaderOwnership.cmake` is the authoritative ownership registry.
Every `include/Horo/*.h` file is listed exactly once and configure rejects missing
or duplicate ownership. `horo_configure_target_header_boundary` removes broad
repository include roots from usage requirements and publishes only the owning
target's staged view. Publicly linked dependencies contribute their own views in
the normal CMake dependency chain.

Headers under `src/` are target-private by default and must never be exposed by a
production target's `PUBLIC` or `INTERFACE` include directories. A source header
needed by another target is an architecture decision: move the stable contract
to its owning public surface or introduce a narrowly scoped, non-installed
internal interface target. Adding `${PROJECT_SOURCE_DIR}/src` back as a public
include root is not an allowed migration shortcut.

See [Header Visibility And Ownership](../foundation/header-visibility-and-ownership.md)
for the classification and migration contract.

The architecture dependency check runs in CI and rejects forbidden include or
link directions.

## Dependency Management

Third-party dependencies are fetched at configure time using CMake
`FetchContent`. Dependency versions and options are declared in a central
`cmake/Dependencies.cmake` file.

### Dependency Pinning

Every dependency must be pinned to an immutable Git commit SHA. Tags are not
allowed because a tag can be force-moved. This guarantees reproducible builds
across developer machines and CI.

```cmake
# Wrong — mutable tag
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
)

# Correct — immutable SHA
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG f87239e71e42da91ca317a12eefb82cfbf3393eb
)
```

`GIT_SHALLOW TRUE` must not be combined with a commit SHA. CMake only supports
shallow Git clones when `GIT_TAG` names a branch or tag, which are not accepted
as immutable CI pins. Git dependencies pinned by SHA therefore use a full
clone. For large repositories where history size is material, prefer a release
archive with `URL` and `URL_HASH SHA256` instead of weakening the pin:

```cmake
FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/archive/f87239e71e42da91ca317a12eefb82cfbf3393eb.tar.gz
    URL_HASH SHA256=<reviewed-archive-sha256>
)
```

When updating a dependency, the SHA change is reviewed like a source code
change. The old SHA and the new SHA must both be visible in the diff.

[ADR-104](../../adr/104-default-navigation-provider-and-recast-detour-adoption.md)
selects Recast Navigation commit
`9f4ce64458dfae86e1239c525ddc219c4e9e06f1` for the first grounded NavMesh
provider. Its implementation change must declare that exact revision here,
register it in the dependency fingerprint/manifest and lock the reviewed native
options. Runtime-only products link Detour plus only requested optional modules;
bake/cook compositions add Recast privately. Tags, system packages and a floating
`main` checkout are not compatible substitutions.

### Configure Performance

By default `FetchContent` checks the remote on every configure call. This is
disabled in Horo so that configure stays fast:

```cmake
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
```

With this flag, an absent dependency is still downloaded. For an existing Git
checkout, however, the update step cannot contact the remote. Changing to a SHA
that is not already present locally fails instead of silently using the old
revision. The dependency update procedure below removes the old checkout before
reconfiguring so the new immutable revision is downloaded cleanly.

### Dependency Declaration

`cmake/Dependencies.cmake` declares every third-party dependency and makes it
available:

```cmake
include(FetchContent)

set(HORO_SDL3_REVISION
    "f87239e71e42da91ca317a12eefb82cfbf3393eb"
)
set(HORO_NLOHMANN_JSON_REVISION
    "a123b456c789d012e345f678a901b234c567d890"
)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG "${HORO_SDL3_REVISION}"
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG "${HORO_NLOHMANN_JSON_REVISION}"
)

FetchContent_MakeAvailable(SDL3)
horo_register_resolved_dependency(
    NAME SDL3
    KIND GIT
    EXPECTED_IDENTITY "${HORO_SDL3_REVISION}"
    SOURCE_DIR "${sdl3_SOURCE_DIR}"
)

FetchContent_MakeAvailable(nlohmann_json)
horo_register_resolved_dependency(
    NAME nlohmann_json
    KIND GIT
    EXPECTED_IDENTITY "${HORO_NLOHMANN_JSON_REVISION}"
    SOURCE_DIR "${nlohmann_json_SOURCE_DIR}"
)

if(BUILD_TESTING)
    set(HORO_CATCH2_REVISION
        "6ee0826dcae55ed1e06b2c5701981221e979e1e6"
    )
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG "${HORO_CATCH2_REVISION}"
    )
    FetchContent_MakeAvailable(Catch2)
    horo_register_resolved_dependency(
        NAME Catch2
        KIND GIT
        EXPECTED_IDENTITY "${HORO_CATCH2_REVISION}"
        SOURCE_DIR "${catch2_SOURCE_DIR}"
    )
endif()

horo_write_resolved_dependency_manifest()
```

Production dependencies are made available independently so configure errors
identify the failing declaration clearly. Test-only dependencies are declared
and populated only when the canonical test option is enabled. The same
`BUILD_TESTING` guard controls `add_subdirectory(tests)`, so CLI,
runtime-only, and production release configurations do not download or
configure Catch2.

Every selected FetchContent dependency is registered manually immediately
after its successful `FetchContent_MakeAvailable()` call and inside the same
selection guard. The revision variable is shared by the declaration and
registration so the expected identity cannot drift through duplicated
literals. `horo_write_resolved_dependency_manifest()` is called once after all
dependency-selection branches. The architecture check fails with the dependency
name and required helper call when a selected dependency is not registered. See
[Build Cache](./build-cache.md#layer-2-fetchcontent-cache) for the manifest
schema and CI verification command.

Targets link against the imported target directly:

```cmake
target_link_libraries(HoroEngine::Gui
    PRIVATE
        SDL3::SDL3
        nlohmann_json::nlohmann_json
)

target_link_libraries(test_scene_project_model
    PRIVATE
        HoroEngine::SceneModel
        Catch2::Catch2WithMain
)
```

### Caching Dependencies

`FETCHCONTENT_BASE_DIR` points to the dependency cache location. Locally it
defaults to `build/debug/_deps`. CI caches the fetched sources separately. See
[Build Cache](./build-cache.md).

### Updating A Dependency

For a Git dependency:

1. Change the immutable `GIT_TAG` commit in `cmake/Dependencies.cmake`.
2. Delete the local fetched copy: `rm -rf build/debug/_deps/<name>-src`.
3. Reconfigure: `cmake --preset debug`.
4. Verify the populated checkout resolves to the reviewed commit.
5. Run affected tests and CI jobs.
6. Update lock metadata if the project maintains a dependency lock file.

For an archive dependency, update the archive URL and content hash together:

```bash
curl --fail --location \
  --output dependency.tar.gz \
  "https://github.com/<owner>/<repo>/archive/<commit>.tar.gz"
cmake -E sha256sum dependency.tar.gz
```

Record the printed digest as `URL_HASH SHA256=<digest>`, review both changed
values in the same diff, and delete the local populated source before
reconfiguring. CI repeats archive hash verification during download; a mismatch
is a configure failure.

Changes to `cmake/Dependencies.cmake` and dependency-selection inputs change the
shared dependency fingerprint described in
[Build Cache](./build-cache.md#layer-2-fetchcontent-cache). All workflows
therefore receive a new FetchContent cache namespace automatically. Dependency
updates do not require a manual CI cache purge. Comments and formatting in this
file also change the fingerprint intentionally, so documentation-only edits to
dependency declarations can produce a conservative CI FetchContent cache miss.

### Vendoring Exception

A dependency may be vendored into `third_party/` only if:

- it requires local patches
- it is not available through a reliable Git remote
- its size or configure time makes FetchContent impractical

Vendored dependencies must include a `README.horo.md` describing the version,
purpose, and any local modifications.

A CI check enforces the vendoring rules:

```bash
# ci/vendor-check.sh
errors=0
for dir in third_party/*/; do
    if [ ! -f "${dir}README.horo.md" ]; then
        echo "Missing README.horo.md in ${dir}"
        errors=1
    fi
done
exit $errors
```

The check runs as part of `scripts/dev.py check` and in CI.

## Adding A New Module

1. Create a directory under `src/<module>/` with `include/` and `src/`
   subdirectories.
2. Add `src/<module>/CMakeLists.txt` declaring the real target and its
   `HoroEngine::<Module>` alias.
3. Link the real target privately to `HoroEngine::CompileOptions`. Add
   `HoroEngine::SanitizerOptions` or `HoroEngine::CoverageOptions` only through
   the established preset-controlled pattern.
4. Declare dependencies as `PUBLIC`, `PRIVATE`, or `INTERFACE`; do not publish a
   repository-wide include root.
5. Add public headers under `include/Horo/<Module>/`, assign every new header to
   the real target in `cmake/HoroPublicHeaderOwnership.cmake`, and verify the
   generated per-target consumer target.
6. Decide whether the target uses no PCH, defines a module-specific PCH, or can
   safely `REUSE_FROM Foundation` under the compatibility rules below.
7. Add the module to the root `src/CMakeLists.txt` and verify its dependency
   direction.
8. If the module is part of the installable SDK surface, add an
   `install(TARGETS <real-target> EXPORT HoroEngineTargets ...)` rule and its
   public headers. Never pass the namespaced alias to `install()` or `export()`.
9. Add unit tests under `tests/test_<module>/` and register the appropriate test
   lane.
10. Update [System Design](../foundation/system-design.md) and this document.

## Adding A New Test Target

Test targets are separate executables linked against production libraries.
They must not compile production source files directly.

```cmake
add_executable(test_scene_project_model
    test_scene_project_model.cpp
)

target_link_libraries(test_scene_project_model
    PRIVATE
        HoroEngine::SceneModel
        Catch2::Catch2WithMain
)

catch_discover_tests(test_scene_project_model)
```

Add the test to the fast lane in `tests/CMakeLists.txt` unless it is explicitly
slow, GUI-only, or platform-specific.

## Adding A New App

Application executables live in `apps/<name>/`. Installed product hosts depend on
higher-level libraries plus the renderer component/module host; they do not link
every concrete renderer. The component service resolves one exact verified
module and the module host adapts it into the host-owned
`RenderBackendRegistry`. `RenderFrontend` does not discover modules or select
concrete graphics APIs.

```cmake
add_executable(HoroEditor
    src/main.cpp
    src/HoroEditorApp.cpp
)

target_link_libraries(HoroEditor
    PRIVATE
        HoroEngine::Gui
        HoroEngine::Mcp
        HoroEngine::Application
        HoroEngine::RenderFrontend
        HoroEngine::RenderBackendRegistry
        HoroEngine::RenderModuleHost
        HoroEngine::Platform
)
```

Development, unit-test, and dedicated-server profiles may explicitly link a
concrete backend and call its registration function to avoid product packaging
overhead. Linking still does not activate a backend through static
initialization. Product editor profiles exercise component manifest, signature,
ABI, probe, and controlled-load behavior before registry registration. CLI and
dedicated-server profiles can retain direct `RenderNull` registration without
fetching or linking desktop GPU dependencies.

## Build Outputs

Build artifacts are placed under `build/<preset>/`:

```text
build/debug/
    bin/                     executables and test binaries
    lib/                     static and shared libraries
    tests/                   per-test CMake artifacts
    sdk/                     staged runtime data (shaders, templates)
    _deps/                   FetchContent sources
```

Executables run from `build/debug/bin/` with working directory set to the repo
root. Test fixtures load data from `tests/fixtures/` using paths relative to the
repo root.

## scripts/dev.py

`scripts/dev.py` is the canonical developer interface. It wraps CMake, test
runners, and static checks.

| Command | Purpose |
|---|---|
| `setup` | Install toolchain, configure debug preset, smoke test |
| `build` | Build the default profile (`editor`) for local development |
| `build --profile cli` | Build CLI targets only; default for headless CI jobs |
| `build --profile runtime-only` | Build runtime libraries without editor or GUI |
| `build --profile tests` | Configure with tests enabled and build test targets |
| `build --preset release` | Build a specific preset |
| `build --unity` | Build with unity build enabled |
| `test` | Configure tests if needed, then run the fast test lane |
| `test -- test_a test_b` | Configure tests if needed, then run specific test binaries |
| `test --all` | Configure tests if needed, then run the full test matrix |
| `test-game --project <path> --profile <name>` | Developer convenience wrapper around the public `horo-engine test --project <path> --profile <name>` game-project test command |
| `format` | Format changed or staged files |
| `format --check` | Verify formatting without changing files |
| `format --staged` | Format only staged files |
| `check` | Run format, architecture, header, vendor, and tests-profile fast tests |
| `configure --fresh` | Reconfigure the platform-default preset from scratch |
| `configure --fresh --preset <name>` | Reconfigure an explicit preset from scratch |
| `clean` | Remove build artifacts for the active preset |
| `clean --all` | Remove all build directories |

`scripts/dev.py` is the preferred way to invoke builds and tests. Direct `cmake`
and `ctest` commands are documented here for advanced use and CI.

`horo-engine test` is the public downstream game-test contract. `scripts/dev.py
test-game` exists for developers working inside this repository: it builds or
locates the local `horo-engine` executable and then delegates to the same public
command. Documentation for game projects should show `horo-engine test`; engine
developer workflow documentation may show the wrapper when local build selection
matters.

## Compiler And Linker Options

Common warning and standard settings are applied through interface libraries:

- `HoroEngine::CompileOptions` — warnings, C++20, debug flags
- `HoroEngine::SanitizerOptions` — ASan/UBSan (sanitizer preset only)
- `HoroEngine::CoverageOptions` — coverage instrumentation (coverage preset only)

Target `CMakeLists.txt` files link these interface libraries instead of
repeating compiler flags.

## Precompiled Headers

`HoroEngine::Foundation` provides a precompiled header (PCH) for commonly used
standard library headers. Targets can reuse it to reduce compile time:

```cmake
target_precompile_headers(MyTarget REUSE_FROM Foundation)
```

The Foundation PCH includes headers that appear in most translation units:

```cpp
// foundation/include/HoroEngine/Foundation/pch.h
#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <functional>
#include <memory>
#include <optional>
#include <expected>
#include <filesystem>
#include <algorithm>
#include <ranges>
```

Rules:

- Only Foundation defines a project-wide PCH.
- Other targets may `REUSE_FROM Foundation` or define their own PCH for headers
  that dominate their module.
- `REUSE_FROM Foundation` is allowed only when the consuming target and
  Foundation use the same compiler options, flags, definitions, language
  standard, and relevant architecture settings. Otherwise the target defines
  its own PCH or disables PCH.
- PCH is controlled by `HORO_ENGINE_ENABLE_PCH` and is enabled by default for
  supported local `debug`, `release`, and profile builds.
- Compiler-cache CI jobs set `HORO_ENGINE_ENABLE_PCH=OFF`. The project does not
  enable ccache's relaxed PCH invalidation settings or claim sccache/MSVC PCH
  artifacts as part of the supported cache contract. See
  [Build Cache](./build-cache.md#what-ccachesccache-does-not-cache).
- PCH is disabled for `sanitizer` and `coverage` presets to avoid masking
  instrumentation issues.
- Cross-compilation uses a separate build tree and generates PCH artifacts with
  the target toolchain; host-built PCH artifacts are never shared with Android,
  iOS, or WebAssembly builds.
- Experimental cross-compilation presets keep PCH disabled until each
  toolchain has a CI job proving clean and incremental builds. Enabling PCH for
  a cross preset is an explicit, per-toolchain decision.

## Unity Build

Unity build (jumbo build) combines multiple translation units into a single
compile unit. It reduces header parse overhead and can significantly shorten
full rebuilds of large targets such as `HoroEditor`.

Unity build is **opt-in** because it can hide ODR issues and changes compile
error locations:

```bash
python3 scripts/dev.py build --unity
python3 scripts/dev.py build --unity --profile editor
```

CMake enables unity build per target through:

```cmake
set_target_properties(HoroEditor PROPERTIES UNITY_BUILD ON)
```

CI runs a normal build and a unity build in parallel to ensure both compile and
to catch ODR regressions early.

## Static Analysis

Sonar and Codacy provide the pull-request static-analysis gates. The repository
also keeps an optional `.clang-tidy` configuration for focused local analysis.
Its checks include:

- `cppcoreguidelines-*`
- `modernize-*`
- `performance-*`
- `readability-*`
- `bugprone-*`

Generate a compilation database and run clang-tidy directly when a focused
local investigation benefits from it:

```bash
cmake -S . -B build/tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build/tidy src/scene/model/Scene.cpp
```

## Architecture Dependency Check

CI validates that include and link dependencies follow the documented
hierarchy. The check uses the CMake generated graph and a forbidden-dependency
list in `ci/architecture-check.py`.

Run locally:

```bash
python3 scripts/dev.py architecture-check
```

## Public Header Check

Every public header must compile independently using only its owning target and
that target's declared public dependencies. `horo_add_public_header_consumer_targets`
generates one translation unit per registered header. The configure-time inventory
also rejects a new header until it has exactly one owner. This combination prevents
accidental reliance on the repository-wide source include tree.

## Install Targets

Horo Engine is primarily distributed as executables (`HoroEditor`,
`horo-engine`, `horopak`) and is not yet designed to be consumed as a library
through `find_package(HoroEngine)`. However, internal library targets can be
installed for advanced use cases such as CI artifact staging, packaging, and
future third-party integration.

Install rules are declared per target in the relevant `CMakeLists.txt`:

```cmake
include(GNUInstallDirs)

install(TARGETS Application
    EXPORT HoroEngineTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/HoroEngine/Application
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/HoroEngine
)
```

`Application` is the real build target. `HoroEngine::Application` is an ALIAS
used by in-tree consumers and cannot be installed or exported directly. The
`NAMESPACE HoroEngine::` argument below gives the installed target its
namespaced downstream name.

The export set is collected in `cmake/HoroEngineTargets.cmake`:

```cmake
install(EXPORT HoroEngineTargets
    FILE HoroEngineTargets.cmake
    NAMESPACE HoroEngine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HoroEngine
)
```

A config file produced by `CMakePackageConfigHelpers` allows downstream projects
to consume installed targets:

```cmake
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroEngineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/HoroEngineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HoroEngine
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/HoroEngineConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HoroEngine
)
```

This is considered advanced usage. The primary distribution path remains the
release executables and packaged assets.

### Public Game Test SDK Target

Downstream game tests link the public `HoroEngine::TestSdk` target. The target
contains the runtime-facing test driver contracts documented in
[Game Project Testing](./game-project-testing.md); it does not expose editor
internals, private ECS storage, renderer backend handles, or GUI types.

Generated game templates should wire test targets like this:

```cmake
add_executable(MyGameTests
    src/tests/test_player_controller.cpp
)

target_link_libraries(MyGameTests
    PRIVATE
        MyGame::Gameplay
        HoroEngine::TestSdk
)
```

`horo-engine test` discovers project test descriptors, builds or locates the
configured test harness when the selected profile requires it, executes the
tests, and writes the shared JSON/JUnit result schema. `scripts/dev.py test-game`
is only the in-repository wrapper around that public command.

### find_package Fallback

A future enhancement is to allow `FetchContent` declarations to fall back to
`find_package` when a system package is available. This is useful for Linux
distribution packagers but adds complexity and is not required for the engine's
primary development workflow.

When implemented, each dependency declaration will follow this pattern:

```cmake
find_package(SDL3 3.4 CONFIG QUIET)

if(SDL3_FOUND AND NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "SDL3 package was found but did not provide target 'SDL3::SDL3'")
endif()

if(DEFINED SDL3_VERSION AND NOT SDL3_VERSION VERSION_LESS 4.0)
    message(FATAL_ERROR "Unsupported SDL major version for SDL3 dependency: ${SDL3_VERSION}")
endif()

if(NOT TARGET SDL3::SDL3)
    message(STATUS
        "SDL3 config package not found or incompatible; using pinned FetchContent source"
    )
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG "${HORO_SDL3_REVISION}"
    )
    FetchContent_MakeAvailable(SDL3)
    horo_register_resolved_dependency(
        NAME SDL3
        KIND GIT
        EXPECTED_IDENTITY "${HORO_SDL3_REVISION}"
        SOURCE_DIR "${sdl3_SOURCE_DIR}"
    )
endif()
```

`QUIET` is intentional: an absent or incompatible system package must fall back
to the pinned source dependency. `REQUIRED` would terminate configuration
before that fallback can run. The explicit status message keeps the fallback
visible in configure logs. `CONFIG` is also intentional: module-only packages
are not consumed because project-owned or distribution-provided find modules
can expose inconsistent targets and incomplete version metadata. Config mode
performs the requested minimum version check when the package supplies version
metadata; project code also validates the expected imported target and any
stricter supported-version policy. Dependencies with inconsistent package
names, target names, or version variables are handled by a project-owned
wrapper rather than copying this block with ad hoc variations.

Until then, all dependencies are fetched through `FetchContent` with pinned
SHAs.

## Cross-Compilation

Cross-compilation is currently **experimental**. The following toolchain
presets are planned for mobile and console targets:

| Target | Toolchain | Preset |
|---|---|---|
| Android | Android NDK | `android-debug`, `android-release` |
| iOS | Xcode toolchain | `ios-debug`, `ios-release` |
| WebAssembly | Emscripten | `wasm-debug`, `wasm-release` |

Toolchain files live in `cmake/toolchains/`:

```text
cmake/toolchains/
    android-ndk.cmake
    ios-xcode.cmake
    wasm-emscripten.cmake
```

The Android toolchain file expects `ANDROID_NDK_ROOT` to be set:

```bash
export ANDROID_NDK_ROOT=/opt/android-ndk
cmake --preset android-debug
```

Cross-compilation presets reuse the same target hierarchy and build profiles as
host presets. The active `CookTarget` in the asset pipeline is derived from the
preset name so that the correct cooked assets are produced automatically.

### Cross-Platform Dependency Selection

Each toolchain preset declares target capabilities as CMake cache variables.
Dependencies are selected from capabilities rather than scattered checks for
`ANDROID`, `EMSCRIPTEN`, or a preset name. The initial capability set includes:

- `HORO_TARGET_HAS_PLATFORM_MEDIA`
- `HORO_TARGET_HAS_NATIVE_AUDIO`
- `HORO_TARGET_HAS_DYNAMIC_LOADING`
- `HORO_TARGET_HAS_THREADS`
- `HORO_TARGET_BUILDS_HOST_TOOLS`

`cmake/Dependencies.cmake` guards declaration and population with those
capabilities:

```cmake
if(HORO_TARGET_HAS_PLATFORM_MEDIA)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG "${HORO_SDL3_REVISION}"
    )
    FetchContent_MakeAvailable(SDL3)
    horo_register_resolved_dependency(
        NAME SDL3
        KIND GIT
        EXPECTED_IDENTITY "${HORO_SDL3_REVISION}"
        SOURCE_DIR "${sdl3_SOURCE_DIR}"
    )
endif()
```

Android and WebAssembly presets do not fetch or configure desktop-only
dependencies merely because a host machine could build them. A target backend
must either select a dependency through a capability or provide its own
platform implementation. Unsupported capability combinations fail during
configure with a direct diagnostic.

Tools that execute during the build, such as code generators or asset
processors, are host tools. They are built in a separate native host-tools
build when cross-compiling and are imported into the target build by explicit
path. Target libraries fetched and compiled with the Android NDK, Emscripten, or
another cross toolchain are never executed on the host.

Capability values, build-profile inputs, and gated dependency declarations are
part of the shared dependency fingerprint. Two presets may share a FetchContent
cache only when they select the same dependency graph for the same target
platform.

Cross-compilation presets are not required for local development on the host
platform, but they keep the build system aligned with the multi-platform asset
pipeline documented in [Asset Pipeline](../runtime/asset-pipeline.md).

## Related Documents

- [Build Output UI Reference](../../../mock-studio/designs.md#architecture-runtime-build-output)

- [System Design](../foundation/system-design.md): module boundaries and ownership.
- [Gameplay Module](../extensions/gameplay-module.md): public game-module build and
  SDK dependency boundary.
- [Extension System](../extensions/plugin-system.md): external extension package and ABI build
  contract.
- [Build Cache](./build-cache.md): compiler and dependency caching.
- [Developer Environment](./developer-environment.md): local setup and IDE
  configuration.
- [Testing Architecture](./testing-architecture.md): how tests are organized.
- [Quality And CI](./quality-and-ci.md): CI gates and quality expectations.
