# Scripts

Developer and reusable tooling scripts belong here. GitHub workflow helpers belong
under `.github/scripts/`; shared action setup belongs under `.github/actions/`.
Legacy scripts are under `deprecated/scripts/` until migrated.

## Pull request annotations

`pr_annotations.py` checks a PR against its development issue. It validates the
milestone, a GitHub closing reference, labels, projects, and assignee. It finds
the issue from the PR's closing references/body/title, or searches for the Jira
key in the head branch. Use `--issue` when that lookup is ambiguous. If the
issue has no assignee, the PR author is the default assignee. Use `--project` to
also require a project on the PR. Set `GH_TOKEN` or `GITHUB_TOKEN`, or authenticate
the `gh` CLI for `github.com`; project edits need the `project` token scope.
API requests use fixed-host HTTPS and JSON payloads. The CLI is used only to
read credentials with fixed arguments, never to execute PR-supplied values.
Repository names must be ASCII `OWNER/REPO` identifiers. Requests and credential
lookup have a 30-second timeout; responses and pagination are bounded.

```bash
python3 scripts/pr_annotations.py 3067              # read-only check
python3 scripts/pr_annotations.py 3079 --project 1 --fix
python3 scripts/pr_annotations.py --all --project 1 --state all
python3 scripts/pr_annotations.py --all --project 1 --state all --fix
```

The `--all` run paginates through every PR, including historical merged PRs,
using up to eight parallel workers by default (`--workers N` adjusts this).
Use `--state open`, `closed`, or `merged` to narrow it, or `--limit N` to cap
the run. A missing issue milestone is reported but never cleared from a PR.
Existing PR-only labels and projects are preserved. `--fix` creates a manual
Development link through GitHub's GraphQL API, which also works for stacked PRs
targeting a non-default branch. Any unresolved issue link or API failure exits
nonzero.

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
