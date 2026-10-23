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

The latest inspected main run,
[39798037208](https://github.com/horocore/horo-engine/actions/runs/39798037208),
at `770461dfb9c40971d756b940078ed2e9e63dd965` failed C++ coverage tests and
skipped analysis entirely. It therefore did not refresh the analysis baseline
for that revision. Its package setup took only 17 seconds, further indicating
network variability rather than intrinsically expensive package installation.
Restore a completed main analysis after the existing Linux/UI fixes before
attributing all PR cache misses to the cache storage model.

## First change: bound package acquisition

`setup-build-tools` delegates Linux installation to
`.github/scripts/install_linux_build_dependencies.sh`. The package list is
unchanged. The script uses invocation-local Ubuntu sources and package indexes,
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

## Next experiments, in order

1. Measure this package change on GitHub at an exact recorded revision. Record
   runner image, phase times, failures and package versions. If mirror variance
   remains material, evaluate a pinned, preinstalled CI image with a maintained
   update process. Compare image startup/pull cost against package setup cost.
2. Make CTest concurrency safe before raising jobs. Catch test cases run as
   separate processes. `ShaderCompilerToolchainFixture` currently creates a
   process-local counter-based name under the shared temporary directory; cases
   can collide. Audit other shared files, ports, working directories, GPU contexts
   and coverage output. Use exclusive fixture directories or narrowly scoped
   resource locks, retaining existing serial tests and fixture dependencies.
   Compare jobs 1, 2 and 4 with the identical test manifest and coverage counters;
   investigate flakiness or counter loss rather than relaxing checks.
3. Measure CFamily cache reuse separately from compiler/scanner-download caches.
   The workflow already uses server analysis caching and lets active main scans
   finish. Check the freshness of that main baseline and cache-invalidating build
   or quality-profile changes. Evaluate PR-scoped filesystem analysis caching
   only with explicit cache ownership, quota, trust and invalidation rules;
   preserve the analyzer's own validity checks and the required gate.
4. Profile configuration, linking and native-platform cache eviction. The earlier
   macOS run had low compiler-cache reuse and a nearly full 2 GiB cache. Larger
   runners or caches are candidates to benchmark, not assumed improvements.

The current configure/build/coverage path alone exceeds the overall target even
if package setup becomes free. A successful package fix is therefore only the
first milestone. Do not report the goal achieved until hosted measurements show
the complete required workflow within budget, with all checks still represented.

## Delivery validation

Run the installer regressions together with CI grouping/cache and coverage
contracts. Run the repository quality preflight on the final worktree. On the
macOS validation host, `scripts/analyze.py check` stops with
`Managed quality processes require Linux with pidfd support or Windows` before
producing a report. Sonar IDE, CLI secrets and Codacy are consequently unvalidated;
this is not a clean preflight or a hosted quality-gate result.
