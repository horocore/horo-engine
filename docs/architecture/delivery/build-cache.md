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
ccache transparently through the launcher variables in `CMakeCache.txt`.

For a shared main-derived checkpoint across local worktrees, use the developer
runner instead:

```bash
python3 scripts/dev.py cache prepare -j 2
python3 scripts/dev.py cache status
python3 scripts/dev.py build HoroEditor -j 2
```

`cache prepare` fetches `origin/main`, pins its exact commit, and builds an
isolated detached checkout. It never switches or incorporates changes from the
calling worktree. Run it again after main changes to refresh the checkpoint;
`--offline` uses the already fetched `origin/main` reference. `--target HoroFoundation`
warms only that target's compilation closure, and `--type Release` selects a
Release preparation. Additional configuration is supplied as repeatable
`--cmake-arg=-DNAME=value` options, also accepted by `build`, `test`, and `check`.
Use the same composition and build type to benefit from the prepared entries.

The runner stores compiler entries outside the repository in the host's user
cache directory, keyed by the Git common directory, OS, and architecture. Linked
worktrees discover the same completed checkpoint automatically through `build`,
`test`, `check`, and `run editor`. No machine-specific paths are committed. A
second computer creates its own checkpoint with the same command; these are
local stores rather than copied object directories or a remote cache service.

Preparation copies the previous compiler store into a new candidate without
hard links. The old generation remains published until the new main build
succeeds and its source is still clean. Publication replaces a small ready
manifest atomically. Failed preparation preserves the ready generation and
keeps its failed checkout available for diagnosis. Old completed generations
are removed only when their reader leases have released; leased generations
are checked again during a later refresh. Unknown directories are preserved.

Feature worktrees consume the completed compiler store read-only. Changed
sources still compile normally into their own build directories; only main
preparation publishes shared compiler entries. ccache's content checks remain
authoritative for source, headers, compiler, and flags. GCC/Clang debug paths
are normalized for different worktree directories, and PCH is disabled for
managed cache builds. Compiler flags from the existing build or explicit
configure arguments are preserved. Other toolchains may miss across build
directories; there is no relaxed hashing or ignored-header workaround.

The default budget is 4 GB per generation, so allow space for two compiler
stores plus the candidate build. The runner serializes configure/compilation
across linked worktrees and defaults to two compiler jobs. This lock covers
`dev.py` builds; direct CMake invocations still require orchestration. Cache
refresh enters the same build queue. Tests and editor execution happen after
the build lock is released. A cache checkpoint records successful compilation,
not passing tests or a Sonar/Codacy gate.

On macOS/Linux, cancellation verifies and terminates owned subprocesses before
releasing build and cache leases. On Windows, each developer command starts
suspended and enters a private non-breakaway Job Object before its initial
thread resumes. Cancellation and ordinary command completion drain the entire
job and verify zero active processes before releasing the build lock. Verified
cancellation returns exit code 130. If termination cannot be verified, the runner
records `cleanup-unverified.json` in its cache root and blocks later builds and
generation retirement. Audit the remaining process tree before removing this
marker; it is not an automatic retry or cleanup shortcut.

Missing or invalid checkpoints fall back to an ordinary build. Explicit
`--compiler-launcher`, cache environment variables, and PCH requests take
precedence over automatic discovery. `--no-cache` disables discovery. This
first managed lifecycle supports ccache on macOS and Linux. Windows preparation
and automatic discovery remain disabled; explicit Windows sccache configurations
continue through the existing launcher option. Windows process containment and
cancellation have independent native regression coverage.

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
  from `scripts/dev.py`. The digest includes the root CMake
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

### External storage migration contract

The selected external storage backend is an S3-compatible object store, initially
Cloudflare R2. GitHub Actions remains the execution service. Migration must keep
the existing test selection, instrumented coverage, static analysis and quality
gates. A cache hit is never evidence that a test or quality gate passed.

There are two independent payload types:

- Dependency SDK packages contain compiled vendor libraries, headers, generated
  configuration, required host tools, notices and test fixtures. A downloaded
  source archive is not an SDK package. In particular, current tests use ufbx's
  `data` directory, Catch2's CMake integration and ImGui test-engine headers;
  removing the source tree without supplying these would reduce validation.
- Compiler caches contain Horo translation-unit results validated by ccache or
  sccache. They do not contain test results or replace link/test execution.

SDK identities must include dependency pins and Horo patches, package recipe,
target OS and architecture, compiler identity, platform SDK, C++ ABI/CRT,
configuration, relevant feature switches and instrumentation flags. Native
Debug, Release and Sonar coverage packages are distinct. Consumers reject a
manifest mismatch and build from the pinned sources. Packages must work after
relocation and must not expose producer-machine include paths.

Only trusted main runs may publish. PRs, including fork PRs, receive no storage
write credentials and do not start producer jobs. Existing successful main
builds supply publication inputs; a serialized publisher owns the dedicated
bucket's manifests and retention. Before consuming any uploaded input it checks
the producing repository, event, main ref and commit. It must not execute code
from an untrusted artifact. Existing per-platform PR job limits and cancellation
of obsolete PR commits remain in place.

Read access must not require a write credential. Public OSS payloads may be
served through a dedicated HTTPS domain; production traffic must not depend on
R2's rate-limited development endpoint. Reads have bounded timeouts. Missing,
incompatible, corrupt or unavailable payloads produce an explicit cache-miss
diagnostic and fall back to the normal build. Archive validation and staging
must complete before a package becomes visible to CMake.

The initial application storage budget is 9,000,000,000 bytes in a dedicated
bucket. This leaves headroom under R2's advertised 10 GB-month free storage
allowance; it is not a billing cap and does not bound request charges or other
buckets. Account-wide usage and request counts must also be checked before
claiming zero cost. Publication accounts for old/new generation overlap and
incomplete multipart uploads. If the candidate cannot fit while preserving all
currently published packages, skip publication and report capacity pressure.
Do not delete the only usable package merely to make an upload fit.

The pure planner in `.github/scripts/external-cache-policy.js` counts the whole
bucket, preserves objects referenced by published manifests, and only proposes
removing unreferenced objects in `horo-ci/v1/`. A publisher must collect a complete
paginated inventory, all manifest references and incomplete upload reservations,
then revalidate under the single-writer lock before applying this plan. Unknown
objects count toward capacity but are not eligible for deletion.

The SDK payload format is implemented by
`.github/scripts/dependency_package.py` (Python 3.11 or newer). A producer stages
the complete relocatable SDK, supplies an independently computed identity JSON
and invokes `pack --root <staging> --identity <identity.json> --output <sdk.tar.gz>`.
A consumer invokes `restore --archive <sdk.tar.gz> --identity <expected.json>
--destination <absent-directory>`. Neither operation accesses the network.
The identity has exactly these fields: `platform`, `architecture`, `compiler`,
`sdk`, `abi`, `configuration`, `dependencies`, `recipe`, `instrumentation` and
`features`. Explicit values such as `none` distinguish disabled instrumentation
from missing metadata. These values must describe the actual resolved build;
the payload tool does not discover toolchains or infer ABI compatibility.

The payload CLI's working directory is its independent filesystem authority.
CLI staging/verify roots, package outputs and restore destinations must resolve
strictly inside that directory; read-only verification may use the directory
itself. Leaf symlinks, absolute paths outside it, parent traversal and symlink
parents resolving outside it are rejected before writing. Archive and identity
inputs may be read from another directory; those inputs do not grant write
access there. Library calls remain available for trusted callers that own their
private temporary directories.

CMake explicitly invokes payload verification/restoration from the selected build
directory, package production from the build directory, and registry fetches
from the selected SDK cache directory. This also supports out-of-source builds
outside the repository. Registry cache-directory creation is bounded and
nonfatal: an unavailable cache retains the source-build fallback. Manual CLI
users must first select the intended build/cache working directory; do not add
a CLI argument that authorizes an arbitrary write root.

The archive starts with a bounded JSON manifest and contains regular files only.
The producer materializes symlinks when staging, freezes staging during packing,
and includes executable host tools as executable files. The consumer checks
identity, per-file SHA-256 and sizes, rejects extra/missing members and unsafe or
non-portable paths, and stages outside the final destination. Only a completely
validated payload is renamed into place. A verified manifest receipt remains
inside the destination. Digests detect corruption; transport authentication and
trusted-writer restrictions establish provenance. An archive's own digest list
does not establish publisher trust.

The first CMake integration is Unicode/ICU. Configure a producer with
`HORO_BUILD_DEPENDENCY_SDK_PACKAGES=ON` and build `horo-package-unicode` after the
normal native build. Archives appear in `build-dir/dependency-sdk/packages`.
A consumer sets `HORO_DEPENDENCY_SDK_CACHE` to a directory of trusted archives.
`HoroDependencySdk.cmake` computes the identity from the recipe, compiler,
platform SDK and effective global build flags. It restores only the matching
archive and verifies an existing restored payload again on reconfigure.
Unicode keeps the same target/alias, interface definitions and data installation
contract. A missing Python 3.11 interpreter, missing archive or failed verification
uses the existing source build. Both options are disabled/empty by default.

The initial integration supports single-configuration native builds. Coverage,
profiling and sanitizer flags deliberately retain source compilation until
instrumented object/profile relocation is qualified. In particular, this
integration must not replace Sonar's coverage objects with an ordinary native
SDK. Extending the package path to these configurations is outstanding work.

Implementation status: the capacity planner and payload format have offline
regression coverage, including linking a relocated native fixture after removing
its producer tree. The real pinned ICU source was also built and packaged on
macOS with AppleClang 21, Debug and two build workers. Its package contained the
library, public headers, license and dictionary data. A fresh consumer configured
with FetchContent fully disconnected imported the library, created no `_deps`
directory and compiled zero ICU objects (the producer compiled 203). Both ran
the same NFC normalization probe with the packaged ICU data. The 24,139,674-byte
archive's local producer build/pack took 58.036 seconds and consumer compile/link
took 0.666 seconds according to Ninja logs; these exclude configure, network and
test time and are not hosted or full-engine timings.

The current transport is public GHCR through
`.github/scripts/dependency_registry.py`; readers use an empty credential store
and only canonical main producers publish. The native SDK producer has completed
on Linux, macOS and Windows, and Linux job `120596826395` verified a published
package and configured without recompiling Unicode. Other dependency recipes,
full platform consumers and stable hosted timing still require qualification.
The earlier bucket capacity planner remains an offline alternative, not a
configured R2 service. Compiler checkpoints remain in Actions storage and can
still be evicted. The 10–15 minute target is not yet achieved.

### Compiler checkpoint fallback in public GHCR

The compiler exporter uses the existing public SDK package with distinct
`compiler-v1-<namespace-sha256>` tags and the
`application/vnd.horo.compiler-cache.v1` artifact type. It does not reuse the
SDK compatibility schema. Its namespace is the existing native OS, architecture,
compiler/version, profile and configuration cache prefix, or the separate
instrumented Sonar prefix. Compiler entry hashes remain authoritative; no flags,
header checks, quality scope or instrumentation are relaxed.

`compiler-checkpoints.yml` runs only on reviewed main code, on a completed
canonical main push, a schedule, or manual dispatch. Its four platform/profile
exports run serially, use existing main Actions snapshots, and never compile.
Missing snapshots skip publication. Triggering workflow conclusions do not grant
trust: repository, branch and push event must match. A successful checkpoint
means compilation completed, not that tests or quality gates passed.

The exporter checks the public package's source checkpoint before packing and
skips the same or newer run/attempt. Publication replaces one namespace tag only
after its blob upload completes. Only an explicit missing manifest permits a
first publication; outages and incompatible manifests fail without overwriting.
A single workflow concurrency group prevents competing latest-tag writers. New
compiler/toolchain namespaces get distinct tags; old namespace/tag cleanup is
not automated in this first rollout. GHCR version storage must be monitored;
Actions duplicate retention remains independent.

PRs retain their existing jobs and read-only package permissions. They first
restore Actions storage, then try the anonymous public fallback on a miss for
native Debug or Sonar. The client uses an empty credential store and a 90-second
network budget. A fixed digest selects the blob, capped at 3 GiB compressed and
4 GiB expanded. A bounded manifest requires the independently selected namespace,
source key, regular portable paths, sizes and per-file SHA-256. Every file is
verified privately before installing any entry. Configuration, locks and counters
are excluded; local entries/settings are preserved. Restore runs before compiler
activity, with the Windows cache server stopped. A failed fetch retains normal
compilation. The CLI chooses only the two fixed workspace cache paths, rather
than accepting an arbitrary destination.

This is independent storage against Actions quota eviction, not a latency
measurement or a replacement for dependency packages. Hosted publication and
anonymous cross-platform restores still require qualification before claiming
the 10–15 minute target. Large transfers and warm linking/analysis remain part
of each job's measured duration.

### Auxiliary native compiler working set

Native qualification switches to a separate `native-variants-v1-Mixed` compiler
store after the primary Debug checkpoint. Its fixed path is
`.ci-cache/compiler-variants`, independently bounded at 2G. Release audio, Metal,
navigation, GNS and XR use this store. Their real flags and compiler entry hashes
remain authoritative; Mixed describes the collection, not binary compatibility
between build configurations. The main checkpoint before tests preserves entries
from successful auxiliary compilation even if a later test fails.

The existing serial native exporter publishes both primary and auxiliary
namespaces in one job per platform. PRs restore each store only at its owning
phase, without uploading. The temporary runner may hold up to 4G of native
compiler entries across these two stores; the Debug working set cannot be
evicted by later Release/provider compilation. This does not prove all variants
fit the auxiliary budget. Per-phase hit rates and pressure still require hosted
measurement. Actions storage remains a best-effort staging layer: it may evict a
new checkpoint before the periodic exporter reads it. GHCR preserves previously
published checkpoints independently; a missing publication builds normally.

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
