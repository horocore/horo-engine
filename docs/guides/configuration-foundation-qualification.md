# Configuration foundation qualification

This record covers CFG-001.5 / HORO-1781 / #1825, CFG-001.6 / HORO-1782 /
#1826 and CFG-001.7 / HORO-1783 / #1827 under CFG-001 / HORO-1765 / #1809.
It records executable coverage separately from execution evidence. Native
credential-provider qualification remains SEC-001 work; CLI integration remains
owned by its consuming host.

## Whole-family acceptance audit

Read-only GitHub and Jira checks on 2026-11-11 identified seven child tickets.
Their tracking states differ: GitHub has closed the first four, while Jira has
only marked the ratification ticket Done. No tracking state was changed by this
audit, and a closed ticket is not treated as execution evidence.

| Child | Jira / GitHub state | Current implementation evidence | Remaining acceptance evidence |
|---|---|---|---|
| CFG-001.1 / HORO-1777 / #1821 | Done / closed | ADR-009 records deviations, ownership and canonical precedence; ancestor `60d836468` | Preserve the decision and verify subsequent contracts against it; no reimplementation required |
| CFG-001.2 / HORO-1778 / #1822 | In Review / closed | Resolver, immutable captures, host environment seam and Platform persistence; ancestor `45623cb8d` | Rerun affected tests after current source/provenance/persistence changes; supported-platform qualification remains pending |
| CFG-001.3 / HORO-1779 / #1823 | To Do / closed | Inert module contributions, typed conflict checks and stable order; ancestor `649976b1c`; ModuleHostCompositionTests captures a snapshot through shutdown | Rerun composition tests with the changed descriptor/value contract; no new contribution implementation required |
| CFG-001.4 / HORO-1780 / #1824 | To Do / closed | Staged reload, coalescing and queued owner delivery; ancestor `b23d6b915`; reload storm/boundary/reader tests | Rerun after prepared-writer integration and check notification counts and delivery affinity |
| CFG-001.5 / HORO-1781 / #1825 | To Do / open | Current uncommitted descriptor session, combined durable modal Apply, typed validation, stale rejection and preview rollback | Compile and execute model/lifecycle/presentation tests; verify both theme layers and manual localized interaction |
| CFG-001.6 / HORO-1782 / #1826 | To Do / open | Current uncommitted typed reference, Security lifecycle provider seam, operation-local secure result and redaction tests | Build public consumers and execute persistence/log/diagnostic/read-time failure tests; downstream native provider qualification is separate |
| CFG-001.7 / HORO-1783 / #1827 | To Do / open | Current uncommitted hostile-input, recovery, concurrent writer, path and modal tests plus the matrix below | Execute those tests and required quality checks; obtain supported-platform and permission/filesystem evidence |

All four prerequisite SHAs were verified as ancestors of the current worktree
HEAD. Parent CFG-001 / HORO-1765 / #1809 remains open/To Do: current evidence does
not yet prove identical results across supported platforms, complete reader
isolation, or the complete integrated notification and persistence contract.

The local branch is `feat/HORO-1782_configuration_completion` in worktree `6c5e`.
It was rebased with autostash onto the verified local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`. All 48 changed/untracked paths were
preserved: 47 retained identical content hashes; `tests/CMakeLists.txt` contains
the new parent C11 fixture configuration plus the same two task additions.
There is no task-specific commit, remote branch or PR yet. This is an acceptance
audit and local implementation checkpoint, not completed delivery.

## Diagnostic and transaction matrix

| Input or transition | Required outcome | Executable coverage |
|---|---|---|
| Every source layer, explicit Session exception | Canonical precedence and winning provenance | `ConfigurationTests.cpp`: Resolver Applies Every Canonical Precedence Layer; Session Override Policy |
| Unknown key, forbidden source, wrong type in any layer | Sorted safe diagnostics; no partial activation, including shadowed inputs | Resolver Rejects All Invalid Sources; Failed Multi Source Resolution |
| Missing/duplicate environment aliases, unadmitted binding | Reject binding before reading; use injected process only | Environment Capture Uses Only The Injected Host Provider; Environment Capture Rejects Missing Aliases |
| Descriptor ranges and string rules | Same validation at schema, source, environment and draft ingress | Descriptor Rules Reject Invalid Shadowed Values; descriptor session tests |
| Duplicate JSON key, unsupported version/member/type, oversized input | Fail before activation, with bounded diagnostics | Versioned Documents Are Strict Bounded And Deterministic |
| Excessive depth, malformed UTF-8, surrogate, NUL or trailing input | Stop bounded SAX validation before DOM construction | Hostile Documents Reject Nesting Encoding And Trailing Data; Invalid UTF8 Cannot Enter Snapshots |
| Host numeric locale changes | Byte-identical canonical serialization | Configuration Serialization Is Independent Of The Host Numeric Locale |
| Copied snapshot/provenance outlives service | Immutable captured values remain owned | Copied Configuration Provenance Owns Its Captured Inputs |
| Preview, Cancel, invalid edit | Active snapshot and events unchanged; typed inline error | Draft Previews Retain Precedence; `ConfigurationSettingsSessionTests.cpp` |
| Shadowed source changes externally | Advance revision; stale Apply requires reload/review | Draft Previews Retain Precedence; Modal stale configuration review |
| Read-only, invocation-only or sensitive field | No ordinary editable control or draft write | Read Only And Invocation Only Descriptors; descriptor session and presentation tests |
| Prepared persistence fails or transaction is abandoned | Keep active snapshot and prior durable document | Prepared Configuration Writes Cancel Cleanly; Configuration File Store Never Publishes Partial Or Failed Writes |
| Configuration callback during combined editor Apply | New durable editor projection and configuration revision already observable | Shared descriptors commit with legacy controls through one durable modal operation |
| No-op Apply/reload | No extra configuration notification | Prepared Configuration Writes; Reload Of Unchanged Values Does Not Notify |
| Reload storm, invalid replacement, direct commit, policy boundary | Latest valid complete candidate only; cancellation and host-owned activation | Configuration reload policy, storm and concurrent-reader tests |
| Concurrent native writers/readers, orphan preparation | Old/new complete document only; exclusive private preparation | `PlatformTests.cpp`: Concurrent Configuration Writers; Configuration Preparation Recovers Orphans |
| Relative, space/Unicode and Windows long paths, missing input, injected I/O failures | Preserve path identity and typed failure; retain prior file | Configuration File Store tests and native durable filesystem tests, including a long-path configuration round trip |
| Credential reference ingress | Exact bounded opaque identity; strings rejected; no provider lookup | Credential References Round Trip Without Admitting Plain Strings |
| Primitive CLI option bound to a reference-valued setting | Typed incompatible-value failure without reference coercion or disclosure | `CliOptionParserTests.cpp`: CLI primitive options reject credential references |
| Operation credential resolution | Explicit lifecycle provider, short-lived `SecureBytes`, expiry/revocation/unavailable fail closed | `SecurityBaselineTests.cpp`: Configuration credentials resolve only through the operation provider |
| Provider error payload contains a dummy secret | Strip message, diagnostics and causes; logs/files/events contain no material | Same Security test exercises actual persistence and logger output |
| Shared modal plus legacy controls | One durable User-source transaction; source overrides preserved | `SettingsModalLifecycleTests.cpp` |
| Light/dark, narrow/wide, long text, English/Turkish | Render without changing active configuration | `SettingsModalPresentationTests.cpp` |
| Cancel theme preview after an external commit | Restore both ImGui colors and design tokens to the current committed theme; preserve its revision | Cancelling a shared theme preview restores both committed theme layers |
| Explicit Unicode theme resource roots | Comments remain supported; duplicate/deep resources rejected | `EditorThemeBehaviorTests.cpp` |

Foundation tests are in `tests/unit/foundation/ConfigurationTests.cpp`; other
filenames in the matrix refer to the corresponding `tests/unit` subtree. A test
source entry describes intended coverage and does not by itself establish a pass.

## Execution evidence

The current implementation has **not yet completed C++ qualification**. On
2026-11-13 the user replaced named-grant scheduling with nonblocking physical
lock admission. The first resumed core batch found the canonical lock busy and
returned immediately without starting configure, compilation, CTest or analysis.
No background waiter was created. A named main-chat grant is no longer required.

The branch now includes published parent
`3df476ff430a2ad28cd62a8a0c1780ae0fea71bc`. Its only autostash conflict was in the
editor JSON reader; resolution retains the bounded SAX validation and ordinary
DOM parse, including duplicate-key rejection. The saved source checkpoint is
retained locally outside delivery artifacts.

Completed local checks:

- `python -m pytest -q --tb=short tests/python/test_configuration_settings_localization.py tests/python/test_navigation_settings_localization.py`:
  five tests passed on Windows, 2026-11-11.
- `git diff --check`: passed after the current source edits.
- Changed C++ files were formatted with the repository's available clang-format.
- On 2026-11-13, `python -m pytest -q --tb=short
  tests/python/test_local_build_cache.py
  tests/python/test_configuration_settings_localization.py
  tests/python/test_navigation_settings_localization.py`: 30 passed, two skipped.
  This includes real OS-lock contention/release coverage through
  `heavy_build_lock(..., blocking=False)`; it does not establish a C++ pass.

Pending validation, to be recorded with actual command/result evidence:

1. Configure the exact worktree; build configuration, module descriptor and
   composition, security, platform, CLI, editor settings and all affected
   public-header consumers with one compiler worker.
2. Run the corresponding CTest targets, including GUI headless lifecycle,
   presentation and theme tests in a GUI-enabled composition.
3. Validate affected headless and GUI CMake target/dependency combinations and
   compile the editor host composition.
4. Run the final worktree-aware `scripts/analyze.py check --base
   origin/codex/editor-improvements`, and record Sonar IDE, CLI secrets and Codacy
   findings/completeness separately. A prerequisite-only doctor run is not
   analysis evidence.
5. Obtain Linux/macOS native filesystem results and any platform-dependent skip
   details, plus manual modal interaction for hover, focus, active, disabled,
   popup, localized and constrained-width states. Headless draw tests do not
   establish manual visual acceptance or native credential-store qualification.

Windows heavy work must own the repository's physical shared heavy-build lock;
an isolated sandbox lock is not coordination evidence. No CI, GPU smoke, native
credential-store or cross-platform result is claimed by this record.
