# CLI Architecture

## Purpose

This document defines command discovery, parsing, application use-case
integration, output, exit codes, progress, cancellation, configuration,
executable composition, adapter equivalence, and testing for the `horo-engine`
and `horopak` command-line hosts.

Normative decision: [ADR-019: CLI Host, Command Ownership, Adapter Equivalence and horopak Boundary Decision](../../adr/019-cli-host-command-ownership-adapter-equivalence-and-horopak-boundary.md).

## Core Decisions

- `HoroEngine::CliHost` owns option parsing, command registry, help formatting,
  execution dispatch, structured presentation, and exit-code mapping.
- CLI commands are host presentation adapters over shared application use cases
  via `ICliCommandAdapter`.
- Parsing, execution, and presentation are separate stages.
- Every command declares human and machine-readable output contracts.
- Machine-readable stdout never contains logs, progress decoration, or prompts.
- Exit codes represent stable categories.
- Long-running commands expose cancellation and progress without requiring GUI.
- Interactive prompting is explicit and disabled in non-interactive mode.
- Headless execution runs without window, ImGui, or GPU dependencies unless the
  command explicitly declares them.
- CLI registry, in-game Debug Console (`DBG-001`), and AI agent MCP tools (`MCP-001`)
  maintain distinct registries with explicit delegation seams (`console exec`, `mcp serve`).
- `horopak` is strictly limited to asset cooking, archive packing, verification, and
  extraction, and cannot instantiate the engine GUI or rendering pipeline.

## Executable Composition and Responsibilities

Horo Engine partitions command-line and automation tasks across three dedicated
composition roots:

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                                 Horo Engine                                 │
├───────────────────────────┬───────────────────────────┬─────────────────────┤
│        horo-engine        │        HoroEditor         │       horopak       │
│                           │                           │                     │
│ - Headless host & CLI     │ - Graphical Editor IDE    │ - Standalone pack   │
│ - Project / Asset / Test  │ - ImGui screen host       │   and cook utility  │
│ - Headless MCP server     │ - Viewport rendering      │ - Archive format    │
│ - Zero GUI / GPU default  │ - Embedded MCP server     │ - Zero GUI/Renderer │
└───────────────────────────┴───────────────────────────┴─────────────────────┘
```

### 1. `horo-engine` (Primary Host & Terminal CLI)

- **Binary**: `apps/horo-engine`
- **Ownership**: The primary terminal executable, batch automation host, test runner,
  and headless MCP server.
- **Dependencies**: Links `HoroEngine::Application`, `HoroEngine::CliHost`,
  `HoroEngine::Platform`, `HoroEngine::Foundation`, `HostObservability`, and domain
  service libraries (`HoroEngine::Assets`, `HoroEngine::Testing`,
  `HoroEngine::ProjectMigrations`).
- **Invariants**:
  - Compiles and runs in headless environments (CI runners, containers, servers)
    without display servers (X11, Wayland, Cocoa) or GPU hardware contexts.
  - Does NOT link `HoroEngine::Gui`, ImGui, or concrete viewport renderers
    (`HoroEngine::EditorViewportOpenGL`, `HoroEngine::EditorViewportMetal`).
  - Commands requiring GPU acceleration (e.g. GPU baking) declare explicit
    capabilities and select null/headless backends when run without display hardware.

### 2. `HoroEditor` (Graphical IDE & Embedded MCP Host)

- **Binary**: `apps/HoroEditor`
- **Ownership**: The graphical authoring environment, document workspace, docked
  panel host, and live viewport render extractor.
- **Dependencies**: Links `HoroEngine::Gui`, `HoroEngine::EditorServices`,
  `HoroEngine::EditorRenderExtraction`, `HoroEngine::RenderFrontend`,
  `HoroEngine::Runtime`, concrete render viewports (`OpenGL` / `Metal`), and
  `HoroEngine::Mcp`.
- **Invariants**:
  - Owns window creation, OS presentation loops, ImGui frame rendering, and
    viewport picking.
  - Can host `McpServer` over stdio or local HTTP/SSE while the editor runs,
    dispatching agent tool calls directly to the main editor thread.

### 3. `horopak` (Specialized Asset Packaging & Cook Utility)

- **Binary**: `apps/horopak`
- **Ownership**: Standalone, lightweight CLI utility for `.horo` asset pak creation,
  table of contents (TOC) inspection, integrity verification, compression, encryption,
  and extraction.
- **Dependencies**: Links `HoroEngine::CliHost`, `HoroEngine::CliApi`,
  `HoroEngine::Archive` (pak format, SHA-256/CRC32 verification, AES-128-CTR crypto),
  `HoroEngine::Foundation`, `HoroEngine::Platform`, and minimal asset compilation
  adapters. **`CliHost` is the sole argv parser for `horopak`; no second CLI
  implementation exists.**
- **Invariants**:
  - Strictly forbidden from linking `HoroEngine::Gui`, `HoroEngine::RenderFrontend`,
    `HoroEngine::RenderApi`, `HoroEngine::RuntimeScene`, `HoroEngine::GameplayRuntime`,
    `HoroEngine::Physics`, `HoroEngine::Audio`, or `HoroEngine::Networking`.
  - Cannot instantiate full scene pipelines or launch game runtime loops.
  - A bounded cold-start and memory budget suitable for high-throughput
    containerized asset packaging. Concrete thresholds require a release-build
    benchmark on each supported host profile.

## Host Model and Execution Pipeline

```text
argv / environment / stdin
          │
          ▼
┌──────────────────┐
│  CliOptionParser │ ──> Syntax diagnostics (Exit Code 2)
└─────────┬────────┘
          │ Typed Request
          ▼
┌──────────────────┐
│ CliCommandRegistry│ <── Validated Descriptors from built-in & domain modules
└─────────┬────────┘
          │ Resolved Descriptor & Adapter
          ▼
┌──────────────────┐
│  CliDispatcher   │ ──> Binds CliExecutionContext (CancellationToken, InvocationId, ICliProgressSink)
└─────────┬────────┘
          │ Dispatches request
          ▼
┌──────────────────┐
│ICliCommandAdapter│ ──> Calls Application Services / Domain Use Cases
└─────────┬────────┘
          │ Typed Result
          ▼
┌──────────────────┐
│CliOutputPresenter│ ──> Formats stdout (Pure Human/JSON/JSONL) & stderr (Logs/Progress)
└─────────┬────────┘
          │
          ▼
     OS Exit Code (0, 2..8, 10)
```

The CLI does not invoke GUI code or synthesize editor widget actions.

## Command Registry

Every CLI command is declared through a validated `CliCommandDescriptor`:

```cpp
struct CliCommandDescriptor {
    CommandPath path;
    std::string summary;
    OptionSchema options;
    CapabilitySet capabilities;
    OutputSchema output;
    InteractivePolicy interactive;
    HostAvailability hosts;
    ContractVersion contractVersion;
    SideEffectPolicy sideEffects;
    CancellationPolicy cancellation;
    TimeoutPolicy timeout;
    StdinPolicy stdinPolicy;
};
```

Command paths form a hierarchy:

```text
horo-engine project create
horo-engine project validate
horo-engine project restore
horo-engine scene validate
horo-engine asset import
horo-engine asset cook
horo-engine package restore
horo-engine package verify
horo-engine package cache list
horo-engine package cache clean
horo-engine save inspect
horo-engine save import
horo-engine save export
horo-engine save migrate
horo-engine save load
horo-engine save delete
horo-engine save resolve-conflict
horo-engine build
horo-engine release
horo-engine test
horo-engine console exec
horo-engine mcp serve
horopak inspect
horopak verify
```

Duplicate command paths and option names are startup errors. Help is generated
from the typed registry.

### Registry Activation Contract

`HoroEngine::CliHost` exposes the public descriptor and registry contracts under
`Horo/Cli/`. `CliCommandRegistry::Create` is the sole admission point for both
built-in and approved contributed descriptors. The composition root supplies an
active host, its granted capability set, the newest descriptor contract it
understands, and explicit metadata limits. Admission is atomic and inert: a
failure publishes no partial inventory and invokes no adapter or lifecycle code.

The current compatibility rule accepts the same contract major and descriptor
minor versions no newer than the host. Patch releases remain compatible within
an accepted minor. The registry rejects malformed or duplicate paths and options,
inconsistent option/output schemas, ungranted capabilities, and descriptors that
do not name the active host. These failures retain distinct stable `horo.cli`
error identities so composition diagnostics never depend on message text.

Accepted descriptors are copied, canonicalized, and sorted by hierarchical path.
`Commands`, `Find`, `Discover`, `DiscoverNextSegments`, and `GenerateHelp` read only that immutable
accepted collection; there is no handwritten help or completion inventory.
Options, enum alternatives, and capability requirements are also sorted so the
same contribution set produces byte-identical LF-delimited help and discovery
order on every supported platform. Registry construction and discovery remain
inert: parsing is an explicit invocation step, while dispatch, output presentation,
and adapter execution remain separate later stages.

### Save Command Boundary

Save commands are application adapters under
[ADR-116](../../adr/116-save-data-threat-model-and-trust-policy.md), not direct
filesystem or codec utilities. `inspect`, `import`, `export`, `migrate`, `load`,
`delete` and conflict resolution use distinct capabilities and host confirmation
policy. A command receives a typed save address or an explicitly admitted external
file handle; archive bytes cannot choose their destination root, trust policy or
credential binding.

Inspection applies fixed framing/parser/work budgets and returns bounded allowlisted
metadata plus safe diagnostics. It grants no load, migration or publication authority.
Raw participant export is development-only, requires a separate capability and a
validated output root, and is absent from shipping profiles unless the product
explicitly admits it. `horopak` never parses or mutates runtime `.horosave` files.

## Command Contributions & Adapter Equivalence

CLI commands are contributed through validated command descriptors and typed
command adapters (`ICliCommandAdapter`). Built-in modules, first-party tools,
and approved extension packages may contribute commands only through the
host-owned command registry.

```cpp
namespace Horo::Cli {

    /**
     * @brief Presentation-independent progress/event sink exposed to adapters.
     *
     * `CliOutputPresenter` implements this interface and converts events to the
     * active output mode (progress bar for human, JSONL record for jsonl).
     * Adapters must not write stdout or stderr directly.
     */
    class ICliProgressSink {
    public:
        virtual ~ICliProgressSink() = default;
        virtual void Report(const CliProgressEvent& event) = 0;
    };

    /**
     * @brief Invocation-scoped context provided to a CLI command adapter during execution.
     *
     * Adapters receive their domain-service dependencies by constructor injection at
     * registration time. This context carries only the values scoped to a single
     * command invocation: cancellation, identity, and progress reporting.
     */
    struct CliExecutionContext {
        const CancellationToken& cancellation;
        InvocationId             invocationId;
        ICliProgressSink&        progress;
        // ApplicationServices, JobSystem, ConfigurationSnapshot, and output writers
        // are not exposed here. Domain services are injected via the adapter constructor.
    };

    /**
     * @brief Interface implemented by domain modules to contribute CLI behavior.
     */
    class ICliCommandAdapter {
    public:
        virtual ~ICliCommandAdapter() = default;

        [[nodiscard]] virtual const CliCommandDescriptor& GetDescriptor() const noexcept = 0;

        [[nodiscard]] virtual Result<CliCommandResult> Execute(
            const CliCommandRequest& request,
            CliExecutionContext&     context) = 0;
    };

}  // namespace Horo::Cli
```

**Constructor injection** is the only approved mechanism for adapters to access domain
services:

```cpp
// Correct: domain services injected at construction, not discovered at invoke time.
class AssetCookCliAdapter final : public ICliCommandAdapter {
public:
    explicit AssetCookCliAdapter(IAssetCooker& cooker);

    Result<CliCommandResult> Execute(
        const CliCommandRequest& request,
        CliExecutionContext&     context) override;

private:
    IAssetCooker& m_cooker; // injected, not fetched from ApplicationServices
};
```

### Adapter Equivalence Contract

GUI, CLI, and MCP adapters must call the same application use cases for the same
business operation. Differences are limited to:

- input parsing and validation envelope
- presentation format
- transport/protocol error envelope
- interactive prompting policy
- progress delivery mechanism

Domain modules (`AssetCooker`, `ProjectService`, `TestRunner`, `ReleasePipeline`)
own use-case logic, transactions, and state invariants. They must never place
business logic, scene mutation paths, asset import logic, or build algorithms
inside CLI handlers.

[ADR-106](../../adr/106-navigation-bake-ownership-transaction-and-cache.md)
applies this equivalence contract to `horo-engine navigation bake`. The CLI
descriptor parses a project-contained definition/scope, grounded profile set,
target/profile and output mode, then invokes the application-owned
`NavigationBakeService`. It may join/observe an existing identical operation or
wait through the CLI host's bounded operation pump; it never calls a provider
builder, chooses a workspace path, writes a cache/generation, owns a lock or
implements supersession/recovery. Text, JSON and JSONL are presentations of the
same operation terminal result used by GUI, MCP and release cook.

## Separation of Concerns: CLI, Debug Console, and MCP

CLI commands, Runtime Debug Console commands (`DBG-001`), and MCP tools (`MCP-001`)
address different interaction paradigms and maintain distinct registries:

```text
                  ┌──────────────────────────────────────────────┐
                  │            Shared Domain Services            │
                  │  (AssetCooker, ProjectService, TestRunner,   │
                  │   ReleasePipeline, DebugConsoleService)      │
                  └──────▲────────────────▲──────────────▲───────┘
                         │                │              │
                         │                │              │
          ┌──────────────┴──────┐  ┌──────┴──────┐  ┌────┴─────────────┐
          │ ICliCommandAdapter  │  │ IDebugCmd   │  │ McpToolAdapter   │
          └──────────────▲──────┘  └──────▲──────┘  └────▲─────────────┘
                         │                │              │
     ┌───────────────────┴──────┐  ┌──────┴──────┐  ┌────┴─────────────┐
     │   CliCommandRegistry     │  │ DebugConsole│  │ McpController    │
     │   (HoroEngine::CliHost)  │  │ (Runtime)   │  │ (HoroEngine::Mcp)│
     └───────────────────▲──────┘  └──────▲──────┘  └────▲─────────────┘
                         │                │              │
                    Terminal argv    In-Game Console   AI JSON-RPC
```

1. **CLI Registry (`CliCommandRegistry`)**:
   - Shell argument syntax (`--flag`, positional arguments, `--` termination).
   - Stdin streaming policies, human vs JSON/JSONL output formatting, and OS exit codes.
2. **Debug Console (`DebugConsole`)**:
   - In-game console syntax, cvar read/write, autocomplete, in-game terminal overlay,
     game pause/step integration, product-profile gating (shipping vs dev).
3. **MCP (`McpController`)**:
   - JSON-RPC 2.0 protocol over stdio/SSE with JSON Schema parameter definitions,
     structured tool responses, and LLM context payloads.

### Delegation Seams

- **Debug Console Delegation**:
  The CLI does not duplicate runtime console commands. `horo-engine console exec "<cmd>"`
  (or `--exec-console="<cmd>"`) delegates execution to `DebugConsoleService` /
  `IDebugConsoleHost` on an initialized headless or connected instance, respecting
  console permissions and product profiles.
- **Headless MCP Serve**:
  `horo-engine mcp serve` initializes the headless `McpServer` over stdio or SSE.
  The CLI ensures strict stream isolation: JSON-RPC communication on `stdout` is
  isolated from engine diagnostic logging (routed to `stderr`).

## Parsing

Parsing produces a typed request or a list of diagnostics. It does not start
application work.

Rules:

- unknown options fail
- missing required values fail
- enum and numeric ranges validate before execution
- `--` terminates option parsing
- response/config files require an explicit supported format
- paths are normalized by the platform adapter
- credentials are never accepted in positional arguments
- external save inputs are untrusted and use the same bounded application admission
  pipeline as GUI/MCP; a canonical save-directory path does not skip verification

Common options such as project, output format, logging, and non-interactive mode
use one shared descriptor inventory. The current shared names are `--project`
(`-p`), `--output` (`-o`), `--log-level`, and `--non-interactive`; command
registration rejects any local long-name or short-name collision with that inventory.

`CliOptionParser` performs a bounded, allocation-visible pre-dispatch pass. It
resolves the longest registered command path, accepts long and single-character
short options, produces typed values for flags, strings, integers, finite
floating-point numbers, closed enums, and platform-normalized paths, then binds
ordered typed positionals. Grouped short options are deliberately unsupported.
Unknown commands, malformed values, duplicates of non-repeatable options, missing
required values, conflicting schemas, and extra positionals produce bounded
structured diagnostics without including argument contents.

## Configuration

CLI options participate in the precedence defined by
[Configuration System](../foundation/configuration-system.md). Explicit command options
override environment and persisted configuration only for keys the command is
allowed to control.

The parser never reads environment variables or persisted files. The composition
root supplies an immutable `ConfigurationSnapshot` already resolved by the
configuration system. Invocation values win over that snapshot; when invocation
input is absent, only a descriptor-declared configuration key is consulted, and
the exact `ConfigurationSource` provenance is retained. Descriptor defaults are
the final fallback. A type-incompatible resolved value fails parsing rather than
being coerced or silently ignored.

The effective safe configuration and its provenance may be shown with a
diagnostic command. Secret values are never printed.

## Output Modes

Canonical modes:

- `human`: concise text and terminal-aware progress
- `json`: one valid JSON result document
- `jsonl`: streaming records for commands that declare a streaming schema

In structured modes:

- stdout contains only schema-valid command output
- logs and diagnostics intended for humans use stderr
- colors and terminal control sequences are disabled
- field names and enum values are versioned contracts
- partial failure is represented structurally

Commands do not silently change schema based on TTY presence. TTY detection may
change presentation only in human mode.

### Output Mode Contract

| Mode | `stdout` | Progress / events |
|:---|:---|:---|
| `human` | Final human-readable payload | TTY: live progress bar on `stderr`. Non-TTY: rate-limited phase updates on `stderr`. |
| `json` | **Single** final JSON envelope only. Never emits partial or streaming records. | Not emitted on `stdout`. Optional human-readable progress on `stderr`; machine consumers must ignore `stderr`. |
| `jsonl` | One JSON object per line: zero or more progress/event records, then one terminal result record. | Events appear on `stdout` as JSONL; no ANSI sequences. |

`--output=json` on a long-running operation (e.g. `horo-engine asset cook --output=json`)
yields **one** envelope at completion. Machine consumers that need incremental progress must
use `--output=jsonl`.

### Output Ownership

Final result and streaming progress have distinct owners. `CliOutputPresenter` is
the only component that writes to `stdout` or `stderr`:

```text
Final payload:
  adapter -> CliCommandResult -> CliOutputPresenter -> stdout

Streaming progress/events:
  adapter -> ICliProgressSink -> CliOutputPresenter -> stdout (jsonl) or stderr (human)
```

Adapters never write `stdout` or `stderr` directly. `ICliOutputWriter` is not
part of the adapter contract.

## Structured Output Envelope

Structured output modes use a stable top-level envelope unless a command
explicitly declares a different streaming schema.

```json
{
  "schemaVersion": 1,
  "command": "project.validate",
  "invocationId": "cli-...",
  "status": "succeeded",
  "result": {},
  "diagnostics": [],
  "error": null,
  "metadata": {
    "durationMs": 123,
    "projectId": "..."
  }
}
```

Rules:

- `schemaVersion`, `command`, `invocationId`, and `status` are always present.
- `result` is command-specific and follows the command's declared output schema.
- `error` uses the canonical Horo serialized error shape.
- `diagnostics` contain safe, structured diagnostics.
- Human logs, progress decorations, prompts, and ANSI control sequences never
  appear in structured stdout.
- New optional fields may be added only under a versioned compatibility policy.
- Commands that return JSONL streams declare a per-record schema and emit the
  same envelope as the final summary record unless they choose a declared
  streaming schema.

## Progress

Long-running operations expose progress from the authoritative job store.

Human TTY mode may render a live progress line. Non-TTY human mode emits
rate-limited phase updates. Structured streaming mode emits declared progress
records only when requested.

Progress is bounded and never delays the operation because a consumer is slow.
The host polls `CliProgressProjection` for one correlated operation or job and
coalesces pending updates in `CliProgressMailbox`. A presentation loop drains the
mailbox at its own pace and applies `CliProgressCadence` for human TTY or non-TTY
output. `CliProgressPresenter` writes those coalesced updates on the presentation
thread: human updates go to stderr, JSON mode suppresses them, and JSONL updates
are encoded as progress records on stdout. Adapters may report bounded events
through the mailbox; they do not write to the output streams.
`CliProgressProjection` reads the authoritative operation
record first and uses the job record only before that operation is observed.

## Cancellation And Signals

### Signal Ownership Chain

```text
OS SIGINT / SIGTERM
   -> Platform signal bridge (HoroEngine::Platform)
   -> CancellationSource.cancel()
   -> CancellationToken
   -> adapter / domain use case
```

- **`HoroEngine::Platform`** owns signal registration and the typed bridge. Domain
  operations and adapters never install OS signal handlers.
- **`CliHost`** owns orchestration: it subscribes to the process `CancellationSource`,
  aborts in-flight dispatch, and maps cooperative cancellation to exit code `7`.
- Domain operations observe `CancellationToken` and stop at safe checkpoints.

### Cancellation Semantics

The first interrupt requests cooperative cancellation. A second interrupt within
a configured period may request forced host termination after emergency
diagnostics.

Cancellation:

- propagates to the command's task group
- terminates owned subprocesses through platform policy
- preserves transactional file guarantees
- returns the cancellation exit category

The CLI does not leave background jobs running after process exit.
The host creates one `CliInvocationStopController` per invocation, forwarding
platform interrupt events and host shutdown to it. Its first interrupt cancels
cooperatively; a repeated interrupt supplies a separate forced-termination token.
The dispatcher passes both tokens and the remaining descriptor deadline into
`CliExecutionContext`. A process-capable adapter receives `IExternalProcessRunner`
at the application composition boundary, caps its request timeout to
`RemainingTimeout()`, and passes `Cancellation()` and `ForceCancellation()` to
the runner. The platform runner owns termination and pipe draining. An adapter's
successful result or unrelated failure remains authoritative if cancellation
arrives late;
only an acknowledged cancellation is mapped to timeout when the deadline fired.

## Input Sources

Commands that read from stdin must declare an explicit `StdinPolicy` in their
descriptor.

Supported policies:

- `None`: stdin is ignored and never blocks command execution.
- `JsonDocument`: stdin contains one bounded JSON document.
- `JsonLines`: stdin contains bounded streaming JSONL records.
- `BinaryStream`: stdin contains binary input and requires a declared size or
  bounded streaming policy.

The current parser captures an explicitly selected payload once, enforces the
host byte limit before dispatch, validates complete JSON documents or every
non-empty JSONL record, and enforces a JSONL record-count limit. Binary mode is
opaque but remains byte-bounded. Malformed structured input produces a typed
parse failure with a `stdin` diagnostic location.

Commands must not accidentally block on stdin. In non-interactive mode, a
command may read stdin only when its descriptor declares it and the user selected
the matching input option. A command that declares `StdinPolicy::None` keeps
stdin unopened; it never calls the injected reader. A missing reader, an
unselected mode, or a selected mode that differs from the descriptor fails before
dispatch instead of probing ambient stdin.

## Interactive Input

Commands declare whether they may prompt. `--non-interactive` is supported by
all commands and is implied when no suitable terminal is available unless an
explicit input channel exists.

Prompts:

- are written to the terminal, not machine stdout
- provide deterministic alternatives through options
- never echo secrets
- fail with an actionable error when required input is unavailable

Release credentials use credential providers or protected input channels, not
ordinary command arguments.

## Exit Codes

Stable categories:

| Code | Category | Description |
|:---:|:---|:---|
| `0` | **Success** | Command completed successfully. |
| `1` | **Host Failure** | Uncaught host-level exception or pre-initialization runtime failure. |
| `2` | **Usage Error** | CLI syntax error, unknown flag, missing required argument, or invalid option value. |
| `3` | **Input Validation Failure** | Target project, file path, scene, or input payload failed domain validation. |
| `4` | **Capability Unavailable** | Required engine capability, host environment, or dependency is missing. |
| `5` | **Operation Failed** | The requested domain operation encountered a functional error (e.g. compilation error, build failure). |
| `6` | **Security / Permission Error** | Access denied, untrusted script execution, or unauthorized cvar/command execution. |
| `7` | **Cancelled / Interrupted** | Operation was cancelled cooperatively via `SIGINT`/`SIGTERM` or cancellation token. |
| `8` | **Timeout** | Operation exceeded its declared execution timeout. |
| `10` | **Internal Invariant Failure** | Engine bug or unexpected internal invariant violation detected in-process. Not a crash mapping — `SIGSEGV`, `SIGABRT`, and other fatal signals keep OS-native termination semantics and are not rewritten into `10`. |

Code `1` is reserved for legacy or host-adapter failures that occur before the
Horo error mapping layer is available, such as an uncaught host exception or a
failure to initialize the CLI runtime. Normal engine failures use the stable
categories listed above so scripts can distinguish generic process failure from
Horo's typed error domains.

Detailed failure identity remains in the structured Horo error code. Shell
scripts should not infer domain details from prose.

## CLI And Data Bus

Commands call use cases directly and receive typed results. They may subscribe
to process-level job or lifecycle notifications to refresh queries, but do not
publish command requests through `EngineDataBus`.

One-shot commands stop subscriptions before application services shut down.

### Capability-Scoped Dispatch Contract

`CliDispatcher` owns accepted adapters and binds one `CliExecutionContext` per
validated request. Activation is atomic: an adapter descriptor must exactly
match the accepted registry descriptor, its constructor-bound capability list
must exactly match the descriptor requirements, and every requirement must be
present in the host grant set. Host availability and side-effect authority are
also checked before an adapter can observe the request.

The execution context exposes only the immutable configuration snapshot,
cooperative cancellation/deadline observation, exact declared capability
identities, bounded progress reporting, and invocation correlation. It does not
expose `ApplicationServices`, a mutable engine object, `JobSystem`, an output
writer, or a capability lookup returning service objects. Concrete adapters
receive narrow application use-case interfaces through constructor injection;
GUI and MCP adapters call those same interfaces.

Invocation, operation, job, and application-approved safe project identities
are copied into the terminal envelope on both success and failure. Progress and
typed result fields have explicit count and byte bounds. An adapter invocation
is a structured one-shot scope: it releases subscriptions and joins or cancels
all child work before returning. The composition root destroys the dispatcher,
and therefore its owned adapters, before the injected application services.

## Observability

CLI logs use stderr and the common structured schema. Each command establishes
operation context containing command path, invocation ID, project ID when safe,
and job ID.

Arguments are redacted before logging. Diagnostic bundles may include the safe
effective command configuration but not credentials or arbitrary environment
variables.

## Migration from Legacy Ad-Hoc Parsing

The migration path removes legacy ad-hoc parsing in `apps/horo-engine/main.cpp`
without maintaining competing sources of truth:

1. **Foundation**: Implement `HoroEngine::CliHost` with `CliCommandDescriptor`,
   `CliCommandRegistry`, and `CliOptionParser`. The descriptor, registry, typed
   parsing, configuration provenance, explicit input policies, and parser-level
   interactive admission portions are complete. Executable composition still
   uses the legacy entry point until the dispatch and presentation stages exist.
2. **Dispatch & Adapters**: `CliDispatcher`, `CliExecutionContext`, typed terminal
   correlation, and the constructor-injected application adapter seam are implemented.
   Migrate built-in commands (`--emit-observability-smoke`, `--diagnostic-bundle`)
   into typed `ICliCommandAdapter` registrations:
   `CommandPath{{"observability","smoke"}}`, `CommandPath{{"diagnostics","bundle"}}`.
   User-facing syntax: `horo-engine observability smoke`, `horo-engine diagnostics bundle`.
3. **Structured Streams & MCP Serve**: Implement JSON/JSONL output presenters,
   cancellation hooks, and headless `horo-engine mcp serve` composition.
4. **Host Switchover**: Replace `apps/horo-engine/main.cpp` entry point with
   `Horo::Cli::CliHost::Run(argc, argv)` and delete legacy `ParseOptions`.

## Testing

Required tests cover:

- registry uniqueness, conflict rejection, and generated help
- parser success, enum/range validation, and diagnostic failures
- option/configuration precedence and provenance
- stdout purity in JSON and JSONL modes (zero logs or control sequences)
- stable exit-code mapping across all 10 categories
- TTY and non-TTY progress behavior
- cancellation, signals, and subprocess termination
- non-interactive prompt failure and fallback
- redaction of arguments and credentials in logs and diagnostics
- headless commands executing without GUI or renderer targets
- equivalence of GUI, CLI, and MCP use-case results
- save inspect/import/export/migrate/load/delete/resolve-conflict capability
  separation, bounded hostile input, shipping-profile denial and raw-state/path/
  credential redaction
- `horopak` isolation (absence of GUI/RenderApi linkage)

## Related Documents

- [ADR-019: CLI Host, Command Ownership, Adapter Equivalence and horopak Boundary Decision](../../adr/019-cli-host-command-ownership-adapter-equivalence-and-horopak-boundary.md)
- [ADR-004: CLI / Core / GUI Boundary](../../adr/004-cli-core-gui-boundary.md)
- [System Design](../foundation/system-design.md)
- [Error And Diagnostics](../foundation/error-and-diagnostics.md)
- [Configuration System](../foundation/configuration-system.md)
- [Concurrency And Job System](../foundation/concurrency-and-jobs.md)
- [Save Game And Persistence](../runtime/save-game-and-persistence.md)
- [ADR-116: Save Data Threat Model and Trust Policy](../../adr/116-save-data-threat-model-and-trust-policy.md)
- [MCP Architecture](./mcp-architecture.md)
- [Runtime Debug Console And Development Overlays](../runtime/debug-console-and-overlays.md)
- [Application Security](../security/application-security.md)
