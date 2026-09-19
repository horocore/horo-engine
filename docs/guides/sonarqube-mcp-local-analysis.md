# Local analysis with the SonarQube CLI

## Policy

The SonarQube CLI (`sonar`) is the supported full local-analysis path in this
repository. It is the worktree-friendly developer workflow: each invocation
reads the Git change set of the worktree in which it runs.

For a bounded C/C++ file diagnosis, the repository also provides
`scripts/sonar_ide_analysis.py`. It asks a trusted, running VS Code SonarQube
for IDE extension to analyze explicit local files and returns its findings as
JSON. This is supplemental IDE feedback, not a SonarCloud PR result or a
replacement for the CLI/CI quality gate. `sonar-scanner` remains the
CI/full-project scanner; it is not a substitute for either local workflow.

## Local C/C++ IDE diagnostics

Use this path when a developer needs issue-level feedback for one or more local
C/C++ files, especially when the SonarQube CLI cannot analyze C++ because the
connection lacks Vortex entitlement. It requires no Sonar token and does not
upload source.

1. Generate `build/sonar-local/compile_commands.json` in the target worktree:

   ```sh
   cmake -S . -B build/sonar-local -G Ninja \
     -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
     -DBUILD_TESTING=OFF
   ```

2. Configure that path in the trusted worktree's VS Code settings under
   `sonarlint.pathToCompileCommands`, open the worktree in its own VS Code
   window, and wait for SonarQube for IDE to activate.
3. Identify that window's loopback bridge port (it must be `64120`–`64130`):

   ```sh
   lsof -nP -iTCP -sTCP:LISTEN | rg '6412[0-9]'
   ```

4. Submit explicit paths, which is the preferred narrow mode:

   ```sh
   python3 scripts/sonar_ide_analysis.py --port 64121 \
     src/runtime/renderer/api/ShaderReflection.cpp
   ```

   Use `--base origin/main` for a committed delta or no path arguments for the
   staged, unstaged, and untracked C/C++ changes in the current worktree.

The script checks the bridge status before posting absolute file paths to its
loopback endpoint. It reports `status: "clean"` only after at least one C/C++
path was submitted and the bridge returned an empty `findings` array. Its exit
status is `0` for clean, `1` for findings, and `2` for missing compilation
database, bridge, selection, or response prerequisites. Always report submitted
and skipped paths; a bridge error or skipped request is not a clean result.

`sonar analyze` combines two kinds of feedback:

- secrets detection runs locally and does not require a SonarQube connection;
- Agentic/Vortex analysis checks code-quality and security issues on SonarQube
  Cloud using the project's previously analyzed build context.

Vortex is server-side analysis. It is not an offline C++ compiler or a complete
replacement for the CI quality gate.

## Prerequisites

- SonarQube CLI installed (`sonar --version`).
- A SonarQube Cloud or Server user token. Do not use project, global, or
  organization-scoped tokens.
- The organization, server URL, and project key for the repository.
- For C and C++, a successful CI analysis of a long-lived branch so Vortex has
  the compiler, dependency, and build context required for fast analysis.
- An active SonarQube Cloud subscription that includes Vortex/Agentic Analysis.

The CLI supports C and C++ in Vortex. A `403 Forbidden` response saying that
Vortex is unavailable means the account or project is not entitled; it is not
evidence that the worktree was clean.

## Authentication without saving credentials

For local automation and agent runs, provide credentials only in the process
environment. This avoids writing a token to the repository or the OS keychain:

```sh
export SONARQUBE_CLI_TOKEN="<user-token>"
export SONARQUBE_CLI_ORG="<organization-key>"
export SONARQUBE_CLI_SERVER="https://sonarcloud.io"

sonar auth status
```

For SonarQube Server, set `SONARQUBE_CLI_SERVER` to the server URL and omit
`SONARQUBE_CLI_ORG`. Unset the token when the run is complete:

```sh
unset SONARQUBE_CLI_TOKEN
```

`sonar auth login` stores credentials in the system keychain and must not be
used for an ephemeral agent run. Never place tokens in this repository, shell
scripts, command output, or issue comments.

## Resolve and verify the project

Do not guess a project key. Discover it from the authenticated organization:

```sh
sonar list projects --query horo-engine
```

Then verify the selected project explicitly:

```sh
sonar auth status
sonar list projects --query <project-name-or-key>
```

Keep the resolved key in a local environment variable or pass it with
`--project`; do not commit account-specific values to the repository.

## Analyze one worktree

Run the command from the worktree being inspected. With no file selector,
`sonar analyze` collects staged, unstaged, and untracked files from that Git
worktree. JSON is the preferred format for agents and automation:

```sh
sonar analyze \
  --project <project-key> \
  --format json \
  --depth STANDARD
```

`STANDARD` is the fast per-change mode. Use `DEEP` when cross-file context is
important:

```sh
sonar analyze --project <project-key> --format json --depth DEEP
```

The result separates `secrets` findings from `agentic` findings and reports
skipped files, failures, and global errors. Treat a non-zero exit or a
`globalError` as a failed validation even when the secrets section is clean.

## Select a different change set

```sh
# Staged files only.
sonar analyze --project <project-key> --staged --format json

# Changes relative to a branch or commit.
sonar analyze --project <project-key> --base origin/main --format json --depth DEEP

# One or more explicit files.
sonar analyze --project <project-key> \
  --file src/example.cpp \
  --format json --depth STANDARD
```

Use `--base` for a branch comparison. Use the default change-set mode when the
goal is specifically the current worktree's uncommitted edits.

## Scan all dirty worktrees

The command must run with each worktree as its current directory. This example
skips clean worktrees and keeps the output associated with its path:

```sh
git worktree list --porcelain |
  awk '/^worktree / { print substr($0, 10) }' |
  while IFS= read -r worktree_path; do
    if [ -n "$(git -C "$worktree_path" status --porcelain)" ]; then
      printf '\n=== %s ===\n' "$worktree_path"
      (
        cd "$worktree_path" || exit
        sonar analyze \
          --project <project-key> \
          --format json \
          --depth STANDARD
      )
    fi
  done
```

Do not combine files from different worktrees into one invocation. Their
relative paths, Git change sets, and build contexts are independent.

## C/C++ expectations

Vortex C/C++ analysis restores compiler and dependency context from a prior CI
analysis. It does not use the active VS Code workspace, IntelliSense settings,
or a randomly selected `compile_commands.json` entry. A project without a
successful long-lived-branch CI analysis may return no usable C++ findings or
may reject the request.

For a C/C++ worktree result, verify all of the following:

- the output contains the intended files under `agentic.files`;
- `agentic.summary.totalSkipped` is zero unless a skip is intentional;
- `agentic.failures` is empty;
- `globalError` is absent;
- the reported project and branch context match the worktree being reviewed.

An empty issue list is not a clean result if files were skipped or analysis was
forbidden. Report the analyzer state and the skipped paths separately.

## Reporting and follow-up

Report each finding with its severity, rule, file, line, and message. Keep the
raw JSON available for automation, but summarize the actionable findings for
review. A local result does not resolve or close a server issue, and it does not
replace the CI quality gate.

After an authorized fix, rerun the same `sonar analyze` command for the changed
worktree and compare the findings. Do not modify issue status automatically.

## Troubleshooting

| Symptom | Meaning | Recovery |
| --- | --- | --- |
| `Authentication failed` | Wrong token, server, or organization | Run `sonar auth status`; use a user token and the correct Cloud region. |
| Project list is empty | The authenticated organization cannot see the project | Recheck `SONARQUBE_CLI_ORG` and project access; do not guess a key. |
| `403` / Vortex unavailable | Vortex is not entitled for this connection | Report failed CLI quality validation; when file-level local C++ feedback is needed, run the IDE bridge workflow separately. |
| `no files in the change set` | The worktree is clean or the command ran in another directory | Run from the intended worktree and inspect `git status --porcelain`. |
| Files are skipped | The change set includes unsupported, ignored, binary, or oversized files | Report skipped paths; do not call the result clean without checking them. |
| C++ analysis has no usable context | No suitable long-lived-branch CI analysis exists | Run the supported CI analysis first, then repeat the worktree scan. |

Do not repeatedly retry an unchanged authorization or entitlement failure.

## Validation checklist

Before reporting success, record:

1. `sonar --version` and `sonar auth status` passed.
2. The exact project key came from `sonar list projects`.
3. The command ran from the intended worktree.
4. The output has no `globalError`, unexpected skips, or failures.
5. Secrets and Agentic/Vortex results were reported separately.
6. No token or generated report was written to the repository.

## References

- [SonarQube CLI](https://docs.sonarsource.com/sonarqube-cli)
- [SonarQube CLI quickstart](https://docs.sonarsource.com/sonarqube-cli/quickstart-guide.md)
- [Sonar Vortex analysis](https://docs.sonarsource.com/agent-centric-development-cycle/inside-your-agent-the-agentic-loop/sonar-vortex-analysis.md)
