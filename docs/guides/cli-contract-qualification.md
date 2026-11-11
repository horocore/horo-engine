# CLI contract qualification

This local delivery was rebased onto the verified local parent
`5d0aff9d63b03b3370f0f09fab49893ec0e4ae08`.
Its parent is GitHub #1857 / HORO-1813 [CLI-001]. The remaining children are
GitHub #1865 / HORO-1821 [CLI-001.8] and GitHub #1866 / HORO-1822 [CLI-001.9].
Their titles and identities were checked against both issue services on
2026-11-11. Both children remain open in GitHub and To Do in Jira.

## Security changes and migration

`CliOptionParser` rejects ordinary argv input for options marked `sensitive`,
including inline, separate, long and short forms. Rejection happens before
path normalization or stdin capture. The diagnostic contains no input value.
Registry admission already rejects sensitive positionals, defaults and
configuration bindings. No production command currently declares a sensitive
option. Future credential-consuming commands require the protected provider
contract instead of an argv value.

`Diagnostics::ValidateDiagnosticBundleDestination` is an additive Foundation
API on the existing owned `DiagnosticBundle.h` header. CLI calls it before
collection progress. `HostObservabilitySession` calls it before flushing logs
or invoking summary providers. Generation repeats the check before creating
directories or collecting its allowlist. Existing output files, dangling leaf
links, occupied `.tmp` paths, malformed paths and non-directory parents fail
with the original `observability.bundle.invalid_request` identity. Existing
directory links are resolved using native directory semantics, including OS
temporary-directory aliases. Explicit output remains an invocation-owned value.
The existing HostObservability allowlist and bundle redactor own exported data.

Callers that previously left an occupied temporary path now receive an
invalid-request error before collection. Retry requires choosing a fresh
destination or resolving that owned staging artifact. The path preflight
does not reserve a destination against concurrent filesystem replacement.
Native `.tmp` path construction preserves Unicode spelling.

The public header remains owned by HoroFoundation. No target ownership or
dependency promotion changes. `CliOutputPublicHeaderConsumer` exercises the
new API through its existing declared Foundation dependency.

## Regression sources

| Acceptance | Executable evidence source |
| --- | --- |
| Registry, generated help, parser, precedence, stdin, interaction, dispatch admission | `HoroCliCommandRegistryTests` |
| Sensitive argv rejection before input acquisition | `CliOptionParserTests.cpp`, synthetic credential in four argv forms |
| Destination validation without mutation and output-link handling | `HoroObservabilityTests` |
| Destination validation before summary collection, export redaction and host shutdown | `HoroHostObservabilityTests` |
| Native JSON/JSONL framing, invocation-only export intent, environment/output isolation, preserved occupied files | `HoroCliProductionOutputContract` |
| Every registered command's schema identity/version and required result fields, plus all eight stable exit categories | `tests/fixtures/cli/output-v1.json`, production, presenter and adapter tests |
| Bounded progress retention with a stalled consumer, output capacity, interrupts and timeout translation | CLI progress, dispatcher and presenter suites |
| Live presentation during synchronous dispatch, slow writer isolation and writer-failure shutdown | `HoroCliHostLifecycleTests` |
| Headless transitive dependency rejection, aliases and LINK_ONLY edges | `HoroCliHeadlessDependencyPolicy`, production configure guard |
| MCP protocol channel isolation and bounded disconnect/shutdown | `HoroCliMcpServeTests`, `HoroCliMcpStdioContract` |
| Equivalent real navigation publications and exactly one approved MCP mutation | `HoroNavigationBakeAdapterTests` |
| Native project capture, cache publication and MCP approval barriers | `HoroNavigationNativeCliTests`, `cli_navigation_mcp_contract.py` |
| Actual native script/module stdout framing and shared package producer fields | Reference Extension, Module Script Product Host and Package Application suites |
| Native process launch failure, cancellation, forced timeout and owned descendant cleanup | `HoroPlatformTests` process cases |
| Windows native CTRL_C/CTRL_BREAK with redirected MCP channels | `cli_mcp_stdio_contract.py`, exact birth identity and private hidden console verification |

Navigation equivalence uses the GUI-facing application boundary headlessly,
the actual CLI parser/dispatcher and the actual authorized MCP registry. It
compares input fingerprints and committed manifest digests. Each path builds
the same two tiles. MCP replay cannot create another operation or publication.
Live GUI interaction remains separate evidence.

## Open acceptance and required evidence

GitHub #1826 / HORO-1782 [CFG-001.6] remains an external prerequisite.
`SettingSensitivity::SecretReference` exists, but Configuration resolution
rejects such values. `SettingValue` remains bool/integer/string, and the
Configuration API provides no typed credential-reference resolution seam.
The separate Security credential primitives do not establish that missing
Configuration contract. Provider success/failure, approved environment secret
references and their end-to-end operation integration remain unqualified.

The fixture contains every registered command schema. Production help discovery
is compared against its complete inventory, so adding a command requires a
fixture. Native script, module and navigation successes validate actual stdout.
Package descriptors and actual install/restore/disable results validate the
shared package result producer. Activation denial remains covered by the
existing trust/selection regressions. Genuine native-extension activation
success requires its separately approved package trust fixture.

The native host now accepts policy-bounded `--timeout-ms` before startup and
preserves that absolute deadline through dispatch. Opt-in deadlines preserve
the existing unlimited default for commands whose default is zero. A native
MCP regression holds stdin open while requiring timeout exit 8 and empty
protocol stdout. The new progress worker owns presentation until join and
samples the existing bounded mailbox during synchronous application dispatch.
Its slow-writer regression holds output blocked while 65,537 producer reports
complete, then verifies bounded retained text and ordered final presentation.
A separate 4,096-invocation projection stress regression checks two-record
terminal retention and release of each operation's cancellation callback owner.
Final native run evidence for these additions remains pending.

The current application source tree and selected build contain no `horopak`
executable target. Its approved packaging composition and absence of RenderApi
dependencies therefore remain unqualified. The root guard also applies if
that target is introduced later.

Linux and macOS process-tree, signal, repeated interrupt, timeout, startup,
shutdown and adapter suites require native runs. Windows evidence does not
qualify those platforms. Symlink fixtures that cannot acquire native link
creation privileges must be reported as skipped. Native permission-denial
and concurrently replaced filesystem paths require separate qualification.

The Windows control-event fixture owns a new hidden console for each CLI
child. A separate helper verifies its live process creation identity and
requires that the console contains only that CLI and the helper before
broadcasting. The test driver's console and handlers remain untouched.
These native additions require the final Windows run before qualification.

## Local run evidence

The first Windows run used `build/validate_cli.py` in this worktree, imported
`scripts/dev.py`'s `heavy_build_lock`, built with `--parallel 1` and set
`CTEST_PARALLEL_LEVEL=1`. It configured headless MSVC/Ninja with native
Navigation enabled and built the CLI, observability and native Navigation
targets. CTest selected 113 tests and reported three failures. Two targets
were absent from that build selection. The remaining failure assumed
`OperationSpan::Complete` guaranteed queue acceptance, although telemetry
uses a nonblocking, best-effort queue. The lifecycle regression now checks
observed queue acceptance before requiring its persisted history record.
Two native Navigation symlink cases were skipped for unavailable privileges.
This initial run predates the final regression edits and is partial evidence.

The physical lock handle was not verified in that initial run. Subsequent
helpers inspect their own native open-file handles after acquisition and
before configuring, compiling, testing or preparing analysis compiler context.
They require the canonical machine-wide lock handle and stop on a mismatch.

Final source, consumer and quality verification is pending the shared heavy
validation queue. The parent and both children retain their open acceptance.

## Whole-parent acceptance audit

The audit refreshed GitHub #1857–#1866 and Jira HORO-1813–HORO-1822 on
2026-11-11. GitHub closes .1–.7, but Jira records .1 as Done, .2–.6 as
In Review, and .7–.9 as To Do. These workflow states do not establish acceptance.
The persistent CLI-001 goal covers all nine children. No issue state was changed.

Criterion numbers below follow each issue's acceptance list in order. A source
or regression listed here is implementation evidence, not a passing final run.
All affected execution evidence must be refreshed after the final edits.
ADR-019 still says Proposed; the current normative CLI architecture is the
contract authority. No ratification or missing composition is inferred from a
closed decision issue.

| Child / Jira | Criterion | Implementation or missing evidence |
| --- | --- | --- |
| [CLI-001.1 #1858](https://github.com/horocore/horo-engine/issues/1858) / HORO-1814 | A1. Every CLI contract type and runtime service has one documented owner and dependency direction. | CLI architecture ownership table and HoroDependencyPolicy |
| [CLI-001.1 #1858](https://github.com/horocore/horo-engine/issues/1858) / HORO-1814 | A2. The migration path removes ad hoc parsing without preserving two competing command sources of truth. | NormalizeArguments plus registry-generated discovery/help |
| [CLI-001.1 #1858](https://github.com/horocore/horo-engine/issues/1858) / HORO-1814 | A3. `horopak` is limited to approved packaging capabilities and cannot expose the complete engine host. | Normative boundary only; horopak and HoroEngine::Archive targets are absent |
| [CLI-001.1 #1858](https://github.com/horocore/horo-engine/issues/1858) / HORO-1814 | A4. Domain modules contribute adapters but cannot place business rules in CLI handlers. | NavigationCommands and PackageCommands delegate application services |
| [CLI-001.1 #1858](https://github.com/horocore/horo-engine/issues/1858) / HORO-1814 | A5. CLI, MCP, and debug-console boundaries are explicit and protected by dependency tests. | CliHost dependency allowlist and MCP boundary configure tests |
| [CLI-001.2 #1859](https://github.com/horocore/horo-engine/issues/1859) / HORO-1815 | A1. Duplicate command paths, option names, incompatible schemas, unauthorized capabilities, and unsupported hosts fail activation with typed diagnostics. | CliCommandRegistryTests duplicate/schema/capability/host cases |
| [CLI-001.2 #1859](https://github.com/horocore/horo-engine/issues/1859) / HORO-1815 | A2. Built-in and contributed commands use the same descriptor shape and validation path. | Same registry Create admission for built-in and extension contributions |
| [CLI-001.2 #1859](https://github.com/horocore/horo-engine/issues/1859) / HORO-1815 | A3. Help and discovery contain only accepted descriptors and have no parallel handwritten command inventory. | GenerateHelp/Discover plus production command-inventory fixture |
| [CLI-001.2 #1859](https://github.com/horocore/horo-engine/issues/1859) / HORO-1815 | A4. Registry ordering and generated output are deterministic across supported platforms. | Registry permutation/order tests; native platform runs outstanding |
| [CLI-001.2 #1859](https://github.com/horocore/horo-engine/issues/1859) / HORO-1815 | A5. Contract-version compatibility is validated before a contribution becomes executable. | Registry contract-version compatibility tests |
| [CLI-001.3 #1860](https://github.com/horocore/horo-engine/issues/1860) / HORO-1816 | A1. Unknown options, missing values, invalid ranges, conflicting sources, and unsupported input modes fail before execution. | Parser aggregated diagnostics and timeout-before-stdin tests |
| [CLI-001.3 #1860](https://github.com/horocore/horo-engine/issues/1860) / HORO-1816 | A2. Explicit command options, environment indirection, and persisted configuration resolve according to the shared configuration contract. | Configuration provenance and invocation-over-snapshot tests |
| [CLI-001.3 #1860](https://github.com/horocore/horo-engine/issues/1860) / HORO-1816 | A3. `StdinPolicy::None` commands cannot block on piped or inherited stdin. | Never-read-inherited-stdin parser regression |
| [CLI-001.3 #1860](https://github.com/horocore/horo-engine/issues/1860) / HORO-1816 | A4. Non-interactive execution never opens an implicit prompt and every prompt has a deterministic option alternative. | Required deterministic interaction alternative regression |
| [CLI-001.3 #1860](https://github.com/horocore/horo-engine/issues/1860) / HORO-1816 | A5. Paths, JSON, JSONL, and binary inputs are normalized and bounded before dispatch. | Path normalizer and bounded JSON/JSONL/binary regressions |
| [CLI-001.4 #1861](https://github.com/horocore/horo-engine/issues/1861) / HORO-1817 | A1. JSON mode emits exactly one schema-valid result document on success and failure. | Presenter one-document cases and native output fixture |
| [CLI-001.4 #1861](https://github.com/horocore/horo-engine/issues/1861) / HORO-1817 | A2. JSONL commands emit only declared records and a valid terminal summary contract. | Declared JSONL progress and one terminal summary cases |
| [CLI-001.4 #1861](https://github.com/horocore/horo-engine/issues/1861) / HORO-1817 | A3. TTY detection changes presentation only in human mode and never changes structured schemas. | TTY versus pipe schema equality cases |
| [CLI-001.4 #1861](https://github.com/horocore/horo-engine/issues/1861) / HORO-1817 | A4. Equivalent application errors preserve meaning across GUI, CLI, and MCP translations. | Shared GUI/CLI/MCP error projection comparisons |
| [CLI-001.4 #1861](https://github.com/horocore/horo-engine/issues/1861) / HORO-1817 | A5. Exit codes distinguish usage, validation, unavailable capability, operation failure, security, cancellation, timeout, and unexpected host failure. | Versioned exit-category fixture and host-failure regression |
| [CLI-001.5 #1862](https://github.com/horocore/horo-engine/issues/1862) / HORO-1818 | A1. Commands receive only their declared capabilities and cannot obtain an omnibus mutable engine object. | Explicit execution context capability span and scoped adapter construction |
| [CLI-001.5 #1862](https://github.com/horocore/horo-engine/issues/1862) / HORO-1818 | A2. Missing or unauthorized capabilities fail before domain mutation begins. | Dispatcher registration and invocation admission regressions |
| [CLI-001.5 #1862](https://github.com/horocore/horo-engine/issues/1862) / HORO-1818 | A3. Domain handlers call the same application use cases as GUI and MCP adapters. | Navigation application/CLI/MCP publication equivalence regressions |
| [CLI-001.5 #1862](https://github.com/horocore/horo-engine/issues/1862) / HORO-1818 | A4. Invocation, operation, job, and safe project context remain correlated through terminal result production. | Invocation/operation/job/project correlation tests |
| [CLI-001.5 #1862](https://github.com/horocore/horo-engine/issues/1862) / HORO-1818 | A5. One-shot commands leave no subscriptions, services, or background work alive after host teardown. | Adapter destruction ordering plus native service/host shutdown tests; repeated lifecycle evidence pending |
| [CLI-001.6 #1863](https://github.com/horocore/horo-engine/issues/1863) / HORO-1819 | A1. Slow or disconnected progress consumers cannot delay the underlying operation. | One-slot progress mailbox and stalled-writer producer stress |
| [CLI-001.6 #1863](https://github.com/horocore/horo-engine/issues/1863) / HORO-1819 | A2. Cancellation, timeout, signalled termination, forced termination, and ordinary failure remain distinct stable terminal outcomes. | Stop controller, dispatcher reason translation and external process tests |
| [CLI-001.6 #1863](https://github.com/horocore/horo-engine/issues/1863) / HORO-1819 | A3. POSIX process groups follow the documented graceful-to-forced sequence and leave no descendant running. | POSIX external process group tests; Linux/macOS native evidence missing |
| [CLI-001.6 #1863](https://github.com/horocore/horo-engine/issues/1863) / HORO-1819 | A4. Windows process groups and Job Objects follow the documented graceful-to-forced sequence and leave no descendant running. | Windows external process tests and private-console interrupt fixture; final run pending |
| [CLI-001.6 #1863](https://github.com/horocore/horo-engine/issues/1863) / HORO-1819 | A5. Output pipes are drained within bounds before the terminal result is published. | External process bounded drain contracts and native output test |
| [CLI-001.6 #1863](https://github.com/horocore/horo-engine/issues/1863) / HORO-1819 | A6. Process and cancellation tests run on Linux, macOS, and Windows and prove equivalent Horo-level outcomes. | Linux/macOS/Windows equivalence remains unqualified |
| [CLI-001.7 #1864](https://github.com/horocore/horo-engine/issues/1864) / HORO-1820 | A1. Headless commands link and run without editor, ImGui, window, or interactive-renderer targets. | Native headless graph plus negative dependency fixture; final configure/run pending |
| [CLI-001.7 #1864](https://github.com/horocore/horo-engine/issues/1864) / HORO-1820 | A2. A command requiring an unavailable optional capability fails explicitly without silent fallback. | Registry host/capability denial and optional-capability native failures |
| [CLI-001.7 #1864](https://github.com/horocore/horo-engine/issues/1864) / HORO-1820 | A3. `mcp serve` does not duplicate MCP protocol, registry, authorization, or session behavior. | McpServe composes MCP-owned controller/session/registry |
| [CLI-001.7 #1864](https://github.com/horocore/horo-engine/issues/1864) / HORO-1820 | A4. Logs and ordinary CLI presentation cannot corrupt MCP framing. | Native MCP JSONL protocol isolation and project navigation fixture |
| [CLI-001.7 #1864](https://github.com/horocore/horo-engine/issues/1864) / HORO-1820 | A5. Startup failure, disconnect, cancellation, and shutdown release sessions and application services exactly once. | McpServe lifecycle unit tests, native EOF/partial-input/interrupt/timeout cases; final run pending |
| [CLI-001.8 #1865](https://github.com/horocore/horo-engine/issues/1865) / HORO-1821 | A1. Secrets never appear in argv, stdout, stderr, logs, command previews, diagnostic records, or exported bundles. | Sensitive argv rejection and isolated synthetic environment/bundle tests; provider integration blocked |
| [CLI-001.8 #1865](https://github.com/horocore/horo-engine/issues/1865) / HORO-1821 | A2. Non-interactive execution cannot prompt or imply destructive confirmation. | Parser non-interactive deterministic alternatives and package trust denial |
| [CLI-001.8 #1865](https://github.com/horocore/horo-engine/issues/1865) / HORO-1821 | A3. Untrusted paths, oversized payloads, and unauthorized side effects fail before mutation. | Parser bounds, side-effect admission and navigation path/authority regressions |
| [CLI-001.8 #1865](https://github.com/horocore/horo-engine/issues/1865) / HORO-1821 | A4. CLI diagnostic export uses the existing HostObservability and DiagnosticBundle allowlist and adds no environment or file discovery. | Existing HostObservability producer/redactor reused; native ZIP entries checked |
| [CLI-001.8 #1865](https://github.com/horocore/horo-engine/issues/1865) / HORO-1821 | A5. Bundle destination and explicit user intent are validated before export starts. | Invocation-source output path and destination preflight tests |
| [CLI-001.8 #1865](https://github.com/horocore/horo-engine/issues/1865) / HORO-1821 | A6. Security regression tests cover malicious arguments, path escapes, symlinks, environment leakage, redaction, and credential-provider failure. | Malicious argv/path/link/environment/redaction regressions; credential-provider failure blocked by CFG-001.6 |
| [CLI-001.9 #1866](https://github.com/horocore/horo-engine/issues/1866) / HORO-1822 | A1. Versioned fixtures protect every public structured command contract and stable exit category. | Thirteen versioned schemas cover invocation and twelve commands; production inventory discovery; final execution pending |
| [CLI-001.9 #1866](https://github.com/horocore/horo-engine/issues/1866) / HORO-1822 | A2. Linux, macOS, and Windows suites cover normal, malformed, unavailable-capability, cancellation, timeout, process-tree, and shutdown paths. | Windows suites pending slot; Linux/macOS native evidence missing |
| [CLI-001.9 #1866](https://github.com/horocore/horo-engine/issues/1866) / HORO-1822 | A3. Headless dependency tests fail if GUI or renderer targets enter commands that do not declare them. | Headless negative dependency fixture; horopak actual composition absent |
| [CLI-001.9 #1866](https://github.com/horocore/horo-engine/issues/1866) / HORO-1822 | A4. Credential, argument, environment, output, log, and diagnostic-export redaction tests pass. | Security fixtures present; final native run pending and provider integration blocked |
| [CLI-001.9 #1866](https://github.com/horocore/horo-engine/issues/1866) / HORO-1822 | A5. Equivalent GUI, CLI, and MCP requests produce equivalent domain results and exactly one authoritative mutation. | Real navigation application/CLI/MCP digests/build counts and approval replay test; final run pending |
| [CLI-001.9 #1866](https://github.com/horocore/horo-engine/issues/1866) / HORO-1822 | A6. Stress tests prove bounded memory, progress, input, output, subscription, and operation retention behavior. | Parser/dispatch/output/progress bounds covered; new 4,096-invocation projection test checks two-record retention and cancellation-owner release; final execution and aggregate subscription lifecycle evidence pending |

The immediate required slot is the owned `build/validate_cli.py core` pass,
then its `broader` pass, each with the canonical physical lock verified and
one compiler/test worker. Full `scripts/analyze.py check --base` follows final
fixes with the same lock. No heavy work is queued in the background while
allocation is missing. Native Linux/macOS runners, CFG-001.6's typed provider
contract, and the missing packaging composition remain external acceptance
requirements; they cannot be replaced by mocks or a renamed SDK utility.
