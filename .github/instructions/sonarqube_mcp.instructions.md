---
applyTo: "**/*"
---

# SonarQube local-analysis policy

Use `python3 scripts/quality_preflight.py check --base <PR-target-ref>` before
preparing a PR. It runs the supported managed Sonar IDE C/C++ analysis, local CLI
secrets scanning and Codacy preflight with one worktree change selection. Use
`--dirty` for only current edits or `--files <paths...>` for a narrow diagnosis.
The helper supports `--worktree`, keeps generated state outside the source tree,
and validates listener ownership, compilation context and C++ sensor execution.

## Result boundaries

- Sonar IDE is local feedback, not a SonarCloud PR or full quality gate result.
- CLI secrets, IDE and Codacy results are separate. A clean secrets scan
  does not establish code-quality success.
- Exit `0` means the requested local checks completed without findings (or the
  change selection was empty). Exit `1` means findings; `2` means incomplete.
- Read submitted/skipped files, errors, freshness and per-file analysis evidence.
  Missing C++ sensor execution, partial tools or a stale report cannot be clean.
- Preserve all findings; highlight changed lines without claiming contextual
  findings existed before the change. Complexity and CPD metrics are advisory.

## CLI secrets

The CLI is used only for `sonar analyze secrets <selected paths>`. Vortex
analysis and entitlement probes are outside this workflow; do not run the
general `sonar analyze` command. `doctor` checks local prerequisites and optionally
compares Codacy cloud configuration without changing checked-in settings.

Prefer environment credentials for ephemeral CLI runs. Do not place tokens in
repository files, process arguments, reports or output. Managed IDE profiles
copy only connection metadata and encrypted Sonar SecretStorage rows; plaintext
tokens are not copied. Authentication remains the OS keychain's responsibility.

## Prohibited substitutions

Do not use unverified explicit bridge ports, `analyze_file_list`,
`toggle_automatic_analysis`, `analyze_code_snippet`, or `sonar-scanner` for local
validation. The scanner remains the full-project CI path. Do not alter rules,
cloud configuration or issue status, or upload results as part of preflight.

See `docs/guides/sonarqube-mcp-local-analysis.md` for setup and troubleshooting.
