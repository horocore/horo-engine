# Runtime console implementation and qualification

This is an implementation evidence ledger for DBG-001, not acceptance of an
unverified product composition. The source baseline is `f51e0c0f44`.
GitHub #1812 / Jira HORO-1768 and all seven children were read on 2026-11-11.
The policy child #1842 / HORO-1798 is closed/Done. ADR-018 still says Proposed;
this work follows the normative runtime document without changing ADR status.

| Child | Implementation | Remaining acceptance evidence |
| --- | --- | --- |
| #1843 / HORO-1799 | Source added: owned immutable registry, inert metadata, typed schema conversion, redacted bounded history, filtered discovery, startup allowlist, no-follow exact-path script capture and optional external SDK component | Native and SDK regressions have not run to completion |
| #1844 / HORO-1800 | Source added: actual thread identity checks, cutoff owner/render queues, JobSystem dispatch, existing OperationStore projection, cancellation and nonblocking shutdown | Native race/lifecycle tests; production host safe-point wiring; general JobSystem main-thread continuation remains unavailable |
| #1845 / HORO-1801 | Source added: help/find/version/clear, real public Scene inspection bindings, real Telemetry availability query and queue-admission audit adapter, approved Foundation support ZIP binding, typed variable inventory, profile-compiled default variable schemas, unified permission-filtered help/find, common executor text-to-variable dispatch, canonical ConfigurationService reads/commits, five apply policies and durable configuration-owner interface | Remaining three standard command handlers, host consent workflow, actual durable host adapter and host consumers of variable state; native regressions |
| #1846 / HORO-1802 | Source added: terminal input controller, central modal routing, bounded layout/focus registry, logs/jobs source projections, 4 Hz refresh and virtual row windows | Graphical packaged runtime host, remaining source panels, localization/style interaction states and native measurement results |
| #1847 / HORO-1803 | Source added: six-profile compile-selected command/variable catalogs, separate availability translation units, exact compiled-profile registry factory, launch parsing, Shipping local-UI opt-in, denied Shipping/Server overlay surfaces and fail-closed unsupported remote requests | Native six-profile builds and binary inspection; complete product artifact/config/launch wiring in a graphical host |
| #1848 / HORO-1804 | Native regression sources and optional SDK fixture added; CI grouping checks pass | Native results, final quality preflight, packaged development/shipping evidence, performance, platform and downstream integration evidence |

## Public boundary and migration

`HoroRuntimeConsole` owns the command registry, executor, core builtins and
variable contracts and depends only on Foundation. `HoroRuntimeConsoleStartup`
adds the existing root-bound ProjectReadFilesApi capability; `HoroRuntimeConsoleUi`
adds the existing Input capability. Each has one explicit public-header owner and
an automatically generated staged-header consumer. `HoroRuntimeConsoleDefaults`
owns the profile-selected catalog and explicit Telemetry adapters.
`HoroRuntimeConsoleScene` adds a private dependency on the existing Scene service
only in Editor/Development; other profiles compile an empty contribution without
the Scene dependency. No existing API is replaced. CLI descriptors and the
editor log pane remain separate presentation concerns; neither is evidence of
a running in-game terminal.

Command descriptors have allocation-free borrowed metadata. `Create` copies all
accepted bytes and nested enumeration values before returning an immutable
registry. Descriptor construction and composition invoke no service, handler,
backend discovery or activation. Handler binding is a separate host step rather
than a callable member in metadata; this makes the documented inert boundary
enforceable without exposing an executable pointer through discovery. Command
errors also expose an inert domain contribution for explicit ModuleHost registration.
Options accept `--name value` anywhere, or `name value` after positional
arguments. A positional string equal to an option name remains a literal;
mixed duplicate option spellings are rejected before dispatch.

External projects opt into `find_package(HoroEngineGameplay REQUIRED COMPONENTS
RuntimeConsole)` and link `HoroEngine::RuntimeConsole`. The default SDK inventory
does not import this component. Its dedicated fixture checks descriptor-owned
namespace registration without importing Physics or RuntimeUi. This is a
build-tree SDK component, following the existing gameplay SDK export mechanism;
it is not evidence of a new installed product distribution.

The owning runtime architecture requires exactly one permission per descriptor;
grant masks may combine permissions. Restricted additionally requires an
authenticated context, and AdminCheat additionally requires server authority.
Namespaces use the canonical engine prefix inventory plus explicit host-approved
project prefixes. A terminal command cannot also own a namespace prefix.

Entity arguments carry the complete `scene:index:generation` reference in a
typed transport DTO. A scene adapter must map all three fields into its existing
`EntityRef` and validate that exact reference; it must not resolve a bare slot in
whichever replacement scene is active when the queue drains. No existing caller
is migrated because this console API is additive to the source baseline.

Console history stores only the prepared redacted projection. Raw malformed or
denied input is not retained. Handler output must be a safe presentation value;
this history type is not a replacement for the structured logging store.

Runtime-only variables own their typed effective state. Configuration-backed
variables always read the canonical immutable snapshot; deferred session changes
are typed write requests, not effective configuration mirrors. Durable writes
require an injected configuration workflow that owns persistence and pending
publication together. The console has no direct file-writing fallback. Secret
reference settings cannot be exposed as console variables. Foundation currently
supports Boolean/Integer/String configuration values; floating-point diagnostic
variables are session-only rather than being coerced into strings.
Deferred configuration requests retain their validation revision. A concurrent
configuration publication causes a typed stale-write failure at activation;
the pending request remains visible instead of overwriting the newer value.
Interactive `name` reads and `name value` writes pass through the same command
executor, bounded owner queue, cancellation and operation store. They share
command tokenization and complete-token numeric conversion. Deferred writes
report active and pending values separately. Script startup remains explicitly
command-allowlisted; variable fallback is disabled for startup submissions.

`sys.metrics` reads Telemetry's bounded privacy-safe diagnostic snapshot. It
reports process-local instrument IDs, generation and typed availability, not
descriptor names, producer payloads or metric history. The explicit audit
adapter emits stable structured events with command ID and prepared redacted
text, excluding ambient context. Rejection by the existing telemetry queue
prevents execution. Acceptance is asynchronous queue admission, not a durability
or sink-delivery guarantee. Neither adapter initializes telemetry.

`diag.support_bundle` binds the existing Foundation ZIP generator through an
explicit host approval authority. Requests contain only previously approved
source allowlists, vetted metadata and an output path; approval acquisition must
not block on UI from a worker or reuse consent. The adapter limits work to 256
entries/64 MiB, dispatches to the real I/O queue and requires the common audit
gate. Sensitive result/error presentation is redacted. Cancellation is checked
before taking approval and before starting generation. Foundation's bounded
atomic generator has no internal cancellation API, so cancellation after that
point may still leave the approved committed output. No output is deleted by
the adapter. A production host consent workflow is still required.

The terminal controller acquires an actual InputRouter modal/text context and
submits through the same executor. It does not yet construct or paint a packaged
runtime UI document, start native IME input or provide a game host executable.
It forwards the central router's committed Unicode/pre-edit delivery once per
eligible frame, preserving modal preemption and focus-transfer discard rules.
Unit coverage of this controller must not be described as packaged UI evidence.

## Validation ledger

- `git diff --check`: passed after the current source edits.
- `clang-format --dry-run --Werror` over all console headers, sources and native
  regression files: passed.
- `pytest -q --tb=short tests/python/test_ci_workflow_grouping.py`: 39 passed.
- Earlier native Windows Debug configure in `build/skeleton`: succeeded with cached
  dependencies, native MSVC and optional GUI/SDL/OpenGL disabled.
  The current expanded source/target topology has not been configured or built.
- Initial sandbox compiler probe: failed with MSVC C1902 (PDB manager mismatch).
- Retried native build: interrupted before console tests ran; **not passed**.
- Earlier attempts invoked `scripts/dev.py heavy_build_lock` and one worker,
  but did not prove the canonical physical lock handle. No serialization claim
  is made for those attempts. The local validation helper now requires psutil
  evidence of the canonical Windows lock handle before any configure/build/test.
  No new heavy queue is started while the main CI chat retains its resource slot.
- Linux/macOS, packaged development UI, Shipping binary inspection and GPU smoke:
  not run. No cross-platform or GPU success is implied.
- Sonar IDE, CLI secrets and Codacy: not yet run on final edits.
- `analyze.py doctor --offline`: ready for local check with the existing portable
  VS Code settings profile and installed Sonar extension directory. The earlier
  settings-unavailable result used the wrong profile. Prerequisite readiness
  does not establish authentication, Workspace Trust or C++ sensor execution.
- Cloud-enabled doctor: local prerequisites ready, Codacy cloud configuration
  drift check incomplete (`cloud read exit 2`; authentication must be reviewed).
  No source or results were uploaded and no quality/security settings changed.
- Native measurement source uses one million disabled refresh calls, zero source
  queries and a 250 ns/call budget. No measured latency or budget pass is claimed
  until the test actually runs. The source-only long-session checks also have
  no native result yet.

## Cross-workstream seams

NET-007.9 (#1169), NET-008.12 (#1183) and WST-010.8 (#1652) must bind to this
registry/policy seam and the common thread dispatcher. This ledger does not claim
their services or host adapters are integrated. In particular the current
Foundation JobSystem does not expose a general main-thread continuation API;
`JobSystem.h` offers worker submission and durable job handles. Any console
safe-point queue must remain explicit at the host's
`ApplyQueuedOwnerThreadCommands`/render boundary, rather than pretending a
synchronous worker wait is a handoff.

`rg` of the baseline's `src` and `include` trees finds no `MetricsStore` or
`ProfilerCaptureService` implementation. Telemetry exposes bounded health and
metric availability (now bound to `sys.metrics`), but not the architecture's metric-history query/capture
services. Logger exposes a global minimum severity, not the category-scoped
`log.level` owner required here. `SaveThumbnailCapture` owns save-slot-specific
capture and must not be reused as a general screenshot policy by fabricating save
identities. These remaining owner capabilities need real adapters/contracts before their
standard console commands or charts can be honestly qualified. No replacement
metric store, global category-level emulation or fake capture handler is added.
