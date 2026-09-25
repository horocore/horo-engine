# Logging, Context, And Diagnostics Contract

## Purpose

This document defines the detailed logging, diagnostic-context, persistence,
privacy, and support-diagnostics contract for Horo Engine.

Start with [Observability Architecture](./observability.md) for the decision
summary. CPU/memory/frame metrics and profiler captures are defined in
[Metrics And Profiling Contract](./observability-performance.md).

The logging system makes failures diagnosable without turning normal execution
into a formatting or I/O workload. Disabled verbose logging does not evaluate
expensive arguments, construct records, allocate, acquire sink locks, or perform
I/O.

## Goals

The logging system provides:

- one structured record model across C++ and Python
- consistent levels, categories, timestamps, and source identity
- editor, CLI, build-tool, release-job, and standalone-game logging
- low-cost disabled `Trace` and `Debug` call sites
- bounded in-memory and asynchronous storage
- rotating persistent files in platform-correct user locations
- mapped diagnostic context across nested and parallel operations
- safe startup, shutdown, crash, and unclean-exit diagnostics
- redaction and privacy controls
- user-approved diagnostic bundles for support and debugging

The logging system is not:

- an event bus or command dispatcher
- a replacement for typed errors and structured diagnostics
- a store for secrets, project assets, or arbitrary environment dumps
- a guarantee that every low-severity record survives overload or a crash
- a network telemetry uploader
- a metrics store, profiler timeline, or production analytics system

Remote telemetry or automatic upload requires a separate privacy and product
decision. Local logging works without network access.

## Record Pipeline

All C++ and Python producers follow the same conceptual pipeline:

```text
typed log call
    |
    v
compile-time level gate
    |
    v
runtime level/category gate
    |
    v
merge inherited diagnostic context and record-local fields
    |
    v
redact and normalize sensitive values
    |
    v
build one immutable structured record
    |
    +-- bounded asynchronous file delivery
    +-- console/debugger rendering
    +-- bounded StructuredLogStore
    +-- emergency high-severity path
```

Filtering happens before formatting and record construction. Redaction happens
before fan-out, so every sink observes the same safe record. Sinks may render or
retain records differently, but they do not reinterpret severity, category,
context, diagnostic identity, or privacy policy.

The asynchronous dispatcher is an implementation detail of delivery, not the
owner of records or application state. Producers never wait for all sinks to
finish during normal execution.

## Host Model

Every process creates one `ObservabilityRuntime` before initializing application
services:

```text
HoroEditor process
    |
    +-- ObservabilityRuntime
            +-- console/debugger sink
            +-- rotating JSONL file sink
            +-- StructuredLogStore
            +-- emergency/crash sink

horo-engine / horopak process
    |
    +-- ObservabilityRuntime
            +-- stderr sink
            +-- optional rotating file sink

packaged game process
    |
    +-- ObservabilityRuntime
            +-- platform debugger sink in development profiles
            +-- rotating game file sink

Python developer or CI process
    |
    +-- Python logging bootstrap
            +-- stderr handler
            +-- optional rotating JSONL handler
```

The process composition root owns initialization, configuration, flushing, and
shutdown. Domain modules receive or use the stable logging facade; they do not
create sinks or choose file paths.

Editor and game processes never write into the same active file. A
play-in-editor runtime adds its own `runtime.instance_id` while remaining in the
editor process. A separately launched game owns a separate game log session.

## Runtime Lifecycle

Observability starts before normal application services and remains available
until their producers have stopped:

1. Install a minimal bootstrap/emergency sink with a bounded preallocated
   buffer.
2. Parse logging configuration and establish the process/session identity.
3. Resolve the platform log root and create the session directory.
4. Install the configured console, file, and in-memory sinks.
5. Replay buffered bootstrap records in original sequence order.
6. Emit the structured startup/system snapshot.
7. Start application, editor, CLI, pipeline, or game services.

Shutdown reverses ownership:

1. Stop accepting new application work.
2. Join or cancel producers that can still emit records.
3. Emit the terminal session state and clean-shutdown marker.
4. Drain the asynchronous queue within a bounded timeout.
5. Flush and destroy normal sinks.
6. Keep the emergency sink valid until final process teardown.

Opening a log file, collecting optional system metadata, or starting an
asynchronous sink may fail independently. Available sinks remain usable, and
the emergency sink records the degraded configuration without recursively
calling the normal logger.

## Module Boundary

The target source layout is:

```text
src/
  foundation/
    observability/
      LogLevel.h
      LogRecord.h
      LogContext.h
      Logger.h
      Sink.h
      StructuredLogStore.h
      Redaction.h
  platform/
    observability/
      LogDirectories.h
      ConsoleSink.h
      RotatingFileSink.h
      EmergencySink.h
  application/
    diagnostics/
      DiagnosticBundleService.h
      StructuredLogEventAdapter.h
  editor/
    state/
      ConsolePresentationState.h
scripts/
  horo_logging.py
```

The public C++ API is Horo-owned. A mature logging library such as `spdlog` may
implement formatting, sink fan-out, rotation, and asynchronous delivery behind
the facade, but its types do not appear in public engine headers. This keeps
categories, context propagation, record schema, and privacy policy under Horo's
control.

Python tooling uses the standard-library `logging` package. Long-running or
parallel tools use `QueueHandler` and `QueueListener`; contextual fields use
`contextvars`. Python does not invent a second level model or file format.

`StructuredLogStore` is a renderer-neutral in-memory sink in the foundation
observability module. Editor, test, and optional game hosts may compose it;
headless tools may omit it. `StructuredLogEventAdapter` belongs above
foundation because it translates store revisions into process notifications.
The Console tab depends on a query capability, not on sink internals.

The editor host composes a 4096-record `StructuredLogStore` alongside the
terminal and JSONL sinks. Its overwrite-oldest retention is process-wide and
thread-safe. The Console tab queries immutable revision snapshots, applies
presentation-only severity and text filters, and stops following the tail while
the user is inspecting older rows. These filters never change producer or
persistent-sink level policy.

Engine and project code do not write diagnostic output directly through
`printf`, `std::cout`, `std::cerr`, ad hoc files, or independent
`logging.basicConfig()` calls. Bootstrap, emergency signal handling, and a
command's declared stdout result are the narrow exceptions. Third-party library
logging is adapted into stable Horo categories at the integration boundary and
must not configure process-global sinks independently.

## Log Levels

The canonical levels are:

| Level | Meaning | Typical examples |
|---|---|---|
| `Trace` | Fine-grained execution flow useful for focused diagnosis | function phases, cache decisions, protocol frame boundaries |
| `Debug` | Developer-facing state and decisions | selected backend, resolved path category, job transition |
| `Info` | Normal lifecycle milestones meaningful to operators | startup complete, project opened, build started |
| `Warn` | Recoverable unexpected condition or degraded behavior | fallback selected, retry, dropped low-priority records |
| `Error` | Operation failed but process can continue | import failed, save failed, tool exited unsuccessfully |
| `Critical` | Process or essential subsystem cannot continue safely | unrecoverable initialization failure, invariant breach |
| `Off` | Disable a sink or category | testing and explicit configuration only |

Rules:

- `Trace` describes flow; it must not be required to understand a normal error.
- `Debug` records decisions and relevant state, not every loop iteration.
- `Info` is sparse enough to remain enabled in editor and game releases.
- `Warn` is actionable or explains degraded behavior. Expected user validation
  failures are typed results, not automatically warnings.
- `Error` corresponds to a failed operation and includes a stable diagnostic
  code when one exists.
- `Critical` records severity but does not itself call `abort()`. The owning
  lifecycle code decides whether to terminate after logging and flushing.
- `Logger::WriteEmergency` bypasses severity filters and queued sinks, writes
  and flushes a bounded emergency record to standard error, and has no result
  value. The assertion boundary uses it before terminating; ordinary logging
  calls never determine operation control flow.
- The same condition is logged once by the layer that owns the failure. Higher
  layers add context only when they convert, recover, retry, or present it.

## Categories

Every record has a stable hierarchical category:

```text
editor.workspace
editor.modal.settings
scene.serialization
asset.import
asset.cook
renderer.opengl
renderer.vulkan
pipeline.build
pipeline.release
mcp.protocol
platform.process
python.dev
```

Categories describe the owning subsystem, not a source filename. Filters match
exact categories or prefixes. Renaming a file does not rename its log category.

Category ownership follows module ownership. Feature code must not log under a
generic `app`, `misc`, or `debug` category when a stable subsystem name exists.

## Instrumentation Expectations

New application use cases, editor workflows, background jobs, and external
integrations include enough logging to reconstruct their important decisions.
At minimum:

- `Info` marks significant process, project, scene, build, release, and runtime
  lifecycle boundaries.
- `Debug` records state-machine transitions, selected strategies/backends,
  fallback decisions, cache outcomes, normalized identifiers, and operation
  duration.
- `Trace` records focused internal phases, queue handoffs, dependency traversal,
  and protocol boundaries where deeper diagnosis is useful.
- `Warn` records retries, degradation, dropped low-priority data, and successful
  recovery from an unexpected condition.
- `Error` records the owning operation failure with its diagnostic code and
  active operation context.

Asynchronous work logs submission, start, terminal state, cancellation, and
duration under one operation context. External process, filesystem, network,
renderer, and serialization boundaries log enough metadata to identify the
boundary without dumping sensitive or unbounded payloads.

ADR-136 Platform Offline Queue instrumentation distinguishes durable admission,
pending/dispatching/suspended/reconciling and explicit terminal disposition. It may
report aggregate counts/bytes, bounded age/delay, capacity pressure and normalized
failure by service/state. It never labels queued/expired work successful, combines
Save cloud-journal depth with the Platform queue, or emits subject partitions,
mutation/definition IDs, payload/presence text, raw provider identity or credentials.

Empty catch blocks and silent fallback paths are not allowed. A fallback that is
part of normal expected selection may be `Debug`; a fallback caused by a failed
preferred path is normally `Warn`.

Instrumentation remains proportional. Hot render, physics, audio, and gameplay
loops use counters, sampled summaries, or explicitly enabled trace categories
instead of unconditional per-frame records.

## Structured Record

All sinks consume the same immutable `LogRecord`:

```cpp
struct LogRecord {
    uint32_t schemaVersion = 1;
    LogSequence sequence;
    std::chrono::system_clock::time_point timestampUtc;
    std::chrono::steady_clock::time_point monotonicTime;

    LogLevel level;
    LogCategory category;
    std::string message;
    StructuredFields fields;
    LogContext context;

    ProcessRole processRole;
    ProcessId processId;
    ThreadId threadId;
    std::string threadName;

    ApplicationIdentity applicationIdentity;
    EngineIdentity engineIdentity;
    BuildIdentity buildIdentity;

    std::optional<SourceLocation> source;
    std::optional<DiagnosticCode> diagnosticCode;
};
```

Required persistent fields include:

- schema version
- UTC timestamp with sub-second precision
- process-local monotonic timestamp or elapsed duration
- process-local sequence number
- level and category
- rendered message
- process role, process ID, thread ID, and optional thread name
- active diagnostic context
- application, engine, and build identity attached by the runtime
- source file, line, and function according to level/profile policy
- stable diagnostic code when the record represents a known failure

JSON Lines (`.jsonl`) is the canonical persistent format: one complete JSON
object per line. It supports streaming, partial-file recovery, command-line
processing, and schema evolution. Human console output is a rendering of the
same record and is not the source of truth.

Example:

```json
{
  "schemaVersion": 1,
  "timestamp": "2026-06-14T12:42:31.194Z",
  "elapsedMs": 1842.37,
  "sequence": 418,
  "level": "debug",
  "category": "asset.import",
  "message": "Resolved texture dependency",
  "process": {
    "role": "editor",
    "pid": 48120
  },
  "thread": {
    "id": "worker-3",
    "name": "asset-import"
  },
  "identity": {
    "application": {
      "name": "HoroEditor",
      "version": "0.9.0"
    },
    "engine": {
      "name": "Horo Engine",
      "version": "0.9.0"
    },
    "build": {
      "id": "20260614.3",
      "configuration": "RelWithDebInfo",
      "sourceRevision": "abc123"
    }
  },
  "context": {
    "session.id": "01J...",
    "project.id": "8ee...",
    "operation.id": "01J...",
    "asset.id": "textures/wall"
  },
  "fields": {
    "dependency.count": 4,
    "cache.hit": true
  },
  "source": {
    "file": "TextureImporter.cpp",
    "line": 217,
    "function": "ResolveDependencies"
  }
}
```

Structured fields preserve booleans, integers, floating-point values, strings,
durations, and stable IDs as typed values. Important searchable data must not be
embedded only inside the message text.

## Startup And Session Records

The first records of every persistent session describe the environment needed
for diagnosis:

- product and process role: editor, game, CLI, packager, Python tool, or test
- engine version, application/game version, build ID, and source revision
- build configuration and compiled logging level
- executable architecture and operating-system version
- process ID, parent process ID, locale, timezone, and startup timestamp
- CPU architecture and logical core count
- available memory class or bucket, not unrelated machine inventory
- renderer backend, GPU/driver identity, and graphics API version when relevant
- project ID and project version when a project is open
- session ID and previous-session clean/unclean shutdown state
- active log configuration and sink destinations

Startup metadata is emitted as structured records, not one multi-line string.
Collection failure for one field does not prevent logging startup.

The startup snapshot must not contain:

- passwords, tokens, credentials, authorization headers, or private keys
- a full environment-variable dump
- username, email address, machine hostname, or hardware serial number
- raw provider/platform account identifiers, generation-scoped subject/social handle
  bytes, gamertags, friends graphs, avatar locators, or presence free text
- full home-directory paths when a normalized or project-relative path suffices
- project assets, source contents, or arbitrary command arguments

Every clean shutdown writes a final session record and flushes sinks. At the
next launch, a missing clean-shutdown marker identifies the previous session as
potentially crashed or forcibly terminated.

## Mapped Diagnostic Context

Mapped diagnostic context (MDC) attaches stable key/value fields to every record
in a logical operation:

```text
[session.id=...] [project.id=...] [operation.id=...] [release.job_id=...]
```

Human sinks may render fields in brackets. JSON sinks store them in the
`context` object.

Recommended keys include:

- `session.id`
- `project.id`
- `scene.id`
- `runtime.instance_id`
- `operation.id`
- `operation.parent_id`
- `request.id`
- `mcp.request_id`
- `release.job_id`
- `release.stage`
- `asset.id`
- `document.id`
- `test.id`

Keys are stable, lowercase, and namespaced with dots. Context values remain
small and must be safe to repeat on every record.

### Context Versus Record Fields

MDC and record-local fields solve different problems:

| Data | Lifetime | Example |
|---|---|---|
| Diagnostic context | Inherited by the logical call/task subtree | `operation.id`, `request.param`, `release.job_id` |
| Record field | Attached only to one log record | `duration_ms`, `retry.count`, `cache.hit` |

A value belongs in context when it should follow `Function1 -> Function2 ->
Function3` and remain present on every relevant record. A value belongs in
record fields when it describes only the event being recorded.

Context is immutable after binding. Nested operations derive a new context
instead of mutating a shared map. Context must not contain large objects,
collections, mutable status, secrets, or values that change on every loop
iteration. Record-local fields must not be used as a substitute for correlation
IDs that downstream diagnosis needs on later records.

### C++ Context

`LogContext` is an immutable value that can be copied cheaply and extended:

```cpp
const LogContext importContext =
    CurrentLogContext()
        .With("operation.id", operationId)
        .With("asset.id", assetId);

{
    ScopedLogContext bind(importContext);
    HORO_LOG_DEBUG("asset.import", "Import started");
    ImportDependencies(importContext, request);
}
```

`ScopedLogContext` uses RAII to bind context for synchronous calls and restores
the previous context on destruction. Nested scopes may add fields. A nested
field with the same key overrides the parent value only for that scope.

Thread-local storage alone is insufficient. Work submitted to a worker pool,
coroutine scheduler, process runner, or callback queue captures the current
`LogContext` and binds it while executing:

```cpp
jobs.Submit(CaptureLogContext(),
            [request](const LogContext& context) {
                ScopedLogContext bind(context);
                RunImport(request);
            });
```

Parallel children derive a child `operation.id` and retain
`operation.parent_id`. They never mutate one shared MDC map. This makes
interleaved logs attributable even when multiple imports, builds, or MCP
requests run on the same worker threads.

### Cross-Process Session Links

When Horo starts a child process, Python tool, release helper, or external game
process, the parent records a session-link record and passes only allowlisted
correlation identifiers.

```cpp
struct SessionLinkRecord {
    SessionId parentSession;
    SessionId childSession;
    OperationId operation;
    ProcessRole parentRole;
    ProcessRole childRole;
    ChildProcessKind kind;
    std::optional<ReleaseJobId> releaseJob;
};
```

Rules:

- the child always creates its own `session.id`;
- the parent never injects the full MDC map;
- only allowlisted IDs such as `HORO_PARENT_SESSION_ID`,
  `HORO_OPERATION_ID`, and `HORO_RELEASE_JOB_ID` may cross the boundary;
- session links are included in diagnostic bundles when both sessions are
  selected;
- missing child logs are represented as missing, not as a failed parent
  operation unless the operation required them.

For example, two concurrent flows may bind different values for the same key:

```text
[operation.id=op-a] [request.param=1] Function1 started
[operation.id=op-b] [request.param=2] Function1 started
[operation.id=op-b] [request.param=2] Function2 completed
[operation.id=op-a] [request.param=1] Function2 completed
```

`Function1`, `Function2`, and `Function3` inherit the context of their logical
caller even if execution moves between worker threads. Reusing a worker thread
does not reuse the previous operation's context. Every task binding restores the
worker's earlier context when the task finishes.

### Python Context

Python uses `contextvars.ContextVar`, not `threading.local`, as the primary
logical context:

```python
from horo_logging import bind_context, get_logger

logger = get_logger("python.dev")

with bind_context(operation_id=operation_id, project_id=project_id):
    logger.debug("configuring build", extra={"preset": preset})
```

`asyncio` tasks inherit `contextvars` naturally. Work submitted to thread or
process executors explicitly uses `contextvars.copy_context()` or passes a
serialized allowlist of context fields. Arbitrary parent environment variables
are not copied to create context.

### Process Boundaries

Approved correlation fields may cross a child-process boundary through a typed
process request or a minimal allowlisted environment:

```text
HORO_PARENT_SESSION_ID
HORO_OPERATION_ID
HORO_RELEASE_JOB_ID
```

The child creates its own `session.id` and records the received parent
identifiers. Context propagation never includes secrets or an unrestricted MDC
map.

## C++ Logging API

The Horo facade provides:

```cpp
HORO_LOG_TRACE(category, format, ...);
HORO_LOG_DEBUG(category, format, ...);
HORO_LOG_INFO(category, format, ...);
HORO_LOG_WARN(category, format, ...);
HORO_LOG_ERROR(category, format, ...);
HORO_LOG_CRITICAL(category, format, ...);
```

The narrowly scoped macros are justified because they:

- capture source location
- remove compiled-out levels completely
- avoid evaluating format arguments when a level/category is disabled
- preserve compile-time format-string validation

Macros forward immediately to typed implementation functions and contain no
business behavior.

The runtime guard is part of the macro expansion, before the format arguments
appear in an evaluated function call. Conceptually:

```cpp
#define HORO_LOG_DEBUG(category, format, ...)                           \
    do {                                                                \
        if constexpr (IsLogLevelCompiled<LogLevel::Debug>) {            \
            if (Logger::IsEnabled(LogLevel::Debug, category)) {         \
                Logger::Write(LogLevel::Debug, category,                \
                              std::source_location::current(),           \
                              format __VA_OPT__(,) __VA_ARGS__);         \
            }                                                           \
        }                                                               \
    } while (false)
```

The production macro implementation may differ, but it preserves these
evaluation semantics. Putting the check only inside `Logger::Write()` is
incorrect because C++ evaluates function arguments before entering the
function.

Structured fields use an explicit API:

```cpp
HORO_LOG_DEBUG_FIELDS(
    "pipeline.build",
    Fields{
        {"target", target.ToString()},
        {"cache.hit", cacheHit},
        {"duration_ms", duration.count()},
    },
    "Build step completed");
```

Call-site rules:

- use stable categories and diagnostic codes
- prefer IDs, counts, durations, enum names, and normalized paths
- do not construct a JSON string manually
- do not pre-format a message before calling the logger
- do not log every frame, entity, allocation, or loop iteration by default
- rate-limit repeated recoverable failures
- summarize batches instead of logging one record per item
- record operation boundaries and state transitions where they aid diagnosis

## Compile-Time And Runtime Filtering

Logging has two independent gates:

1. `HORO_COMPILED_LOG_LEVEL` removes lower levels at compile time.
2. Runtime configuration filters remaining records by level and category.

A disabled call follows this contract:

- compile-time disabled: no generated call, branch, argument evaluation,
  formatting, allocation, context copy, locking, or I/O
- runtime disabled: one inexpensive level/category check; no argument
  evaluation, formatting, record allocation, sink lock, or I/O

Runtime-selectable logging cannot be literally zero-cost because the process
must test whether the level/category was enabled after startup. The contract is
one predictable guard with no expensive work. Call sites that cannot tolerate
even that guard use a compile-time removed level, sampling, or a separately
enabled instrumentation block.

The logging facade must test the runtime gate before evaluating deferred format
arguments. An API that eagerly calls `std::format`, `fmt::format`, string
concatenation, or `ToString()` before `IsEnabled()` violates this contract.

Category filtering uses immutable runtime configuration and stable category
identities. `IsEnabled()` performs no string allocation, sink access, or global
sink lock. Reconfiguration builds a replacement filter snapshot and installs it
atomically; producers do not parse filter strings on the logging path.

Default build policies:

| Product profile | Compiled minimum | Runtime default |
|---|---|---|
| Local Debug/test | `Trace` | `Debug` |
| Local Release/profile | `Trace` | `Info` |
| Packaged HoroEditor | `Debug` | `Info` |
| Game Development | `Trace` | `Debug` |
| Game Profile | `Trace` | `Info` |
| Game Shipping | `Info` | `Info` |
| Diagnostics build | `Trace` | `Trace` for selected categories |

Packaged HoroEditor can enable `Debug` for a support session without a special
binary. `Trace` requires a local/profile or diagnostics build because
high-frequency trace call sites are removed from the standard editor package.
Game Shipping removes `Trace` and `Debug` completely.

When configuration requests a level below the compiled minimum, startup emits
one warning describing the effective level. It does not pretend that removed
records are available.

## Runtime Configuration

Configuration precedence is:

1. command-line options
2. environment variables
3. editor user settings or game logging configuration
4. product-profile defaults

Canonical command-line options:

```text
--log-level <level>
--log-level <category-prefix>=<level>
--log-format pretty|json
--log-dir <path>
--log-console auto|on|off
--log-file on|off
--log-source auto|on|off
```

`--log-level` may be repeated. The most specific category prefix wins; later
equally specific entries win.

Canonical environment variables:

```text
HORO_LOG_LEVEL=info
HORO_LOG_LEVELS=renderer=warn,asset.import=debug,pipeline.release=trace
HORO_LOG_FORMAT=pretty|json
HORO_LOG_DIR=/absolute/path
HORO_LOG_CONSOLE=auto|on|off
HORO_LOG_FILE=on|off
HORO_LOG_SOURCE=auto|on|off
```

Environment parsing is case-insensitive for level names. Accepted warning aliases
are `warn` and `warning`. Invalid values produce one startup warning and fall
back to the next valid configuration layer.

Examples:

```bash
# Run the editor with focused asset diagnostics.
HORO_LOG_LEVEL=info \
HORO_LOG_LEVELS=asset.import=trace,asset.cook=debug \
python3 scripts/dev.py run editor

# Diagnose renderer startup in a packaged editor.
HoroEditor --log-level info --log-level renderer=debug

# Diagnose game AI in a non-shipping profile build.
HORO_LOG_LEVEL=info \
HORO_LOG_LEVELS=game.ai=trace,game.save=debug \
build/profile/bin/MyGame

# Keep machine-readable CLI stdout clean; logs remain on stderr.
horo-engine project validate ./MyGame \
  --log-format json --log-level scene.serialization=debug

# Apply the same schema and context rules to Python tooling.
HORO_LOG_LEVEL=debug \
python3 scripts/generate-sbom.py
```

CLI commands reserve stdout for their declared result format. Logs go to stderr
unless an explicit file sink is selected.

Feature-specific switches such as MCP protocol diagnostics are category aliases,
not separate logging implementations. For example,
`HORO_MCP_LOG=1` may map to `mcp.protocol=trace` for compatibility, but records
still use the common runtime, redaction, sinks, and retention policy.

## Sinks And Delivery

The runtime fans one immutable record to configured sinks:

### Console And Debugger

- Local development uses colored human-readable console output.
- CLI and Python logs use stderr.
- Windows may additionally use the debugger output sink.
- Packaged GUI applications do not require an attached terminal.
- Console coloring is disabled when unsupported or when JSON output is selected.

### Structured Log Store

`StructuredLogStore` is a renderer-neutral bounded, queryable in-memory sink.
HoroEditor composes it as the data source displayed by `ConsoleTab`; tests and
games may compose separate instances when their host profile requires an
in-memory view.

The store:

- is thread-safe for producer append and main-thread query
- has a monotonic revision
- supports range queries and level/category/context filtering
- has explicit record and memory limits
- evicts oldest low-severity records first according to policy
- does not own persistent file retention

`StructuredLogEventAdapter` publishes only revision/count availability events.
It does not publish every log record through `EngineDataBus`. See
[Engine Data Bus](../foundation/engine-data-bus.md).

### Rotating File

Persistent logs use an asynchronous bounded queue and rotating JSONL files.

- `Trace`, `Debug`, and `Info` may be dropped under sustained queue saturation.
- The runtime emits a summarized dropped-record count when capacity recovers.
- `Warn`, `Error`, and `Critical` are never silently discarded. If the normal
  queue cannot accept them, they use the emergency synchronous sink.
- Records have process-local sequence numbers so dropped ranges and ordering can
  be diagnosed.
- Rotation never renames or deletes the active file while another process owns
  it.

### Emergency And Crash Path

The emergency sink is initialized early and has minimal dependencies. It writes
high-severity records when normal asynchronous delivery is unavailable.

Signal/exception handlers do not call the normal logger, allocate memory, take
arbitrary locks, or format complex records. They write only a minimal
platform-supported crash marker/tombstone using pre-established resources.
Normal startup detects the tombstone and associates it with the previous log
session.

## Storage Locations

Logs are stored in user-writable state/log directories, never beside an
installed executable or inside packaged application resources.

Canonical roots:

| Platform | Root |
|---|---|
| macOS | `~/Library/Logs/Horo/` |
| Windows | `%LOCALAPPDATA%\Horo\Logs\` |
| Linux | `${XDG_STATE_HOME:-~/.local/state}/horo/log/` |

Product layout:

```text
<log-root>/
  editor/
    latest
    sessions/
      2026-06-14T124120Z_<session-id>/
        session.json
        0001.jsonl
        0002.jsonl
        metrics.jsonl
        captures/
          2026-06-14T124512Z_<capture-id>/
            capture.htrace
            manifest.json
        shutdown.json
    crashes/
  cli/
    <tool-name>/
      sessions/
  games/
    <project-id>/
      sessions/
  release-jobs/
    <job-id>/
      pipeline.jsonl
      stages/
  diagnostics/
```

For repository-local development and CI, an explicit log directory may point to:

```text
build/<preset>/logs/<process-role>/
```

Tests use isolated temporary directories and never write into a developer's
normal application log directory.

Each process session owns one directory. `session.json` contains immutable
session identity and startup metadata; numbered JSONL segments contain log
records; `metrics.jsonl` contains optional bounded aggregates (defined in
[Metrics And Profiling Contract](./observability-performance.md#metrics-architecture));
capture directories contain explicitly requested profiler sessions (defined in
[Metrics And Profiling Contract](./observability-performance.md#profiler-traces));
`shutdown.json` exists only after a clean bounded flush. Rotation closes the
current segment and atomically creates the next segment without renaming an
active file.

`latest` is a convenience pointer to the current/most recent session. It may be
a symbolic link where reliable or a small atomically replaced file containing
the session-directory name. Session directories and numbered segments are
authoritative. Log files are created with permissions restricted to the current
user where the platform supports it.

## Rotation And Retention

Retention is bounded by file size, total bytes, session count, and age.
Defaults are:

| Product | Per-file limit | Session limit | Age limit | Total limit |
|---|---:|---:|---:|---:|
| HoroEditor | 25 MiB | 10 | 14 days | 250 MiB |
| Standalone game | 10 MiB | 5 | 7 days | 50 MiB |
| CLI/Python tool | 10 MiB | 5 per tool | 7 days | 50 MiB per tool |
| Release job | 25 MiB per stage | job-owned | job-history policy | job-owned |

The active session is never deleted. Cleanup runs after logging initialization
and in a bounded background task. A session associated with a crash may be
retained until its diagnostic bundle is generated or the extended crash
retention period expires.

Changing log level does not change retention automatically. A temporary verbose
support session records its increased-volume policy and remains bounded.

## Editor Logging

HoroEditor enables:

- rotating persistent JSONL logs
- an in-memory `StructuredLogStore`
- console/debugger output in development
- filtering and search in `ConsoleTab`
- a visible session ID and "Open Log Folder" action
- a "Generate Diagnostic Bundle" workflow

The Console tab is a view over `StructuredLogStore`; clearing the view does not
delete persistent files. Filters are presentation state and do not alter global
logger configuration unless the user explicitly changes logging settings.

Editor Settings may configure:

- default runtime level
- per-category overrides
- file and console sinks
- retention limits within product safety bounds
- source-location visibility
- privacy level

Verbose settings apply at a frame-safe boundary and publish a settings
notification. The logging runtime remains the authority for the effective
configuration.

## Game Logging

Every generated game initializes the same logging facade with:

- process role `game`
- project and game identity
- build profile
- renderer and platform metadata
- a game-specific persistent root

Game Development and Profile builds support focused `Trace`/`Debug` diagnosis.
Game Shipping keeps `Info`, `Warn`, `Error`, and `Critical`; lower levels are
compiled out.

Game code uses project-owned categories such as:

```text
game.gameplay
game.ai
game.network
game.save
```

Engine categories remain under their engine subsystem names. The game must not
replace or fork the engine logger.

Standard Game Shipping builds do not accept environment or command-line
configuration that weakens the packaged minimum level, enables source paths, or
redirects logs to an arbitrary path. A product may provide a signed or otherwise
authenticated diagnostics entitlement, but it cannot restore compiled-out
`Trace` or `Debug` call sites. Any support-session override is explicit,
time/session-bounded, visible to the user, restricted to an approved log root,
and subject to the same redaction rules.

User-facing game failures expose a session ID and stable diagnostic code where
the product has an error-reporting surface. A generated game may provide a
local, user-approved diagnostic-bundle command or screen using the same
allowlist and redaction service as the editor; automatic upload is still outside
this architecture.

## Python Logging

All repository Python entry points call one bootstrap before doing work:

```python
from horo_logging import configure_logging, get_logger

configure_logging(process_role="python-tool", tool_name="dev")
logger = get_logger("python.dev")
```

Python behavior:

- uses the canonical levels and category names
- maps Python `WARNING` to Horo `Warn`
- writes the canonical JSONL schema for persistent files
- sends human output to stderr
- uses UTC and monotonic timing
- includes process, thread, task, build, and active context fields
- formats exceptions into structured exception fields
- applies the same redaction and path-normalization policy
- uses a bounded queue for long-running or parallel tools
- flushes and stops its listener during normal exit

Python's custom `TRACE` level is numerically below `DEBUG` and is exposed through
the Horo wrapper. Tool code does not call `logging.basicConfig()` independently.
Python evaluates function arguments before calling the logger, so expensive
diagnostic values are guarded explicitly:

```python
if logger.isEnabledFor(TRACE):
    logger.trace("resolved graph", extra={"graph": build_expensive_graph_view()})
```

Subprocess wrappers capture stdout/stderr as bounded process output. They attach
the current operation context and log summaries or referenced output ranges;
they do not duplicate every child line at multiple levels.

## Release Job Logs

Release and build jobs maintain job-specific logs in addition to the process
session log.

Every job record includes:

- `release.job_id`
- target platform and architecture
- build configuration
- pipeline stage
- child-process identity
- diagnostic code
- artifact identity when available

The release service owns job-log retention and exposes typed range queries.
`BuildReleaseModal`, CLI, and MCP render/query the same records. Closing the
modal does not stop logging or destroy job history.

Child-process output is bounded, redacted, and associated with the stage that
launched it. Complete raw output is retained only when the stage policy permits
it; the global editor log contains summaries and references rather than an
unbounded duplicate.

The authoritative live job logs remain under the release-job log root owned by
`ReleaseService`. A release output's `logs/` directory, when present, contains a
bounded exported copy or summary for the producer/operator. It is not included
in the customer-facing game package unless an explicit developer-diagnostics
profile allows it.

See [Release Architecture](../release/release.md) and
[Release Security](../release/release-security.md).

## Diagnostic Bundles

HoroEditor provides a user-initiated diagnostic bundle for support and
post-failure analysis.

A bundle may include:

- selected current and previous session logs
- crash marker and available crash metadata
- structured startup/system snapshot
- engine, editor, game, build, and renderer identity
- active logging configuration
- the recent bounded CPU, memory, frame, and engine metric window
- profiler capture manifests and explicitly selected capture files
- allowlisted non-secret editor and project settings
- recent structured diagnostics
- release-job identifiers and selected job logs
- a manifest with file sizes and checksums

A bundle does not include by default:

- credentials, tokens, private keys, or authorization headers
- full environment variables
- project assets or source files
- raw scene contents
- unrestricted user paths
- unrelated logs from other games or projects

Before export, the editor displays the bundle contents, time range, approximate
size, included metrics/captures, and privacy level. Creation is local and does
not upload automatically. The bundle is written to a user-selected location
using an atomic temporary file and contains a schema/version manifest.

Every support-facing error can show or copy:

- session ID
- diagnostic code
- operation/request/job ID when available
- relevant log-file location

These identifiers let support find the correct interleaved records without
asking the user to reproduce a full machine state.

## Crash And Hang Diagnostics

Crash and hang diagnostics are local artifacts owned by the same observability
session model. They are not automatic telemetry uploads.

A crash record has:

- a random crash ID distinct from `session.id`
- the active session ID and previous clean-shutdown state
- process role, product profile, build identity, and platform identity
- fatal diagnostic code or signal/exception class when available
- crashing thread identity and best-effort thread summary
- recent high-severity log window and startup/session metadata references
- optional native crash dump or stack payload when the platform and build profile
  permit it
- user comment only when the product provides an explicit support UI

The crash path is split into two phases:

1. **Fault-time marker**: a minimal preallocated writer records the crash ID,
   signal/exception class, session ID, and dump path if one was created. It does
   not allocate, run normal logging, traverse arbitrary engine state, or upload.
2. **Next-launch association**: startup detects the marker, links it to the
   previous session directory, records whether shutdown was clean, and makes the
   crash available for diagnostic-bundle selection.

[ADR-048](../../adr/048-gpu-crash-and-device-loss-diagnostic-bundles.md) extends
this two-phase model for renderer incidents without weakening it. A process fault
adds only preallocated scalar renderer/device/frame context to the marker. A live
device loss may freeze bounded generation-scoped graph, marker, diagnostic,
capability and memory evidence plus admitted native fault queries before device
teardown. Bundle encoding remains asynchronous/next-launch and never gates
recovery or runs inside a signal/exception handler.

Completed structured renderer incident evidence may be selected in a support
bundle. Opaque native GPU fault/dump payloads are restricted developer data,
excluded by default and require a separate explicit confirmation because their
contents cannot be fully inspected or redacted. Missing/partial evidence remains
manifest coverage rather than an empty successful artifact.

Hang diagnostics use an explicit watchdog policy. A watchdog may record a
bounded heartbeat failure, main-thread phase, and recent operation IDs, but it
must not kill the process or collect a dump unless the active host/profile
declares that behavior. Long capture or dump collection is user-approved or
diagnostics-build-only.

Generated games may expose a local "copy support info" or "create diagnostic
bundle" action. The action shows crash/session IDs and bundle contents before
export. Upload, if a product later implements it, is outside this architecture
and requires a separate privacy, authentication, and endpoint policy.

## Redaction And Privacy

Observability output is classified before storage or export:

```cpp
enum class PrivacyLevel {
    PublicDiagnostic,
    ProjectScoped,
    UserLocal,
    SensitiveRedacted,
    Forbidden
};
```

Rules:

- `PublicDiagnostic`: safe IDs, versions, non-user-specific engine state.
- `ProjectScoped`: project IDs, project-relative paths, asset IDs.
- `UserLocal`: normalized local paths or machine-local state shown only to the
  current user.
- `SensitiveRedacted`: original value is never stored; rendered as `[REDACTED]`
  or a safe token.
- `Forbidden`: must not enter logs, metrics, captures, or bundles.

Diagnostic-bundle policies select the maximum allowed privacy level. Sink
formatters cannot downgrade or recover a more sensitive value.

Redaction happens before records enter any sink or in-memory store.

Required rules:

- dedicated sensitive value types format as `[REDACTED]`
- known secret keys and authorization formats are removed
- process command previews are built from redacted typed arguments
- environment dumps are prohibited
- user and project paths are normalized according to privacy policy
- URLs remove credentials and sensitive query parameters
- `Trace` and `Debug` have exactly the same privacy rules as higher levels
- sink formatters cannot recover the original sensitive value

Redaction is a backstop. Code must not pass secrets to the logger and assume a
regular expression will always catch them.

Rate limiting and sampling must preserve the first occurrence, a suppressed
count, and the latest occurrence for repeated warnings/errors. Security-relevant
records are not sampled away.

See [Release Security](../release/release-security.md).

## Failure And Backpressure Policy

Logging must not recursively log its own failure through the normal facade.
Internal sink failures go to the emergency sink and a bounded in-memory status.

Policies:

- file-open failure does not prevent the application from starting when another
  sink is available
- persistent-sink failure is visible through one warning/status indicator
- full queues shed low-severity records before high-severity records
- dropped-record counts are aggregated, not emitted once per drop
- slow or blocked disk I/O does not stall render or worker hot paths
- shutdown has a bounded flush timeout and reports incomplete flush
- logging failure never changes a successful domain result into failure unless
  the operation contract explicitly requires an audit log

The logger must avoid unbounded memory growth even when no sink can make
progress.

## Relationship To Errors And Data Buses

Typed results and diagnostics remain the application contract:

```text
operation fails
    |
    +-- returns Result<T, Error> to caller
    +-- records one structured log at owning boundary
    +-- updates authoritative job/model state
    +-- publishes a small state-change notification when appropriate
```

Logs are not used to communicate success or failure between modules.

`EngineDataBus` and `EditorDataBus` carry revision or availability
notifications after `StructuredLogStore` changes. They do not carry every log
record and are not required for file logging.

[ADR-041 renderer diagnostic events](../../adr/041-backend-neutral-renderer-diagnostics-model.md)
enter through a bounded generation-aware ingest port and become ordinary
immutable structured log records after registered-code, context, field-limit and
redaction validation. Renderer backends/providers do not configure sinks or own a
second store. Native callback threads never perform formatting, file I/O or UI
dispatch; repeated messages use bounded descriptor-declared aggregation. Metrics
and profiler channels retain per-frame numeric/timeline detail instead of emitting
one diagnostic record per draw, resource or frame.

Metrics and profiler bus boundaries are defined in
[Metrics And Profiling Contract](./observability-performance.md).

The editor can continue collecting logs while a modal owns all input focus.
Modal focus rules do not pause logs, metrics, or an active profiler capture.

## Security And Audit Records

Security-sensitive decisions are recorded as structured audit records. Audit
records use the same redaction, context, session, and storage model as logs, but
they have stricter retention and delivery rules.

Audit-worthy events include:

- extension package trust approval, denial, update, or disable;
- native module load or unload with identity and signature status;
- credential-provider access requests and outcomes;
- release signing operations;
- remote MCP/SSE transport enablement and authentication failures;
- diagnostic-bundle creation and selected privacy level;
- support-session logging or profiling overrides;
- security policy violations and permission denials.

Rules:

- audit records are never emitted at `Trace` or `Debug`;
- audit records are not sampled away;
- audit records never contain credentials, tokens, private keys, raw payloads, or
  unrestricted environment values;
- if the normal async sink is saturated, audit records follow the high-severity
  emergency path or record a durable audit-delivery failure;
- audit records use stable event IDs so support and tests can filter them without
  parsing messages.

## Testing

Logging tests use deterministic seams instead of sleeping or depending on the
developer's process-global state.

| Concern | Test seam |
|---|---|
| Time and rotation | `FakeClock` with explicit wall and monotonic advancement |
| Delivery | `RecordingSink` and bounded fake asynchronous dispatcher |
| Files and permissions | isolated temporary platform log root |
| Worker propagation | deterministic executor with barriers and named workers |
| Child process context | test executable that prints its received allowlist |
| Allocation behavior | allocation probe around disabled and enabled calls |

Platform offline tests additionally exercise crash/recovery and terminal checkpoint
boundaries, queue-versus-Save owner separation, bounded cardinality and redaction of
every durable state without exposing personal or provider-native identity.

### C++ Patterns

Compile-time and runtime-disabled calls verify that expensive arguments are not
evaluated:

```cpp
TEST_CASE("disabled debug logging does not evaluate arguments",
          "[observability][logging]") {
    RecordingSink sink;
    Logger logger = MakeLogger(LogLevel::Info, sink);
    ScopedLoggerOverride install{logger};
    int evaluations = 0;

    HORO_LOG_DEBUG("asset.import", "value={}", ++evaluations);

    REQUIRE(evaluations == 0);
    REQUIRE(sink.Records().empty());
}
```

The compile-time form runs in a dedicated target built with
`HORO_COMPILED_LOG_LEVEL=Info`. The runtime form compiles the call site but
configures the category above `Debug`. An allocation probe asserts that neither
disabled path allocates a record or formatting buffer.

Parallel context propagation uses a controlled executor:

```cpp
TEST_CASE("parallel tasks do not leak diagnostic context",
          "[observability][context]") {
    DeterministicExecutor executor{2};
    RecordingSink sink;

    executor.Submit(Context{{"operation.id", "op-a"},
                            {"request.param", 1}},
                    [] { HORO_LOG_INFO("test.flow", "step"); });
    executor.Submit(Context{{"operation.id", "op-b"},
                            {"request.param", 2}},
                    [] { HORO_LOG_INFO("test.flow", "step"); });
    executor.RunInterleaved();

    REQUIRE(sink.ForOperation("op-a").OnlyHas("request.param", 1));
    REQUIRE(sink.ForOperation("op-b").OnlyHas("request.param", 2));
    REQUIRE(executor.WorkerContextsRestored());
}
```

Rotation tests use tiny limits and `FakeClock` to prove that the active segment
is not deleted or renamed, only eligible closed sessions are removed, missing
shutdown markers are detected, and file failure leaves the emergency sink
usable.

Crash diagnostics tests create synthetic crash markers and verify next-launch
association, previous-session clean/unclean state, diagnostic-bundle inclusion,
and absence of automatic upload. Hang watchdog tests use a fake clock and
scripted main-thread heartbeat so dump collection and process termination remain
policy-controlled.

### Python Patterns

Python tests use `caplog`, temporary directories, `contextvars.copy_context()`,
and deterministic queue listeners. They verify:

- canonical level/category/environment parsing
- C++/Python JSON schema parity through shared golden records
- async-task inheritance and explicit executor propagation
- exception fields and redaction before handler delivery
- stderr/stdout separation for CLI tools
- bounded queue listener shutdown and flush

Golden records normalize process IDs, timestamps, paths, and sequence numbers
before comparison. Tests never compare unstable machine-specific values
directly.

### Performance Benchmarks

Dedicated non-sanitized targets report median/high-percentile latency,
allocation count, dropped records, producer count, and output volume for:

- compiled-out logging versus an empty function
- runtime-disabled category checks
- enabled formatting and structured field construction
- multi-threaded producers under normal and saturated queues

Budgets are platform-specific or compared to checked-in baselines with explicit
tolerance.

## CI And Operations

CI:

- stores logs for failed jobs
- may store bounded logs for successful release jobs according to retention
- redacts logs before artifact upload
- uses JSON logs for machine processing and concise human summaries
- records test ID, shard, platform, compiler, preset, and source revision in
  context
- rejects known secret patterns in uploaded log artifacts

Release packages are smoke-tested for:

- writable platform log directory
- startup metadata
- rotation
- clean shutdown marker
- packaged editor diagnostic-bundle creation
- game-specific log separation

## Related Documents

- [Observability Architecture](./observability.md): decisions and reading paths.
- [Metrics And Profiling Contract](./observability-performance.md): CPU, memory,
  frame metrics, and detailed captures.
- [System Design](../foundation/system-design.md): module boundaries and host composition.
- [Engine Data Bus](../foundation/engine-data-bus.md): process notification flow.
- [Editor Data Bus](../editor/editor-data-bus.md): editor-session notification flow.
- [Editor Panel Host](../editor/editor-panel-host.md): Console tab behavior.
- [Editor Modal Host](../editor/editor-modal-host.md): Settings and release workflows.
- [Developer Environment](../delivery/developer-environment.md): development commands.
- [Testing Architecture](../delivery/testing-architecture.md): test layers and harnesses.
- [Quality And CI](../delivery/quality-and-ci.md): gates and retained CI artifacts.
- [Release Architecture](../release/release.md): job-specific logs.
- [Release Security](../release/release-security.md): secrets, privacy, and redaction.
- [ADR-136](../../adr/136-platform-offline-queue-ownership-replay-and-cloud-intent-boundary.md): offline durability states, owner separation and privacy-safe diagnostics.
