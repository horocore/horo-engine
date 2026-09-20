---
applyTo: "**/*"
---

# SonarQube local-analysis policy

The SonarQube CLI (`sonar`) is the supported full local-analysis workflow for
this repository. For a bounded local C/C++ file diagnosis when Agentic/Vortex
is unavailable, `scripts/sonar_ide_analysis.py` may use the trusted running
VS Code SonarQube for IDE bridge. The bridge result is local IDE feedback, not
a SonarCloud, PR, or full-quality-gate result.

## Required workflow

- Run from the worktree being inspected:
  `sonar analyze --project <project-key> --format json --depth STANDARD`.
- With no selector, the CLI analyzes staged, unstaged, and untracked changes.
  Use `--staged`, `--base <ref>`, or repeated `--file <path>` only for an
  intentional narrower scope.
- Resolve the exact project key with `sonar list projects --query <name>`;
  never invent a key.
- Prefer `SONARQUBE_CLI_TOKEN`, `SONARQUBE_CLI_ORG`, and
  `SONARQUBE_CLI_SERVER` environment variables for ephemeral runs. Do not put
  credentials in the repository or command output.
- Report `secrets` and `agentic` results separately. A clean secrets result is
  not a clean quality result when `agentic` contains skipped files, failures, or
  `globalError`.
- For C/C++, verify that the intended files appear in `agentic.files` and that a
  long-lived branch has a successful CI analysis supplying Vortex build context.
- For an explicit local IDE request, create and configure a worktree compilation
  database, then run `python3 scripts/sonar_ide_analysis.py --port <64120-64130>
  <files...>`. Report its submitted and skipped files separately from CLI and
  server findings. The script never uploads source or accepts a token.

## Entitlement and failure handling

`403 Forbidden` or `Vortex analysis is not available on this connection` means
the account or project lacks the required Agentic/Vortex entitlement. Report it
as a failed CLI quality validation. For an explicitly requested file-level
local C/C++ diagnosis, use the IDE bridge result only as supplemental local
feedback; do not represent it as a Vortex or server-quality result, and do not
silently substitute it for the CI gate or `sonar-scanner`.

Do not repeatedly retry an unchanged authorization or entitlement failure. Local
secrets scanning may still be reported, but it must remain clearly separate from
Agentic/Vortex quality findings.

## Prohibited substitutions

- Do not call `analyze_file_list`, `toggle_automatic_analysis`, or
  `analyze_code_snippet` through an MCP server for repository validation; use
  the repository-owned IDE bridge client when file-level IDE analysis is needed.
- Do not run `sonar-scanner` for local uncommitted-change feedback; it is the
  full-project CI scanner.
- Do not claim success from an empty issue list unless the intended files were
  analyzed and no skips, failures, or global errors were returned.
