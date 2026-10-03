---
name: quality-preflight
description: Run local Sonar IDE and Codacy checks before preparing a Horo Engine pull request, or when checking changes in one or several Git worktrees. Report rule findings, complexity, duplication, and incomplete analysis separately from cloud gates.
metadata:
  short-description: Worktree-safe PR quality checks
---

# Quality preflight

Use the repository's `scripts/quality_preflight.py` from the checkout containing
the implementation. Select the target worktree explicitly when inspecting another
checkout; do not assume this chat's working directory is the PR worktree.

```sh
python3 scripts/quality_preflight.py check --worktree <path> --base <PR-target-ref>
```

The default base is `origin/main`. Use the actual PR target for stacked branches.
The base scope includes committed, staged, unstaged and untracked changes. For
an edit-only check use `--dirty`; for a bounded diagnosis use `--files <paths...>`.
No fetch occurs. A missing base must be resolved explicitly rather than replaced
with a guessed ref. Prefix shell commands with `rtk` per the repository contract.

Use `doctor` to diagnose prerequisites and Codacy cloud
configuration drift. It reads cloud settings into cache without importing them.
Use `doctor --offline` when cloud access is intentionally out of scope. Supply
`--compile-commands <path>` to reuse a CMake/Ninja build belonging to the target
worktree. The helper can discover the canonical skeleton build automatically;
otherwise it configures its own build outside the source tree.

Keep the JSON report path as evidence. Read the full issues, errors, skips and
per-file evidence before reporting success. Exit `2`, `incomplete`, cancelled,
stale, unavailable tools, and missing C++ sensor evidence are incomplete checks.
Exit `1` means findings. Preserve contextual findings; changed-line overlap does
not prove other findings predate this change. Complexity and duplication are
advisory estimates; clone groups retain unchanged partners.

Sonar IDE feedback and CLI secrets results are separate. Vortex is outside this
workflow: do not run Vortex analysis or entitlement probes. Neither a local clean
result nor an advisory metric proves that the hosted Sonar/Codacy gate will pass.
Do not disable rules, refresh checked-in configuration, or upload results to
make a check pass. A secret finding is summarized without exposing its value.

After an authorized fix, rerun the same worktree and scope. This skill grants no
commit, push, branch-change, issue mutation, or upload permission. Managed Sonar
sessions are limited to two per repository; `stop` closes only verified idle
sessions owned by this helper. User VS Code windows remain untouched.

For setup, result limitations, and troubleshooting, read
`docs/guides/sonarqube-mcp-local-analysis.md` in the repository.
