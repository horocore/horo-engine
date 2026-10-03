# Scripts

New repository scripts belong here. Legacy scripts are under `deprecated/scripts/` until migrated.

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
