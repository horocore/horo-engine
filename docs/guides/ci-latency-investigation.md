# CI latency investigation

The target is 10–15 minutes of hosted feedback with the existing required tests,
coverage scope, static analysis and quality gates preserved. This is a target,
not a measured result of the changes below.

## Recorded baseline

On 2026-10-23, [Sonar run 39955114498](https://github.com/horocore/horo-engine/actions/runs/39955114498/job/120418905836)
at `715a94f9960f1ec1db1b9f8cb467d723f40aa213` recorded:

| Stage | Duration |
| --- | --- |
| Build tools | 16m 24s |
| Configure instrumented build | 3m 36s |
| Build instrumented targets | 8m 15s |
| C++ coverage tests | 15m 7s |
| Python coverage tests | 20s |
| Collect coverage | 2m 31s |

The job was still analyzing when inspected; these are completed step durations,
not its final total. APT downloaded 29.1 MB in 16m 4s at 30.2 kB/s from the Azure
Ubuntu mirror. Compiler caching was already effective: 5,589 of 5,660 cacheable
calls hit (98.75%). Restoring a cache alone is not evidence of useful cache hits.

An earlier [Sonar run 39934076034](https://github.com/horocore/horo-engine/actions/runs/39934076034)
at `ae47073aa354834bc8580539ab481c40f17f2fa3` spent about 32 minutes in the
CFamily sensor, with 1,023 of 1,956 analysis-cache hits. Its gate wait was about
eight seconds. It is a different revision, useful for diagnosis but not an A/B
performance comparison.

An earlier inspected main run,
[39798037208](https://github.com/horocore/horo-engine/actions/runs/39798037208),
at `770461dfb9c40971d756b940078ed2e9e63dd965` failed C++ coverage tests and
skipped analysis entirely. It therefore did not refresh the analysis baseline
for that revision. Its package setup took only 17 seconds, further indicating
network variability rather than intrinsically expensive package installation.
Restore a completed main analysis after the existing Linux/UI fixes before
attributing all PR cache misses to the cache storage model.

## First change: bound package acquisition

`setup-build-tools` delegates Linux installation to
`.github/scripts/install_linux_build_dependencies.sh`. The original package list is
preserved; the subsequent Linux linker experiment additionally installs `lld`. The script uses invocation-local Ubuntu sources and package indexes,
retains Ubuntu signature/hash validation, and tries the official archive before
the Azure mirror. Each mirror has a 45-second index budget and a 90-second
archive-download budget, with a 10-second termination grace per timed operation.
These are failure bounds, not a promised successful installation time.

Installation starts only after successful acquisition, uses `--no-download`, and
is not interrupted by a download timeout. A failed installation is returned to
the workflow without retrying a partially completed dpkg transaction. Both
mirrors failing also fails the workflow. The step summary records phase times
and exit statuses. No system source files are rewritten.

Local validation uses command-fixture regression tests, ShellCheck, and a real
Ubuntu 24.04 amd64 Docker run (one CPU, 1 GiB memory, emulated on macOS). The real
run installed all dependencies successfully: indexes took five seconds and the
offline install took 45 seconds. Container bootstrap and emulation differ from
GitHub runners; this is functional validation, not a hosted speedup claim.

## First hosted package measurement

[Sonar run 39966026495](https://github.com/horocore/horo-engine/actions/runs/39966026495/job/120455995804)
for PR head `ab73d4c26d1d5f1dfd774cec5190e84bf30acc79` completed package setup in
23 seconds. Configure took 3m 31s and compilation took 9m. This is one observation,
not a repeatability claim or controlled A/B test: main advanced with Linux/UI
fixes before the PR merge ref was built. Full workflow completion was pending.

## Preserve the primary compiler working set

Native main jobs now publish the Debug compiler cache immediately after Debug
compilation, before Metal, Release and optional provider builds compete for its
2 GB capacity. The later checkpoint saves complete dependency sources only.
There is still one compiler writer per platform and main run, and PRs remain
read-only cache consumers. This change does not enlarge the repository quota.
A prior macOS run had only 540/8,686 hits, with a nearly full cache and 1,425
cleanups. Saving the primary working set before secondary builds addresses a
specific eviction risk; its improvement requires a new main checkpoint and
subsequent PR measurements, not merely a successful restore message.

## Parallel tests inside the existing runner

`HORO_CI_ISOLATE_TEST_EXECUTABLES` is opt-in and enabled by the native Linux/macOS
and inherited Sonar presets. Every discovered executable gets a canonical private
working directory, temporary directory and home. A per-executable CTest resource
lock serializes its cases, including fixtures with process-local counters.
Independent executables may run concurrently. Existing resource locks, labels, fixture properties and timeouts remain
effective. Environment settings are retained except for the five explicitly
isolated temporary/home variables. GPU, display
and real-time safety suites remain serial; direct integration commands remain
serial as well. Windows stays at its existing serial test setting for this stage.

Linux, macOS and Sonar use two CTest workers. No test selector or coverage
exclusion is broadened. Regression probes execute CTest with paths containing
spaces and non-ASCII characters and check actual overlap, same-suite exclusion,
existing locks, environment and timeout preservation. These probes do not prove
that every engine fixture is isolated: hosted engine tests and coverage counters
must still validate the change. GCC merges concurrent process coverage on file
systems supporting file locking; compare collected coverage and investigate any
counter corruption rather than accepting reduced coverage.

Release audio compilation now uses two workers, matching the bounded auxiliary
builds. Its real-time tests continue to use one worker. The effect on memory and
runtime must be checked on all three hosted platforms.

## Linux linker qualification

The Linux Debug preset (and inherited Sonar preset) selects CMake's `LLD` linker
and caps each executable/shared/module link to two threads. GCC, coverage flags,
optimization modes and test selectors remain unchanged. Other platform linkers
and the headless auxiliary presets retain their existing defaults. The CI preset
file now requires CMake 3.29, which introduced `CMAKE_LINKER_TYPE`; the project's
ordinary non-preset build requirement is unchanged. The inspected hosted images
provide newer CMake versions.

The earlier Sonar build performed 787 link operations and still took nine minutes
with 98.75% compiler-cache hits. This motivates measuring LLD, but does not prove
that linking accounts for all nine minutes. Ubuntu probes compile and execute
coverage-instrumented static/shared-library consumers and dynamically loaded
modules through the real preset's linker settings, verify the LLD marker in the
ELF files, and check concurrent GCC counters with both BFD and LLD. Five probes
passed on Ubuntu 24.04 amd64 (emulated, one CPU/1 GiB). This is compatibility
validation, not an engine performance result; full hosted qualification remains
required before adopting the change.

Dependency cache identities now include `SourceEditorWidgetDependency.cmake` as
well as Horo-prefixed helpers. Sonar restores only exact declaration/default keys:
a broad old-revision fallback is incompatible with disconnected FetchContent's
ability to fetch newly pinned commits. These changes protect pin validity; they
do not solve the cache capacity problem by themselves.

## OSS capacity and concurrent PRs

The design uses standard public-repository runners. As checked on 2026-10-23,
[GitHub's limits](https://docs.github.com/en/actions/reference/limits) list 20
concurrent jobs and 5 macOS jobs for Free, 40/5 for Pro and 60/5 for Team. The
organization's actual plan has not been confirmed; use the conservative 20/5
budget. Free public-runner minutes do not remove concurrency limits. Standard
Linux/Windows runners provide 4 CPUs and 16 GB RAM; ARM macOS provides 3 CPUs and
7 GB RAM ([runner specifications](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)).

No new PR jobs or shards are introduced. Native matrix width is two, shader width
one and Android package width one. With Sonar and D3D12, the post-tooling fan-out is at most six concurrent
jobs rather than nine. The independent Android storage job can additionally
occupy one slot if it has not completed yet.
This is a per-run bound, not an organization-wide fairness scheduler: 5–10 PRs
still queue under the shared limit, and manual UI/release work also consumes
capacity. All platform and package variants remain present. Existing per-ref
concurrency supersedes obsolete PR commits while preserving active main work.
Do not replace it with a single cross-PR cancellation group.

Measure queue delay separately from execution time. Ten macOS jobs need at least
two waves under a five-slot limit, even if every job executes within 15 minutes.
Paid larger runners, larger cache quotas and repository-wide cache deletion are
not part of this change. PRs do not publish duplicate dependency/compiler caches.

## Observed cache pressure

On 2026-10-23, the Actions cache listing contained five entries totaling
10,156,320,046 bytes: Linux/macOS compiler snapshots, two Windows compiler
snapshots, and one macOS dependency snapshot. The two Windows snapshots alone
used 4,297,053,643 bytes. Previously observed Sonar compiler, dependency and
scanner entries were no longer listed. This is consistent with capacity-driven
eviction; the listing alone does not identify who removed an entry or prove the
configured repository quota. The cache usage endpoint's count was inconsistent
with the listing, so use enumerated entries rather than that count.

Budget against the free 10 GB allocation until a different quota is verified.
Per-run immutable main compiler keys can retain duplicate platform snapshots and
crowd out other platforms. Full fetched Git histories also consume substantial
space: the local source clones held about 1 GB of Git metadata. A sustainable
policy must bound retained main snapshots and reduce dependency-cache payloads;
raising each cache limit or adding PR writers would amplify the problem. No
existing cache was manually deleted during this investigation.

`Compiler cache retention` performs one short, serialized maintenance job after
main CI/Sonar runs and twice hourly, independently of PR fan-out. It retains the
newest nonempty snapshot for each exact compiler/profile/archive-version prefix
and retires only older main snapshots in that group. Before each deletion it
rechecks that the replacement still exists. Other owners, dependency sources,
scanner downloads and PR refs are excluded. Only this maintenance workflow has
`actions: write`; it checks out the default branch and never executes the
triggering run's code or artifacts. Manual dispatch defaults to dry-run mode.
The workflow becomes operational only after delivery to the default branch.

This bounds duplicate snapshots, not total repository usage: obsolete compiler
versions and large source payloads still require a separately measured policy.
Periodic maintenance also cannot prevent transient quota eviction during an
upload. Do not count this change as proof that cold starts have been eliminated.

Main Linux job `120454374716` in run `39965417298` restored the 1,966,664,537-byte
compiler snapshot from run `39782945403`, but its Debug build ran from
14:00:37 to 15:01:37 UTC on 2026-10-23 before runner shutdown. The log records
5,695 object-compilation completions through 14:59:35 and 519 link completions
before termination at 6,270/6,525 Ninja operations. These are task completions,
not measured cache misses; the shutdown prevented final cache statistics.
The runner explicitly reported a shutdown signal and exit 143, with no compiler
diagnostic. This is incomplete infrastructure-interrupted evidence, not a passed
build or proof that a restored compiler archive is useful. Compilation spans
almost the entire elapsed build interval, so faster linking alone cannot meet
the target on this workload. Preserve this distinction when evaluating LLD.

Main Windows job `120454374858` completed successfully in 47m03s. Debug build
took 18m14s, Release audio build 10m33s, additional Release allocation build
2m26s and Debug tests 6m59s. Aggregate sccache statistics across these builds
reported 6,528 hits/85 misses (98.71%), but also 533 non-cacheable calls:
525 `/Fp`, four `/Yc` and four multiple-input invocations. The reported hit rate
excludes those uncached calls and must not be interpreted as 98.71% of all work.

The shared CI preset now sets `CMAKE_DISABLE_PRECOMPILE_HEADERS=ON`, matching
the existing local cache configuration policy in `scripts/dev.py`. Jolt/SDL PCH
generation had bypassed sccache on MSVC. A CMake execution probe verifies both
PCH-enabled control and the actual CI policy compile and run the same program,
and only the control produces PCH commands. No test or assertion is removed.
The first run changes dependency object keys and may be slower while warming;
Windows hosted timings and remaining non-cacheable reasons must be remeasured.
This does not make MSVC preprocessing free: sccache 0.17's documented direct
mode supports GCC/Clang, so its global `yes` setting is not MSVC direct-hit proof.

## Remaining work and acceptance

The current configure/build/test path still exceeds the target. Sonar server-side
analysis cache remains enabled, and an active main scan can finish to refresh it.
Do not add multi-gigabyte per-PR analysis caches while native caches already
compete for repository storage. Measure CFamily cache reuse separately from
compiler hits and scanner downloads after the current main scan completes.

Record exact head and merge/base revisions, runner image, queue delay, completed
step durations, compiler statistics, CFamily cache hits, test count and coverage.
Compare cold and warm runs and a representative 5–10 PR burst. The goal remains
unmet until Sonar and each required platform job complete in at most 15 minutes
with the same quality checks. Changing `timeout-minutes` to 15 is not acceleration.
Further changes should address measured configure, linking, source-cache size and
analysis costs rather than adding jobs to hide the critical path. The earlier Sonar log also
reported taint analysis stopping early due to low memory; retain that warning as
an analysis-completeness concern and measure scanner heap/native memory before
tuning memory budgets. Do not count a green gate alone as proof of complete
analysis when that warning is present.

## Delivery validation

Run the installer regressions together with CI grouping/cache and coverage
contracts. Run the repository quality preflight on the final worktree. On the
macOS validation host, `scripts/analyze.py check` stops with
`Managed quality processes require Linux with pidfd support or Windows` before
producing a report. Sonar IDE, CLI secrets and Codacy are consequently unvalidated;
this is not a clean preflight or a hosted quality-gate result.
