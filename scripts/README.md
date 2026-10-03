# Scripts

Developer and reusable tooling scripts belong here. GitHub workflow helpers belong
under `.github/scripts/`; shared action setup belongs under `.github/actions/`.
Legacy scripts are under `deprecated/scripts/` until migrated.

## Local editor runner

Copy `.env.example` to `.env.local`, adjust the local OpenTelemetry endpoint if
needed, then use the narrow developer runner:

```bash
python3 scripts/dev.py run editor
python3 scripts/dev.py run editor -- --project /path/to/project
```

`HORO_DEV_OTEL_EXPORT` is interpreted only by `dev.py`. When enabled, the runner
configures the optional adapter, performs a TCP reachability check, and launches
the editor with explicit export approval. It does not start or manage Docker,
Grafana, or an OpenTelemetry Collector.

When local Grafana is reachable, the runner also synchronizes the repository
HoroEditor dashboard before launch. This import can be disabled with
`HORO_DEV_GRAFANA_AUTO_IMPORT=OFF`; Grafana availability never controls whether
the editor is allowed to start.

## Local quality preflight

Run Sonar IDE, CLI secrets and Codacy against one worktree change selection:

```bash
python3 scripts/quality_preflight.py check --base origin/main
python3 scripts/quality_preflight.py check --dirty
python3 scripts/quality_preflight.py doctor
python3 scripts/quality_preflight.py stop
```

Use `--worktree` for another checkout and the actual PR target for stacked
branches. Exit 1 means findings; exit 2 means incomplete or stale analysis.
Reports and managed profiles live outside the source tree. See the
[local-analysis guide](../docs/guides/sonarqube-mcp-local-analysis.md) for setup,
compiler context, advisory metrics, measured timings and CI limitations.

## Sonar line coverage collection

The Sonar workflow calls `.github/scripts/collect_sonar_coverage.sh`, which uses
pinned fastcov 1.17 with GCC/gcov 13.3.0. It collects all existing `build/sonar` inputs,
including unexecuted units with `--process-gcno`; test and source filters are
unchanged. Collection failure fails the job. `fastcov_sonar_coverage.py` merges
counters across tests and template instantiations and emits generic Sonar line coverage. It excludes only
zero-hit comments, standalone braces and `else`, matching the former gcovr
noncode heuristic. Real uncovered code remains in the report. Paths must resolve
inside the repository, counters must be nonnegative integers, and source line
numbers must exist. The seven-day artifact retains the XML and raw JSON reports.

On PR #3260, run 37140325812 (Ubuntu 24.04, GCC 13.3.0), gcovr 8.6 took
309 seconds and fastcov collection plus conversion took 36 seconds. Installation
was excluded; gcovr ran first. This is one ordered trial, rather than an isolated
benchmark. The normalized fastcov report retains every gcovr line, adds 218
lines and changes one covered flag. A local forced-text-parser template fixture
reproduces gcovr dropping executed lambda lines, consistent with GCC 13 using
gcovr's text parser; it does not prove the exact engine discrepancy from raw
GCC 13 counters. Collector adoption changes these line accounting details.
Branch and function coverage parity is not claimed.

`compare_sonar_coverage.py` remains an offline audit utility for the original
experiment artifacts. It compares source/line identities and any-instance-covered
flags, with complete differences in JSON and a printed Markdown summary.

## CI runner grouping and closed PR cleanup

See [the CI layout](../.github/README.md) for workflow stages and build profiles.
`ci.yml` uses four runner jobs: tooling and one native job per platform. Native
Debug includes audio Debug; the same runner builds the smaller audio Release
group. Windows ABI, MCP, save, input and foundation checks share one headless
Debug build. Test groups are owned by CMake, and CTest retains case names in
JUnit artifacts. Hosted timings remain the performance acceptance evidence.

`cancel-closed-pr.yml` runs on `pull_request_target: closed`. Its trusted base
workflow owns the inline script; no repository source is checked out, and only
`actions: write` is granted. It requests cancellation of active `pull_request` runs linked
to that PR, with exact repository/branch/SHA matching as a fallback for missing
API links. Completed runs and main push runs are preserved. It handles both merge
and manual closure, even after branch deletion.
The helper becomes active after landing on the base branch and may itself queue
while runner capacity is exhausted; it does not instantly release slots at merge.
