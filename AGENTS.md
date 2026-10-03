# AGENTS.md

This file is the working contract for coding agents in `horo-engine`.

## Role

Default posture: make careful, production-grade C++ changes for a real engine codebase.

Optimize for, in order:
- correctness
- root-cause fixes
- ownership and lifetime safety
- architecture consistency
- regression prevention
- delivery speed

## Repository Status And Sources Of Truth

Horo Engine is an active-development C++20 engine and editor IDE. Until a stable
release line exists, APIs may evolve, but changes must still be deliberate,
reviewable, and migration-aware.

Use this precedence when guidance conflicts:

1. the user's explicit instruction for the current task
2. this `AGENTS.md` working contract
3. normative documents under `docs/architecture/`
4. accepted ADRs under `docs/adr/`
5. the current implementation and tests
6. aspirational examples or deprecated code

Important repository boundaries:

- `include/Horo/` is the public API surface. Keep it narrow, backend-neutral, and
  stable unless the task explicitly requires an API change.
- `src/` is the active implementation tree.
- `apps/` owns executable composition and process entry points.
- `tests/` owns executable regression and contract coverage.
- `docs/architecture/` is normative: implementations must follow its dependency,
  ownership, lifecycle, and capability contracts.
- `deprecated/` is migration reference only. Do not compile, repair, or extend it
  unless the task explicitly targets migration from that tree.
- `docs/architecture/desired-project-tree.md` describes the target layout; verify
  that a path exists before treating it as implemented.

`CLAUDE.md` intentionally delegates to this file. Do not duplicate repository
rules into agent-specific instruction files.

## C++ Expectations

- Prefer simple, explicit code over clever or overly generic abstractions.
- Make ownership obvious. Favor RAII, value semantics, and clear lifetime boundaries.
- Keep state transitions explicit, especially in editor, scene loading, and runtime lifecycle code.
- Avoid hidden global behavior and side effects.
- Do not introduce new heap allocation, virtual dispatch, or template complexity unless it clearly improves the design.
- Prefer narrow interfaces and stable headers.
- When the code already has a typed model, do not push behavior back into stringly-typed maps or ad hoc property parsing.
- Preserve invariants first; cleanup and elegance come second.

## C++ Design And Coding Style

- Follow the existing local style and `clang-format` output. Do not do style-only rewrites.
- Keep functions focused and easy to reason about. Break up long functions when it improves clarity, not just to move lines around.
- Prefer explicit types where they improve readability; use `auto` when the type is obvious from the right-hand side.
- Use `const` aggressively for inputs and helper variables that should not change.
- Prefer `enum class`, small structs, and plain data carriers over loose flag combinations and magic strings.
- Minimize macro usage.
- Comments should explain invariants, ownership, or non-obvious intent. Do not add comments that restate the code.
- Headers should expose the minimum necessary surface. Prefer forward declarations when they keep dependencies cleaner.

## Architecture And Dependency Discipline

- Read `docs/architecture/README.md` and the owning subsystem document before a
  cross-module or public-contract change.
- Respect dependency direction from
  `docs/architecture/foundation/system-design.md`; do not solve local convenience
  by introducing a reverse dependency.
- Keep composition in host/application boundaries. Feature code must not discover,
  instantiate, or select concrete platform/render backends.
- Prefer typed configuration, IDs, capabilities, results, commands, and events
  over string comparisons, loosely structured maps, or scattered booleans.
- Preserve error information across layers. Translate errors only at a boundary
  that can add actionable context or present them to a user.
- Treat cancellation, shutdown, rollback, and partial initialization as normal
  lifecycle paths, not exceptional cleanup added after the happy path.
- Breaking a public contract requires a written reason, affected callers, a
  migration path, and regression coverage. Do not silently preserve two competing
  sources of truth as a compatibility workaround.
- Unless an approved architecture revision explicitly authorizes an exception,
  internal module descriptors are inert metadata: creating or validating a
  descriptor should not register services, invoke lifecycle callbacks, inspect
  a service locator, select a backend, or mutate ambient state. Keep
  registration and activation in an explicit host/application composition root;
  follow `docs/architecture/foundation/internal-module-descriptor.md`. Avoid
  introducing ambient side effects for local convenience.

### Header Boundary Enforcement

- Unless the current task explicitly revises this architecture contract with a
  documented migration, assign every header under `include/Horo/` to exactly one
  real CMake target in `cmake/HoroPublicHeaderOwnership.cmake`. Configure rejects
  unowned or multiply owned public headers across the supported `.h`, `.hh`,
  `.hpp`, `.hxx`, `.inl`, `.ipp`, and `.tpp` extensions.
- Do not publish `${PROJECT_SOURCE_DIR}/include`, `${PROJECT_SOURCE_DIR}/src`, or
  another repository-wide source root through `PUBLIC` or `INTERFACE` usage
  requirements. Use `horo_configure_target_header_boundary` so consumers see only
  the owning target's staged public headers and the public headers of declared
  dependencies.
- Headers under `src/` are target-private by default. The supported options for
  a cross-target internal contract are deliberate promotion to a narrow public
  contract or a dedicated non-installed internal interface. If neither is
  appropriate, stop
  and request an architecture decision instead of restoring a broad `src/`
  include path merely to make an include compile.
- When public ownership or dependencies change, update the ownership registry,
  migration notes, and public-header consumer coverage in the same change.

## Performance And Concurrency

- Establish whether a path is frame-hot, load-time, background, or tooling-only
  before choosing an abstraction.
- Avoid per-frame heap allocation, unbounded work, blocking I/O, and accidental
  CPU/GPU synchronization in rendering and editor draw paths.
- Keep data layout and iteration order visible in performance-sensitive code.
  Prefer contiguous data and stable handles where ownership permits.
- Do not add mutexes or atomics without documenting the protected invariant,
  ownership, allowed threads, and shutdown behavior.
- Job-system work must have explicit cancellation, completion, error propagation,
  and lifetime ownership. Never capture references whose lifetime is shorter than
  the scheduled work.
- Performance claims require measurements. State the workload, build mode,
  platform/backend, metric, and before/after result.

## Documentation (Doxygen)

Single source of truth: the **header declaration** owns the full contract (`@brief`, `@param`, `@return`, `@throws`, pre/postconditions). Definitions in `.cpp` stay thin.

Rules:

- **Header declarations** — document public API and types with full Doxygen (`@brief`, `@param`, `@return`). Use `@file` + `@brief` at the top of headers that form the documented surface.
- **`.cpp` definitions** — use `/** @copydoc SymbolName */` above out-of-line definitions. Do not duplicate parameter/return blocks (drift risk).
- **`@copydoc` naming** — free functions in the same namespace: simple name (`@copydoc MakeObjectFromAsset`). Class members: `ClassName::member` (`@copydoc EditorLayer::OnMenuSave`). Qualify further only when Doxygen cannot disambiguate overloads.
- **File-local helpers** — symbols in anonymous namespaces or `static` functions with no header declaration: document at the definition in `.cpp` with `@brief` (and `@param`/`@return` only when behavior is non-obvious).
- **Types** — `@brief` on the type; document non-obvious members with `/**< ... */` trailing briefs.
- **Do not** maintain parallel full doc blocks on both declaration and definition for the same symbol.
- **Do not** use `@copydoc` for symbols that exist only in `.cpp` with no header forward declaration.

Exemplar pairs: `EditorPropertyRules.h`/`.cpp`, `Raycaster.h`/`.cpp`, `EditorLayer.h` + split `.cpp` files.

## How To Work In This Repo

- Inspect the real implementation before changing it.
- Check `git status` before editing. The worktree may contain substantial user
  changes; do not overwrite, unstage, reformat, or fold them into the task.
- Trace a symbol through its declaration, definition, callers, tests, and owning
  architecture document before changing its contract.
- Understand the boundary you are touching:
  - editor document and UI state
  - serialization and path handling
  - typed scene model and runtime conversion
  - lifecycle and reload flows
- Solve the bug completely, but avoid unrelated cleanup unless it reduces real risk.
- If a broader refactor is tempting, prefer a narrow safe fix and note the follow-up.
- When verification is partial, say exactly what was and was not validated.
- Do not add a dependency until existing dependencies and neighboring code have
  been inspected. Pin fetched dependencies according to repository policy and
  keep native/backend dependencies inside their owning target.
- Do not commit generated build output, local caches, logs, screenshots, IDE state,
  credentials, or machine-specific paths.

For general project documentation, build commands, and module overview, use [README.md](README.md).

## High-Risk Areas

- `src/editor/`: document/UI state, modal and route lifecycle, drag/drop, selection,
  settings, localization, asset operations, and live editing
- `src/runtime/`: renderer lifecycle, backend registration, frame ordering, resource
  ownership, scene/runtime conversion, and shutdown
- `src/platform/`: cross-platform paths, files, process execution, window/native
  handles, temporary storage, and OS-specific behavior
- `include/Horo/`: public API and dependency fan-out
- project metadata, serialization, reload, cache invalidation, and file deletion;
  these paths can cause silent data loss or cross-platform regressions

Changes in these areas should bias toward explicit validation and regression tests.

## Editor UI And Localization

- Use shared controls and design tokens from the editor design system. Do not
  bypass a shared component with raw ImGui widgets when the control already exists.
- Native ImGui style and custom-drawn `Theme`/design-token surfaces are separate
  layers; a theme-sensitive change must remain correct in both.
- Avoid hard-coded horizontal offsets for text-adjacent actions. Calculate widths,
  constrain wrapping, and position controls from available bounds.
- Preserve minimum typography and existing font roles. Do not introduce a smaller
  visible text tier merely to make content fit.
- Creation and transient workflows belong in modals/routes, not document tabs.
  Tabs represent persistent editor document/workspace sections.
- New user-visible editor copy must use the localization service. Keep
  `assets/localization/editor/en-US.json` and `tr-TR.json` structurally aligned and
  update localization regression coverage.
- For visual changes, verify normal, hovered, active, disabled, focused, open-popup,
  narrow-width, and long/localized-text states when applicable.

## Renderer And GPU Rules

- OpenGL, Metal, Vulkan, and future interactive renderers are equal concrete peers
  behind Horo-owned typed contracts. Never make one backend the architectural base
  of another.
- Native API types and handles stay private to concrete backend/platform targets.
  Public/editor-facing contracts expose Horo types only.
- Backend selection occurs before window and presentation creation. Do not bolt a
  different scene renderer onto an editor GUI composition owned by another API.
- Keep renderer installation, host support, runtime availability, selection, and
  activation as distinct states. Do not silently fall back to another backend.
- Avoid normal-frame waits such as GPU idle or command-buffer completion. Explicit
  waits are reserved for bounded tests, readback, teardown, or documented recovery.
- GPU resource destruction must account for frames in flight and queued references.
  A CPU owner releasing a handle does not prove the GPU has finished using it.
- Any renderer contract change must consider OpenGL-only, Metal-only where
  supported, combined-backend, and headless/test compositions.

Read these before renderer work:

- `docs/architecture/runtime/rendering-architecture.md`
- `docs/architecture/runtime/render-backend-parity-contract.md`
- `docs/architecture/runtime/renderer-distribution-and-availability.md`

## Verification Bar

- Every behavior change should add or update regression coverage when practical.
- Run targeted tests first, then broader validation when the change crosses subsystem boundaries.
- Think across Linux, macOS, and Windows when paths, temp files, file deletion, or case sensitivity are involved.
- Do not claim success without stating what commands, tests, or manual checks actually ran.

Canonical local skeleton validation:

```bash
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --parallel
ctest --test-dir build/skeleton --output-on-failure
```

Use `cmake --build build/skeleton --target <target>` and
`ctest --test-dir build/skeleton -R <test-regex> --output-on-failure` for the
targeted pass. Reconfigure after CMake options, dependencies, targets, source lists,
or platform guards change.

Additional gates by change type:

- public headers: build every affected consumer target
- CMake/target topology: configure every supported option combination affected
- renderer code: run backend contract tests and relevant GPU smoke tests when a
  compatible display/device is available
- editor UI: run render/model tests plus a manual interaction check when practical
- paths/files/processes: test spaces, non-ASCII names, missing inputs, permissions,
  and platform-specific behavior
- serialization/metadata: test malformed, duplicate, missing, oversized, and
  version-skewed inputs

`HORO_ENABLE_GPU_SMOKE_TESTS` is opt-in because it requires a display server and
hardware graphics context. Do not report those tests as passed unless they were
actually configured and run.

## Sonar Validation

Before opening or preparing any pull request, automatically apply
`.codex/skills/quality-preflight/SKILL.md`; the user need not request it separately.
Run the full Sonar IDE, CLI secrets and Codacy preflight on the actual PR worktree
against its target branch, even when only non-C/C++ files changed. Run after the
final edits and repeat the same scope after fixes. An earlier report with changed
inputs cannot satisfy this step. Record the command, findings, incomplete checks
and report path in the PR validation summary. Resolve newly introduced findings
within the authorized scope; preserve contextual findings rather than treating
them as proven pre-existing. An exit `2` or stale report leaves this required
validation incomplete; resolve or explicitly report the blocker before opening
the PR. This rule does not grant commit, push or PR creation permission.

After modifying C/C++ files, run the worktree-aware local preflight:

```bash
python3 scripts/quality_preflight.py check --base <PR-target-ref>
```

Use `--dirty` for only staged, unstaged and untracked edits. The base scope also
includes uncommitted edits. The managed Sonar IDE bridge is the supported local
C/C++ path. Vortex analysis and entitlement probes are outside this workflow.
It requires Linux, a display, VS Code,
SonarQube for IDE and connected-mode authentication. The helper owns isolated
profiles, verifies listener process identity and C++ sensor execution, and keeps
new analysis builds/reports outside the source tree (a verified existing
worktree build may be reused). Do not use unverified explicit ports,
IDE Model Context Protocol (MCP) calls, or `sonar-scanner` as substitutions.

- Follow `.github/instructions/sonarqube_mcp.instructions.md` and
  [the local-analysis guide](docs/guides/sonarqube-mcp-local-analysis.md).
- Report Sonar IDE, CLI secrets and Codacy results separately. Use
  `doctor` to check local prerequisites and Codacy cloud configuration drift.
- Verify submitted files, C++ execution evidence, skipped files, errors and
  freshness. Exit `2` means incomplete; an empty issue list alone is insufficient.
- Keep all findings on changed files; changed-line overlap is presentation
  context, not proof that other findings predate the change. Report severity,
  rule, file, line and reason. Raw cyclomatic complexity (CCN) and copy-paste detection (CPD) metrics
  are advisory estimates.
- Exclude generated output and `deprecated/` unless explicitly in scope. Fix
  newly introduced findings when safe and authorized; behavior-changing fixes
  still require authorization unless already covered by the task.
- Rerun the same scope after fixes. Local results do not resolve server issues
  or replace CI quality gates. Do not upload source/results or change rules,
  cloud configuration, issue status or Git history as part of this check.

## Review Mindset

Default review posture:
- look for correctness bugs first
- check ownership, lifetime, and invalid state transitions
- look for missing tests and quiet behavior drift
- call out uncertainty instead of guessing
- do not overwrite user changes unless explicitly asked

When reviewing or preparing a PR, focus on:
- why the bug happened
- why the fix is safe
- which invariant is now protected
- what regression coverage was added

## Commits And PRs

### Issue Title Convention

Repository issue titles must start with the ticket identifier in square brackets,
followed by a space and the human-readable title:

- `[EDT-004B.1] Build Output Data Model & Projection Contract`
- `[OBS-001] Unified Observability Foundation`

Preserve the existing ticket identifier exactly when renaming an issue. Do not
use the legacy `TICKET — Title` form for new or updated issues.

### Roadmap Taxonomy

GitHub milestones are whole-product checkpoints, not subsystem phases or a
serial implementation plan. Apply roadmap metadata as independent dimensions:

- `Area` identifies the owning parallel workstream.
- `Roadmap State` records whether work is planned, ready, blocked, or future.
- `Roadmap Horizon` records planning proximity.
- the milestone records the product checkpoint that requires the outcome.
- native `blocked by` relationships record the real technical execution order.
- parent/sub-issue relationships record initiative ownership.

Do not infer dependency order from milestone numbers, issue numbers, board
position, Area, or Roadmap Horizon. Follow
[Product Roadmap Model](docs/architecture/delivery/product-roadmap.md) and keep
subsystem capability stages separate from product milestones.

Use [parent_capability.md](.github/ISSUE_TEMPLATE/parent_capability.md) for
capability initiatives and [sub_ticket.md](.github/ISSUE_TEMPLATE/sub_ticket.md)
for their focused child work. Issue bodies must not repeat metadata already
owned by GitHub fields or native relationships, including classification,
dependencies, parent links, milestone, roadmap state, priority, or assignees.
Parent acceptance criteria describe integrated capability behavior; parent
validation records the evidence required to prove it. Sub-ticket bodies stay
limited to goal, scope, and acceptance criteria.

Use Conventional Commits with a truthful narrow type and scope. Normal delivery
must remain traceable in both Jira and GitHub; GitHub is not the sole issue
authority. The bounded emergency exception below is the only bypass.

### Jira Smart Commits And Delivery Identity

Before creating a normal delivery branch, resolve and preserve all three
identifiers:

- the exact Jira issue key shown on the Jira work item;
- the GitHub issue number with its `#` prefix;
- the bracketed repository domain ticket alias from the GitHub issue title.

The normal commit-subject and pull-request-title order is:

```text
<type>(<scope>): <imperative summary> <JIRA_ID> #<GITHUB_ISSUE> [<DOMAIN_ALIAS>]
```

The Conventional Commit prefix is the semantic change classification; do not
replace it with the Jira or domain identifier. Preserve the Jira key and domain
alias exactly, including capitalization, punctuation and brackets. Use the bare
GitHub issue number with `#` so GitHub links the delivery without implying closure.
The pull-request body owns the explicit `Closes #<number>` relationship.

A commit containing the Jira key is eligible for Jira development-panel linking
when the repository integration is connected. Follow Atlassian's
[Smart Commit syntax](https://support.atlassian.com/jira-software-cloud/docs/process-issues-with-smart-commits/)
for commands, which may follow that key when the task explicitly requires them:

```text
<JIRA_ID> #comment <text>
<JIRA_ID> #time <value> <worklog comment>
<JIRA_ID> #<workflow-transition>
```

Do not add `#comment`, `#time` or a workflow transition command merely to create
a link: those commands mutate Jira. Use them only when the user or workflow
explicitly authorizes the corresponding comment, worklog or transition, and
verify that the commit author email maps to a Jira user allowed to perform it.
The committer or delivery automation performs that check before push. An agent
compares `git log -1 --format=%ae` with the authenticated Jira user's unique
verified email; if either identity cannot be verified, it omits the command,
preserves the plain Jira key for linking and reports the blocked mutation.
For normal delivery, put each Smart Commit command on a single line in the commit
body. The subject remains reserved for its human-readable summary and traceability
identifiers. The emergency exception may omit a Smart Commit command until Jira
recovers; it does not move the command into the subject.

Rules:

- write the subject in imperative mood
- keep one commit focused on one coherent intent
- avoid placeholder subjects such as `wip`, `misc`, `tmp`, `update`, or `fix stuff`

### Branch Naming

The standard normal branch pattern is `<type>/<JIRA_ID>_<short_topic>`; the
emergency exception below defines the only alternate pattern.

- `<type>` is the Conventional Commit type: `feat`, `fix`, `chore`, `docs`, `refactor`, `test`, `perf`, `ci`, `build`
- `<JIRA_ID>` is the exact uppercase Jira issue key and is fixed before branch
  creation
- `<short_topic>` is a lowercase snake_case slug summarising the change

Use one topic slug exactly once per branch; do not append suffixes such as `_v2`
or `_final`. If the scope changes materially, open a new focused branch.

### Emergency Traceability Exception

This exception requires explicit project-lead authorization and applies only to
an active production incident or a confirmed Jira outage that makes normal Jira
identity resolution impossible. Authorization is recorded rather than inferred:
record the approver, reason and missing Jira link in the GitHub pull-request body
before delivery.

Keep the Conventional Commit type/scope, GitHub issue and known domain alias.
Use `<type>/emergency_<short_topic>` when no Jira key can be resolved; never invent
a placeholder key. Keep the pull request unmerged until normal traceability is
restored unless the project lead explicitly authorizes an emergency merge because
delay would increase incident impact.

After Jira recovers, create or identify the Jira work item, link it from the pull
request and restore the normal title metadata within one business day. Do not
rewrite published commits solely to backfill Smart Commit commands, because Jira
may execute repeated commands after history changes. Record the completed
reconciliation in the pull-request body.

Agents must not commit, push, force-push, rewrite history, merge, or change branches
unless the user explicitly requests that operation. Never discard unrelated
worktree changes to make a task easier.

Use [`.github/pull_request_template.md`](.github/pull_request_template.md) for PR structure.

## References

- General repo and build documentation: [README.md](README.md)
- Architecture index and reading order: [docs/architecture/README.md](docs/architecture/README.md)
- System dependency direction: [docs/architecture/foundation/system-design.md](docs/architecture/foundation/system-design.md)
- Ownership and lifetime: [docs/architecture/foundation/ownership-and-resource-lifetime.md](docs/architecture/foundation/ownership-and-resource-lifetime.md)
- Editor UI design system: [docs/architecture/editor/ui-design-system.md](docs/architecture/editor/ui-design-system.md)
- PR structure: [`.github/pull_request_template.md`](.github/pull_request_template.md)
