# Worktree-aware local quality preflight

## Purpose and result boundaries

Run one command before preparing a PR to inspect changed files with SonarQube
for IDE, the Sonar CLI secrets scanner, and the current Codacy Analysis CLI.
The IDE path provides local C/C++ diagnostics. Vortex is outside this workflow;
neither checks nor doctor perform Vortex analysis or entitlement probes.
Coverage, C/C++ taint analysis, Sonar duplication, and the full hosted quality
gates remain CI responsibilities. Codacy CPD is a local duplication estimate.

This repository currently supports managed sessions on Linux with `/proc`,
`pidfd_open`, a display server, VS Code and SonarQube for IDE. macOS/Windows
session ownership is not implemented; the command fails explicitly there.

## Commands

```sh
# Branch changes plus staged, unstaged, and untracked edits.
rtk proxy python3 scripts/quality_preflight.py check --base origin/main

# Only current uncommitted changes.
rtk proxy python3 scripts/quality_preflight.py check --dirty

# Inspect another worktree using the script from this checkout.
rtk proxy python3 scripts/quality_preflight.py check --worktree /path/to/worktree --base origin/main

# Reuse that worktree's CMake/Ninja build and focus on explicit files.
rtk proxy python3 scripts/quality_preflight.py check --worktree /path/to/worktree \
  --compile-commands build/skeleton/compile_commands.json --files src/example.cpp

# Diagnose prerequisites and Codacy cloud configuration drift.
rtk proxy python3 scripts/quality_preflight.py doctor
rtk proxy python3 scripts/quality_preflight.py doctor --offline

# Close only this helper's idle Sonar sessions in the common Git repository.
rtk proxy python3 scripts/quality_preflight.py stop
```

Use the actual PR target as `--base`, including for stacked branches. The default
is `origin/main`; the command never fetches and fails if the ref is unavailable.
`--dirty`, `--base` and `--files` are mutually exclusive. `--only sonar` and
`--only codacy` are explicitly partial checks, recorded as such in the report.
The compatibility entry point `sonar_ide_analysis.py` delegates to the managed
Sonar-only scope. Explicit bridge ports are rejected.

The terminal prints findings and a JSON report path. `--format json` prints the
whole report. Exit statuses are:

| Exit | Meaning |
| --- | --- |
| 0 | Requested local checks completed without findings, or no changes selected |
| 1 | At least one requested analyzer reported findings |
| 2 | Missing prerequisites, incomplete analysis, cancellation, or stale inputs |

Inspect errors, skipped paths, provider status and C++ evidence even when the
issue list is empty. Errors take precedence over findings in the exit code.
Findings on changed lines appear first; all other selected-file findings remain
available. Changed-line overlap does not establish when an issue was introduced.
Raw complexity and duplication estimates are advisory, without new thresholds.

## Setup and owned sessions

Install the Sonar CLI (`sonar`), current `codacy-analysis`, CMake, Ninja and the
repository's native dependencies. Legacy `codacy-cli` is not this workflow.
Install SonarQube for IDE in VS Code and configure/authenticate its connected
mode for the project. `--code-user-data` and `--extensions-directory` select
non-default VS Code installations.

The helper creates a private user-data directory, copied released Sonar extension and Java home for each
worktree under `$XDG_CACHE_HOME/horo-quality` (or `~/.cache/horo-quality`). Only
Sonar connection metadata and encrypted Sonar SecretStorage records are copied
from the source profile, read-only. Plaintext tokens are never copied. VS Code
uses the OS keychain to decrypt the connection; if it cannot, authenticate the
managed window once and retry. The released extension is activated with Code's `--extensionDevelopmentPath`
in the private host; no extension source is modified. Java's `user.home` is also
private, so profiles do not share the mutable `~/.sonarlint` database.
The helper sets `security.workspace.trust.enabled` to `false` before launching
its private profile, so new worktrees do not require a manual workspace trust
confirmation. This applies only to the managed profile. The user's own trust
settings and trusted-folder records are neither changed nor copied. Sessions
created before this setting was corrected are restarted automatically.
The source settings file currently must be JSON,
without JSONC comments. Inline-token-only connections must be migrated to VS
Code SecretStorage first.

There are at most two Sonar sessions per common Git repository. Each active
session has a lease; further requests wait. Idle sessions are reused, or the
oldest idle one is evicted for another worktree. The helper validates a random
launch marker, process executable, PID/start time and the listening socket's
owner. A port remembered from another session is never accepted. `stop` leaves
busy sessions and unrelated user windows alone. Windows can be visible; this
is automated desktop analysis rather than a headless Sonar service.

A worktree-wide lock also prevents concurrent mutation of that worktree's cache.
Independent worktrees can analyze concurrently. Cancellation releases leases;
timed-out command groups are terminated. Interrupted bridge requests run in an
owned worker and retire their managed server, preventing queued work from
leaking into the next lease. Generated state is never committed.

## Compiler context and analysis evidence

Without `--compile-commands`, the helper first checks the canonical
`build/skeleton/compile_commands.json` for this worktree and selected sources.
If that verified build is unavailable, it configures an external Debug/Ninja build
with tests and the optional Vulkan, ImGui UI tests, OpenTelemetry and GNS targets
enabled to cover the Linux CI composition. This first configure can download
dependencies and take materially longer than subsequent checks.

An existing database must be accompanied by its CMake cache naming the exact
worktree source root. CMake refreshes configuration, then only the selected
translation units' object targets are built with Ninja. Make builds use their
owning targets so CMake dependencies and generated headers are prepared.
Staged public-header copies are mapped back only when their contents match the
source header.
Header-only selections require a literal include chain to a compiled translation
unit. Macro-generated include chains without provable context are incomplete.
Platform-guarded files absent from this build are incomplete, not clean.

Requests require fresh Sonar logs naming all submitted files, active C/C++ rules,
a positive count of analyzed compilation units and completed analysis. Sensor
errors or missing connected-mode synchronization reject the result. Initial
indexing retries are distinguished from sensor failures. The IDE's JGit can fail
to discover linked-worktree metadata; this is retained as a branch-matching
warning when C++ execution succeeds. Cloud branch parity remains unverified. Log evidence
is version dependent: an extension update changing these diagnostics may require
an adapter update rather than relaxing the check.

## Codacy configuration and metrics

The checked-in `.codacy/codacy.config.json` selects tools and rules. `check` never
resynchronizes it. `doctor` reads remote configuration into a disposable cache
and reports tool/pattern/exclusion differences, without changing cloud settings
or importing them into the repository. Missing authentication is reported.

The CLI runs against a copied, Git-indexed input mirror, because its adapters
write `.codacy/generated`. Mirror Git operations affect only the disposable cache;
no source worktree index, branch or history is changed. Local configuration files
and relative paths are preserved. Every tool must report successful execution;
CLI exit zero with missing tools, zero routed files or errors is incomplete.
Unsupported selected file types are reported explicitly.

Raw complexity uses the same Lizard executable/version as the Codacy pass:

```sh
rtk proxy python3 scripts/codacy_lizard_complexity.py --base origin/main
```

CPD uses the Codacy-managed PMD distribution and Java runtime, scans the complete
active first-party C/C++ corpus, and retains clone groups touching selected files.
The default C/C++ threshold is 50 tokens, with literals and annotations ignored
and lexical-error skipping requested. Any reported lexical error still makes
our result incomplete. Each group retains all occurrence paths and line ranges,
including unchanged partners. The result is an estimate, not a hosted percentage;
cloud versions and settings may differ. Version identities are recorded.

## CLI secrets

`doctor` checks local prerequisites and optionally compares Codacy cloud settings.
`check` uses only `sonar analyze secrets <selected paths>` for CLI analysis.
Do not run general `sonar analyze` or Vortex entitlement probes in this workflow.
Secret scanner text is not stored because
it can contain credential values; a finding directs you to inspect it locally.
Never put tokens in repository files, report output or command arguments.

## Validation and troubleshooting

The report records worktree path, HEAD/base SHAs, configuration hashes, analyzer
versions, scope, submitted/skipped paths and durations. All first-party inputs,
including headers/configuration and new files, are hashed before and after the
check. Changed inputs invalidate the result; rerun the same scope after fixes.

| Symptom | Action |
| --- | --- |
| Missing base | Resolve the correct PR target ref explicitly |
| Another worktree's database | Configure a build in the intended worktree |
| Missing compilation unit/header context | Enable its owning target or inspect the include chain |
| No owned bridge | Inspect the managed profile's Sonar/extension logs and installed extension |
| Missing connected-mode cache | Authenticate/bind the managed profile; retain the incomplete result |
| Missing Codacy/CPD runtime | Prepare that analyzer with the current Codacy CLI in a disposable cache |
| Codacy configuration drift | Review differences; update policy through a separate deliberate change |
| Stale inputs | Stop edits briefly and rerun the same scope |

Local feedback never resolves hosted issues or replaces CI. No scanner upload,
rule suppression, Git hook, commit, push or cloud mutation is performed.

## Linux validation evidence (2026-10-02)

The Python suite completed with 172 passed, 9 skipped and 33 subtests. Regression
coverage includes Git scopes/renames/Unicode, different worktree inputs, two-slot
queuing, timeout/cancellation, PID and listener identity, log rotation, partial
tool execution, stale inputs, unchanged clone partners, header context and
read-only credential copying. Queue and identity edge cases use controlled
fixtures; they are not all desktop end-to-end tests.

Two real checkouts analyzed `apps/horo-package/Pack.cpp` concurrently with
matching relative paths and different contents. Installed tools and native
skeleton builds were already available; these are cold IDE-process starts,
not fresh installation or full native-build benchmarks.

A later experiment used branches `codex/sonar-preflight-a` and
`codex/sonar-preflight-b`, with intentional null dereferences in the same
`ShaderReflection.cpp` path. Its initial approximately 70-second runs required
manual workspace trust confirmation and do not demonstrate unattended startup.
After correcting the private trust setting, both Code profiles were recreated
without trusted-folder records. Parallel full preflights completed in 22.28 s
each: Sonar found zero issues in the corrected A fixture and both `cpp:S2259`
issues in B. Connected-mode binding and C++ rule execution were verified. Builds,
extensions and Java analyzer caches were already prepared. Both overall commands
returned exit 1 because Codacy also reported context findings.

| Checkout | IDE start + preflight | Repeat preflight | Raw file CCN |
| --- | ---: | ---: | ---: |
| Primary checkout (`0afe48158b56`) | 22.18 s | 9.69 s | 65 |
| PR #3134 worktree (`b18ea70d64fb`) | 21.84 s | 9.96 s | 68 |

Sonar and Codacy ran independently within each check. An actual header-only
selection, `include/Horo/Runtime/Render/ShaderReflection.h`, completed all three
providers clean in 16.90 s. The `ShaderReflection.cpp` Codacy pass found two clone
groups, including partners outside the selected file, and file CCN 236. A stale
result was also observed during active source edits and correctly returned exit 2.

At PR #3134's exact head, Codacy's file API reported CCN 68 for `Pack.cpp`, matching
the local total. Its hosted issue list was empty, while the checked-in local
configuration produced `Lizard_ccn-minor` for `Gather` (CCN 22). `doctor` reported
Lizard and other tool/pattern/exclusion drift. The Sonar PR file query and local
IDE both returned no issues; the head's Sonar/Codacy checks were successful.
Linked-worktree branch matching remains an IDE warning. PR #3197's Codacy branch
resource returned 404, so its comparison is unavailable. These samples do not
establish a rule catch rate or end-to-end PR time savings.

The installed versions were Sonar CLI 1.9.0, Codacy Analysis CLI 0.23.1, Sonar IDE
5.10.0, Lizard 1.24.0, and PMD CPD 6.55.0. IDE and secrets checks are independent;
Vortex is excluded from this workflow. Reports live in the private
quality cache and are not repository artifacts.

## References

- [Codacy Analysis CLI](https://docs.codacy.com/codacy-analysis-cli/)
- [Codacy CPD configuration](https://docs.codacy.com/repositories-configure/codacy-configuration-file/)
