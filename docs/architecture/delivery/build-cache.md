# Build Cache

This document describes the build cache strategy for Horo Engine — local
development and CI. It covers compiler caches (ccache/sccache), CMake
FetchContent dependency caching, cache key design and invalidation, and
stale-cache recovery.

## Motivation

Horo Engine pulls ~10 vendor dependencies through CMake FetchContent at
configure time and compiles ~15k lines of C++20 across the engine, tests,
and tool targets. Without caching, a clean CI job spends 60–80% of its wall
time recompiling unchanged translation units and re-fetching vendor sources
that haven't moved.

The cache strategy separates two independent concerns:

1. **Compiler cache** — ccache (GCC/Clang) or sccache (MSVC). Skips
   recompilation when the preprocessed source + compiler flags haven't
   changed.
2. **FetchContent cache** — GitHub Actions `actions/cache@v4`. Avoids
   re-cloning vendor repos when `CMakeLists.txt` hasn't changed.

## Local Development

### ccache (macOS / Linux)

ccache is the recommended compiler cache for local Clang and GCC builds.
It intercepts the compiler invocation, hashes the preprocessed source and
flags, and retrieves a cached object file on hit.

**Install:**

```bash
# macOS
brew install ccache

# Linux (Debian/Ubuntu)
sudo apt install ccache

# Linux (Fedora)
sudo dnf install ccache
```

**Configure CMake with ccache:**

```bash
cmake --preset debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

Once configured, every subsequent `cmake --build --preset debug` uses
ccache transparently. No changes to `scripts/dev.py` are required because
the launcher variables are stored in `CMakeCache.txt`.

**Verify it's working:**

```bash
ccache --show-stats
# Look for: cache hit (direct) > 0 after a second build
# A zero-hit first build is normal — the cache is cold.
```

**Cache location and sizing:**

| Platform | Default cache dir      | Max size (default) |
|----------|------------------------|---------------------|
| macOS    | `~/Library/Caches/ccache` | 5 GB             |
| Linux    | `~/.cache/ccache`         | 5 GB             |

Adjust with:

```bash
ccache --max-size 10G        # increase to 10 GB
ccache --show-stats           # view hit/miss rates and cache size
```

### sccache (Windows / cross-platform)

sccache is the compiler cache for MSVC on Windows. It also supports GCC
and Clang but is not the primary choice for those toolchains in this
project.

**Install:**

```bash
# Windows (via cargo)
cargo install sccache

# macOS (alternative)
brew install sccache
```

**Configure CMake with sccache (MSVC):**

```bash
cmake --preset debug-msvc \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
```

**Verify:**

```bash
sccache --show-stats
```

### What ccache/sccache does NOT cache

- Link steps. Only compilation (`.cpp` → `.o` / `.obj`) is cached.
- CMake configure/generate steps. These always run.
- Custom commands (shader copy, asset staging).
- Horo does not rely on compiler-cache hits for precompiled-header artifacts.
  ccache has limited PCH support, but enabling it requires relaxed invalidation
  settings such as `pch_defines` and `time_macros`. The project does not accept
  those correctness trade-offs in CI. sccache/MSVC PCH support is not part of
  the supported cache contract either.

Compiler-cache CI jobs therefore configure
`HORO_ENGINE_ENABLE_PCH=OFF`. PCH remains available for supported local and
non-cache performance builds as described in
[Build System](./build-system.md#precompiled-headers). Object compilation is
still cached normally when PCH is disabled. A toolchain may enable PCH together
with a compiler cache only after a dedicated correctness test proves clean,
incremental, and cache-restored builds produce identical outputs.

## GitHub Actions CI

CI uses two cache layers per job across the validation, UI, and release
workflows. The workflow files implement the contracts in this document; action
versions and runner labels may change without changing those contracts.

### Cache Storage Ownership

Only runs with `github.ref == 'refs/heads/main'` may publish GitHub Actions
caches. Pull requests, feature branches, and tags consume compatible caches
without uploading new snapshots. A miss on those refs builds normally and keeps
any new compiler entries on the temporary runner only. This avoids one stored
compiler/dependency generation per PR competing for the repository cache quota.

Native and Sonar compiler restores select the latest compatible saved main
checkpoint. They never wait for an in-progress main build. Dependency/scanner
restores retain their compatibility keys; their save steps also require main.
Other workflows split restore and save explicitly, and ccache-action's post-job
save input is enabled only on main. Direct sccache GitHub-backend jobs use
`SCCACHE_GHA_RW_MODE=READ_ONLY` outside main; directory-snapshot sccache jobs
disable that backend so there is no second upload path.

Existing caches are not removed by changing writer policy. Main must complete
its initial checkpoints before other refs can benefit from them, and old main
generations still share the repository quota and eviction policy. Cache cleanup
must identify exact refs/keys and preserve needed main checkpoints.

### Native Test And Audio Checkpoints

CI groups Debug and Release audio qualification into one runner per platform,
with separate build directories and JUnit reports. The `audio-safety-v2`
checkpoint contains both compiler configurations under `Debug-Release` and is
bounded to 512 MB (the previous two 256 MB budgets combined). The first run of
this namespace is cold until main saves it. Both configurations must pass the
aggregate gate; a failure in one does not skip the other.

Windows CLI/process/ZIP, VFX, cinematic and prefab qualification share the
`windows-contracts-v1` headless configure/build and a 1 GB compiler checkpoint.
Their test steps and JUnit reports remain separate, and every suite must pass.
The SDL input composition remains independent. These grouped checkpoints use
the same main-only writer policy and never store test results or build trees.

The active `ci.yml` native-test and Audio Real-Time Safety jobs restore caches
through `.github/actions/restore-native-cache` and checkpoint them through
`.github/actions/save-native-cache`. This bounded rollout uses the existing
explicit CMake compositions; it does not assume the target preset/profile tools
described below already exist.

- Compiler snapshots are isolated by OS, architecture, full C and C++ compiler
  versions, build profile, and Debug/Release mode. The composite action probes
  fixed GCC, Clang, or MSVC commands selected by the CI matrix; the Python
  identity helper validates their version output without launching processes.
  Unsupported tool names and malformed versions fail before cache restore.
  An immutable run-ID/attempt
  suffix permits every successful build to publish an updated snapshot. Restore
  selects the latest compatible main snapshot. Only main publishes checkpoints.
  A running main workflow is never awaited: only an already saved checkpoint is
  available, even if that workflow is still executing tests.
- FetchContent caches store populated `*-src` directories and `*-subbuild`
  download metadata, including the stamps that prevent a redundant clone.
  Dependency `*-build` object directories are recreated. Exact keys include the OS,
  architecture, composition profile, CMake major/minor, and a dependency digest
  from `scripts/ci_cache_identity.py`. The digest includes the root CMake
  capability defaults, `Dependencies.cmake`, dependency helper/policy files, and
  GNS protobuf setup; it normalizes CRLF and excludes engine target/source edits.
  Dependency caches have no partial-match fallback. Change the composition
  profile namespace when its explicit capability selection changes.
- The runner's `--fetchcontent-base-dir` option passes the directory as a CMake
  cache variable. `dev.py build` completes compilation, cache saves run next,
  and `dev.py test --skip-build` preserves the normal test filters and JUnit
  output. The latter requires a configured test directory and rejects an empty
  CTest selection. Ordinary `dev.py test` continues to configure and build.
- Native-test compiler stores are capped at 2 GB; each focused Audio store is
  capped at 256 MB. Imported statistics are reset before compilation, and matched
  keys and current-run statistics are printed. Cache checkpoint failures are
  reported without suppressing tests. GitHub repository-wide cache usage must
  be reviewed before raising these limits or extending the rollout.

The first main run must seed these new namespaces. The first PR may therefore
be cold; subsequent runs reuse compatible completed checkpoints. These changes
do not enable the currently disabled full Windows native-test entry or change
test selection. UI, release, Network GNS, and the other focused jobs are separate
follow-up rollout scopes. Sonar compiler/coverage and analysis caches remain in
their existing namespaces.

### Layer 1: Compiler Cache

| Platform | Cache tool | Cache key |
|----------|------------|-----------|
| macOS    | ccache     | `ccache-v1-${{ github.job }}-${{ runner.os }}-${{ steps.compiler-version.outputs.version }}` |
| Linux    | ccache     | `ccache-v1-${{ github.job }}-${{ runner.os }}-${{ steps.compiler-version.outputs.version }}` |
| Windows  | sccache    | `sccache-v1-${{ github.job }}-${{ runner.os }}-${{ steps.compiler-version.outputs.version }}-${{ hashFiles('**/CMakeLists.txt', '**/CMakePresets.json') }}` |

Key design decisions:

- **Compiler identity:** Every compiler-cache key includes a normalized
  compiler identity obtained after toolchain setup. The identity includes the
  compiler family and full version, for example `clang-19.1.7` or
  `msvc-19.44.35207`. ccache and sccache also validate the compiler internally,
  but the external namespace prevents CI from downloading a cache that cannot
  produce hits after a runner-image toolchain upgrade.

- **ccache (macOS, Linux):** The cache action manages ccache storage with LRU
  eviction. Each materially different job or build profile has its own
  namespace. No source hash is included because ccache's content-addressed
  store already invalidates changed translation units, headers, and flags.

- **sccache (Windows):** The sccache directory is persisted through the GitHub
  cache service. The primary key includes CMake inputs so dependency and flag
  changes create a new cache generation, but restores may seed that generation
  from the newest cache for the same job, OS, and compiler:

  ```yaml
  key: sccache-v1-${{ github.job }}-${{ runner.os }}-${{ steps.compiler-version.outputs.version }}-${{ hashFiles('**/CMakeLists.txt', '**/CMakePresets.json') }}
  restore-keys: |
    sccache-v1-${{ github.job }}-${{ runner.os }}-${{ steps.compiler-version.outputs.version }}-
  ```

  A fallback restore is not an empty or pristine store. It is an accepted
  performance trade-off: sccache reuses an object only when its internal key
  matches the compiler, architecture, command line, preprocessing inputs, and
  other compilation state. Incompatible objects remain unused. After a
  successful primary-key miss on main, GitHub saves the resulting updated store
  under the new primary key. Other refs only restore. The compiler version in
  `restore-keys` prevents seeding across compiler namespaces.

- **UI workflow (`ci-ui.yml`):** Uses a separate compiler-cache job or profile
  namespace. The UI automation binary is a different build target with
  different compile definitions, so isolation avoids cache thrashing.

- **Release workflow (`release-binaries.yml`):** Uses a separate
  compiler-cache job or profile namespace. Release jobs build with
  `--preset release` and different compiler flags. FetchContent still uses the
  common dependency fingerprint unless the release profile selects a genuinely
  different dependency graph.

### Layer 2: FetchContent Cache

Vendor dependencies (SDL3, glm, stb, nlohmann_json, tinygltf, ufbx,
xxhash, lz4, bc7enc, optional glslang/Vulkan headers) are fetched at
configure time via CMake FetchContent. Every Git-based FetchContent dependency
is pinned to an immutable commit SHA. Moving tags and branches are not valid CI
pins.

All workflows calculate the same dependency fingerprint with
`scripts/dependency-state.py`. Before cache restore, the script first resolves
the selected preset and profile into a normalized capability file, then hashes
the dependency inputs:

```bash
python3 scripts/dependency-state.py capabilities \
  --preset "${HORO_CMAKE_PRESET}" \
  --profile "${HORO_BUILD_PROFILE}" \
  --output "${RUNNER_TEMP}/horo-capabilities.json"

python3 scripts/dependency-state.py fingerprint \
  --dependencies cmake/Dependencies.cmake \
  --profile "${HORO_BUILD_PROFILE}" \
  --capabilities-file "${RUNNER_TEMP}/horo-capabilities.json" \
  --github-output "${GITHUB_OUTPUT}"
```

The `capabilities` command reads the preset inheritance chain and the canonical
profile mapping, rejects missing or contradictory capability values, and writes
sorted JSON booleans. The `fingerprint` command writes a lowercase hexadecimal
SHA-256 value to the `sha256` output. Its canonical payload contains:

- a fingerprint schema version owned by the script
- the exact bytes of `cmake/Dependencies.cmake`, including Git pins, archive
  URLs, `URL_HASH` values, and dependency-selection logic
- the selected build profile
- sorted capability names and boolean values from the normalized capability
  file
- dependency-provider policy, including whether system-package fallback is
  enabled

Line-ending normalization is the only source-file normalization. Comments and
formatting changes in `cmake/Dependencies.cmake` intentionally change the
fingerprint. This may cause a conservative cache miss, but it prevents selection
logic from changing without invalidation. The same script and schema are used
by validation, UI, and release workflows; workflow-specific prefixes must not
hide a dependency change from another workflow.

CI restores FetchContent sources only for an exact dependency fingerprint:

```yaml
- uses: actions/cache@v4
  with:
    path: ${{ github.workspace }}/.cmake/fetchcontent
    key: fetchcontent-v1-${{ runner.os }}-${{ steps.cmake-version.outputs.major-minor }}-${{ steps.dependency-fingerprint.outputs.sha256 }}
```

FetchContent caches do not use broad `restore-keys`. A partial restore would
make dependency correctness depend on the update behavior of the installed
CMake version and on which Git objects happened to exist in the restored
archive. A fingerprint miss therefore starts with an empty dependency cache
and downloads the declared revisions.

`FETCHCONTENT_BASE_DIR` is passed as a CMake cache variable, not only as a
process environment variable:

```bash
cmake --preset debug \
  -DFETCHCONTENT_BASE_DIR="${{ github.workspace }}/.cmake/fetchcontent"
```

When `FETCHCONTENT_UPDATES_DISCONNECTED` is enabled, exact fingerprint restores
and immutable pins remain mandatory. Configure writes
`build/<preset>/horo-dependencies.json`, containing each selected dependency's
name, kind, expected identity, and resolved source directory.
After each selected dependency is populated, `cmake/Dependencies.cmake`
registers the result through the project-owned
`horo_register_resolved_dependency()` helper. This is an explicit call made
immediately after the corresponding `FetchContent_MakeAvailable()` call and
inside the same capability or test guard; it is not an automatic FetchContent
hook. At the end of dependency setup,
`horo_write_resolved_dependency_manifest()` writes the JSON file from those
records. The dependency architecture check rejects a selected FetchContent
dependency that has no resolved-manifest record and names both the dependency
and the missing registration call in its diagnostic. The declaration pattern
is shown in [Build System](./build-system.md#dependency-declaration).

CI then runs:

```bash
python3 scripts/dependency-state.py verify \
  --resolved-manifest build/<preset>/horo-dependencies.json
```

For Git dependencies, verification runs `git rev-parse HEAD` in the resolved
source directory and compares the full commit ID with the expected immutable
SHA. It also rejects a missing repository or an unresolved symbolic revision.
For archive dependencies, CMake's `URL_HASH` check is the content-integrity
authority; the verifier confirms that the resolved manifest records the
expected URL and SHA-256 and that the populated source directory exists. A
missing or mismatched dependency fails before compilation.

`scripts/dependency-state.py` owns both fingerprint and verification commands.
Tests cover deterministic output, line-ending normalization, capability
ordering, Git mismatches, missing sources, and archive manifest mismatches.

### Devcontainer Cache Seeds

The scheduled `devcontainer-cache-seed` CI job may publish a dedicated Linux
devcontainer cache seed as a developer convenience. It is separate from build,
package, and debugging artifacts and is produced only by a successful workflow
on the protected default branch. Pull-request and unprotected-branch runs
cannot publish a seed. Its exact artifact name is:

```text
devcontainer-cache-seed-v1-linux-<arch>-<compiler-identity>-<cmake-major-minor>-<dependency-fingerprint>
```

The artifact name and embedded manifest identify:

- cache-seed schema version
- producer workflow, run ID, and source commit
- runner OS and architecture
- normalized compiler family and full version
- CMake major/minor version
- dependency fingerprint
- ccache configuration and payload digest
- FetchContent payload digest

The devcontainer computes the same identity tuple locally before requesting a
seed. A setup helper downloads only an exact-name candidate, validates every
manifest field and SHA-256 digest before extraction, and rejects unexpected
archive paths. ccache's internal content keys remain authoritative for object
reuse. FetchContent is restored only for an exact dependency fingerprint and
configure must still write `horo-dependencies.json`; the resolved dependency
verifier runs before compilation.

Artifact absence, expiry, download failure, or any validation mismatch produces
a visible warning and an empty-cache setup. The helper discards the complete
candidate rather than retaining a partially extracted cache. Cache seeds are
therefore optional performance inputs, never dependency or build correctness
authorities.

### Cache Invalidation Triggers

| What changed                    | Which caches invalidate |
|---------------------------------|-------------------------|
| `.cpp` / `.h` source            | Compiler cache (per-file, automatic) |
| Dependency manifest or dependency-selection input | FetchContent cache in every workflow |
| Comment or formatting in `cmake/Dependencies.cmake` | FetchContent cache in every workflow (intentional conservative invalidation) |
| `CMakeLists.txt` / `CMakePresets.json` | Windows sccache primary key; same-compiler fallback remains available |
| Compiler flags (preset changes) | Compiler cache (automatic via content hash) |
| Compiler version upgrade (CI runner image) | Compiler cache namespace changes |
| CMake major/minor upgrade | FetchContent cache namespace changes |
| New vendor dependency           | FetchContent cache misses |
| Vendor version bump             | FetchContent cache misses |

The CMake major/minor component does not represent dependency source identity.
It isolates the CMake-generated population, stamp, sub-build, and dependency
build metadata stored under `FETCHCONTENT_BASE_DIR`. Patch releases share a
namespace; major or minor upgrades receive a clean metadata generation. If the
cache is later narrowed to immutable source directories only, this key
component can be removed after cross-version restore tests.

### Cache Health And Capacity

Compiler-cache statistics are emitted for every CI build, including failed
builds. The job summary records:

- cache hits, misses, and hit rate
- current cache size and configured maximum
- cleanup or eviction activity
- compiler identity and restored cache key

The cache size is configured explicitly in workflow code rather than relying
on the cache action's default. The initial ccache allocation may be 500 MB, but
it is a measured starting point, not a permanent architectural limit. CI emits
a warning when usage exceeds 90% of the configured maximum, when cleanup occurs
on consecutive runs, or when the warm-build hit rate falls materially below
the rolling baseline. Capacity is increased only after the statistics show
that useful entries are being evicted.

Hit-rate alerts exclude known cold-cache events such as a compiler namespace
change or an intentional clean-build run.

### Concurrency

CI workflows use:

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

This means a new push cancels the in-progress run for the same ref.
Cancelled runs do not corrupt cache entries — GitHub's cache restore
is always from the last successful save.

`ci.yml` and Sonar use `cancel-in-progress` only for pull requests. An active
main run may finish its cache checkpoints; newer main pushes replace a pending
run rather than repeatedly interrupting the canonical cache producer. Stale PR
runs remain cancellable.

### Scheduled Cold Builds

Scheduled validation runs at least weekly as part of the Scheduled Validation
pipeline defined in
[Quality And CI](./quality-and-ci.md#scheduled-and-asynchronous-validation).
Compiler-cache and FetchContent restore are disabled independently in every
matrix job. Each job starts from empty build and dependency directories,
downloads dependencies from the immutable manifest, and performs resolved
revision verification. The cold-build jobs retain the normal compiler-cache CI
configuration, including `HORO_ENGINE_ENABLE_PCH=OFF`; only cache restore and
save are disabled. This keeps cold builds comparable with pull-request CI and
isolates reliance on restored state from PCH-specific behavior.

A separate scheduled PCH smoke job configures
`HORO_ENGINE_ENABLE_PCH=ON` on each supported native toolchain, starts from an
empty build directory, and performs both clean and incremental builds without
claiming compiler-cache hits for PCH artifacts. It validates the production PCH
path without weakening the cold-build invariant or changing the normal CI
configuration.

The cold-build matrix is the complete supported platform and configuration
matrix, not a Linux-only representative subset. It includes Linux, macOS, and
Windows plus every production renderer/configuration required by Quality And
CI. Experimental cross-compilation targets join the cold matrix when they
become supported targets. A platform cannot claim scheduled cache validation
from another platform's cold build.

This job detects undeclared dependencies, accidental reliance on restored
state, and cache-key mistakes before they appear in release validation. It is
the proactive counterpart to the recovery procedures below and implements the
"clean builds without warm caches" requirement in
[Quality And CI](./quality-and-ci.md#scheduled-and-asynchronous-validation).

## Stale Cache Recovery

### Symptoms of a stale build

- Build succeeds but runtime behavior is wrong (old object files
  linked with new headers).
- CMake configure fails with cryptic FetchContent errors after a
  dependency version change.
- Test failures that disappear after a clean rebuild.
- Linker errors referencing removed symbols.

### Recovery procedures (ordered least → most destructive)

**0. Inspect and verify the resolved dependency manifest (no state changes):**

```bash
cmake -E cat build/debug/horo-dependencies.json
python3 scripts/dependency-state.py verify \
  --resolved-manifest build/debug/horo-dependencies.json
```

The manifest is written to `${CMAKE_BINARY_DIR}/horo-dependencies.json`, which
is `build/<preset>/horo-dependencies.json` for the standard presets. Inspect it
before deleting state to compare expected identities and resolved source
directories. A missing manifest means configure did not complete dependency
setup and should be rerun before deeper cache recovery.

**1. Force reconfigure (preserves compiler cache):**

```bash
cmake --preset debug -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build --preset debug
```

Re-running configure regenerates the build system. Compiler cache entries
remain valid.

**2. Fresh configure (discards CMake cache, preserves compiler cache):**

```bash
cmake --fresh --preset debug -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

`cmake --fresh` deletes `CMakeCache.txt` and re-runs CMake from scratch.
This resolves incorrect cache variables or broken generator state. It does not
delete an external `FETCHCONTENT_BASE_DIR`; dependency source corruption must
be handled separately. Previously compiled object files in the build tree may
become orphaned but won't cause harm — the next build will recompile anything
whose dependency graph changed.

**3. Clean build directory (preserves compiler cache):**

```bash
python3 scripts/dev.py clean       # remove build/debug/
# or
python3 scripts/dev.py clean --all # remove build/ entirely
```

Deletes all build artifacts. The ccache/sccache stores are external to
the build directory and survive. Next build recompiles everything but
hits the compiler cache for unchanged translation units.

**4. Full reset (nuclear option):**

```bash
python3 scripts/dev.py clean --all
ccache --clear    # or: sccache --clear
rm -rf .cmake/fetchcontent
```

Only needed when a cache store is corrupted (extremely rare) or when disk space
is critically low.

### When to clear the compiler cache on CI

CI ccache/sccache stores accumulate across runs. Cache eviction is LRU-
based and handled by the cache action provider (ccache-action) or GitHub
(`actions/cache`). The cache-health summary makes sustained capacity pressure
visible. Manual clearing on CI is rarely needed.

Force a CI cache miss by changing the cache key in the workflow file
(for example, incrementing an explicit cache schema version). This is the
supported escape hatch when the cache store is suspected of causing build
problems.

### FetchContent cache corruption

If FetchContent restores a partial or corrupted source tree:

```bash
# Delete the cached FetchContent sources locally
rm -rf build/debug/_deps
# or for CI cache paths
rm -rf .cmake/fetchcontent
```

CMake will re-fetch on the next configure. On CI, bump the cache key
prefix to force a miss across all jobs.

## Commands Cheat Sheet

```bash
# Local dev: configure with ccache (macOS/Linux)
cmake --preset debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# Local dev: configure with sccache (Windows MSVC)
cmake --preset debug-msvc \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=sccache

# Check compiler cache hit rate
ccache --show-stats       # macOS/Linux
sccache --show-stats      # Windows

# Recover from stale CMake state
cmake --fresh --preset debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# Clean build output (compiler cache survives)
python3 scripts/dev.py clean

# Clean everything including build dirs
python3 scripts/dev.py clean --all

# Reset compiler cache (rare)
ccache --clear
sccache --clear

# Clear FetchContent cache manually
rm -rf build/debug/_deps
rm -rf .cmake/fetchcontent
```

## Limitations

- ccache does not accelerate link time. For large link jobs, consider
  `lld` (macOS/Linux) or `/DEBUG:FASTLINK` (MSVC, already used in CI).
- sccache on Windows requires the cache directory to be on the same
  drive as the build tree for hard-link support.
- FetchContent archives remain OS-specific because path layout, archive
  metadata, and generated population state may differ across runners.
- Compiler-cache capacity must grow with measured object volume. The configured
  maximum and warning thresholds are operational settings reviewed from CI
  statistics, not fixed assumptions about project size.
