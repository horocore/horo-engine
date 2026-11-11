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
