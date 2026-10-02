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
