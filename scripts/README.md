# Scripts

New repository scripts belong here. Legacy scripts are under `deprecated/scripts/` until migrated.

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

## Sonar coverage collector experiment

The Sonar workflow benchmarks pinned fastcov 1.17 on pull requests after the
existing gcovr baseline is generated. Both collectors consume the same
`build/sonar` coverage data; fastcov includes unexecuted units with
`--process-gcno`. Its timed region includes collection and generic Sonar XML
conversion, but excludes package installation. No test or input directory is
removed from the collection scope.

`compare_sonar_coverage.py` compares normalized source/line identities and
covered flags, including uncovered lines. It writes complete differences to
JSON and timings to the workflow summary. The comparison establishes line
coverage only, matching the existing `--sonarqube-metric line`; function, branch
and exclusion semantics can differ between collectors. Differences are
diagnostic during this trial. Invalid reports or collector failures mark the
experiment incomplete, while Sonar still consumes `coverage.xml` from gcovr.

The seven-day workflow artifact contains both XML reports, fastcov JSON,
comparison JSON and tool versions. A collector change requires reviewing both
the timings and line differences.

## CI runner grouping and closed PR cleanup

`.github/workflows/ci.yml` shares one format prerequisite, runs both audio build
modes on each platform runner, and groups compatible Windows headless contracts.
Both audio modes and all four Windows test groups must pass their aggregate gate.
Separate JUnit files preserve per-suite evidence even if another suite fails.
This reduces the CI plus former Prefab workflow from 16 to 9 runner jobs without
removing targets or test filters. Hosted timings remain the acceptance evidence.

`cancel-closed-pr.yml` runs on `pull_request_target: closed` from the trusted base
branch, checking out only `github.sha` from that trusted base branch. It requests cancellation of active
`pull_request` runs linked to that PR, with exact repository/branch/SHA matching
as a fallback for missing API links. Completed runs and main push runs are
preserved. It handles both merge and manual closure, even after branch deletion.
The helper becomes active after landing on the base branch and may itself queue
while runner capacity is exhausted; it does not instantly release slots at merge.
