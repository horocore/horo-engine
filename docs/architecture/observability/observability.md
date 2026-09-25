# Observability Architecture

## Purpose

This document is the entry point for Horo Engine observability. It summarizes
the decisions that govern logs, metrics, profiler captures, diagnostic context,
storage, and support diagnostics across:

- HoroEditor
- CLI and headless hosts
- Python tooling
- build and release jobs
- development, profile, and shipping games

Detailed contracts are split by concern:

- [Logging, Context, And Diagnostics](./observability-logging.md)
- [Metrics And Profiling](./observability-performance.md)
- [Developer Instrumentation Guide](./developer-instrumentation.md)

## Executive Summary

The architecture is built around ten decisions:

1. **Logs, metrics, and profiler traces are separate signals.** Logs explain
   events, metrics show bounded numeric history, and profiler captures explain
   where time or memory was spent.
2. **Every process owns one `ObservabilityRuntime`.** Modules emit through
   Horo-owned facades; they do not configure independent sinks or stores.
3. **Verbose logging and profiler instrumentation are gated before expensive
   arguments are evaluated.** Compiled-out calls have no runtime cost.
4. **Always-on metrics are sampled and bounded.** CPU, memory, frame time, and
   subsystem counters cannot grow into an unbounded telemetry workload.
5. **Detailed profiling is explicit.** CPU/GPU timelines, allocation events,
   and callstacks run only in allowed product profiles and bounded capture
   sessions.
6. **Diagnostic context follows logical work.** Immutable MDC context propagates
   through nested calls, worker jobs, async tasks, and allowlisted child-process
   boundaries without leaking between parallel operations.
7. **Stores own data; buses announce revisions.** Logs, metric samples, and
   profiler data do not travel as per-record or per-sample data-bus events.
8. **Persistent data is structured, rotated, and private by default.** Logs use
   JSONL, metric summaries are bounded, captures have manifests, and all outputs
   live in platform user-state directories.
9. **Editor and game views are presentation clients.** `ConsoleTab`,
   `PerformanceTab`, and development overlays query stores and services; they do
   not own collection.
10. **Support export is user initiated.** Diagnostic bundles are allowlisted,
    previewable, redacted, and never uploaded automatically.

## Why This Design

One mechanism cannot serve every observability need efficiently:

- Writing a log every frame creates I/O and formatting pressure.
- Keeping only counters cannot reconstruct a failing operation.
- Recording every allocation and callstack continuously changes the behavior
  being measured.
- Publishing every record through a data bus couples producers to consumers and
  gives hidden UI surfaces control over throughput.

Horo therefore uses a layered model:

```text
always available                    explicitly activated

structured lifecycle logs           CPU/GPU scope timeline
bounded CPU/memory metrics           lock and task timeline
frame/subsystem histograms           allocation/free events
tagged allocator totals              allocation callstacks
```

The operating rule is simple: inexpensive continuous health signals stay
separate from detailed capture sessions. A developer should be able to leave
core logs and metrics enabled during normal work, then explicitly arm a bounded
capture when timeline, allocation, or GPU detail is needed.

## Observability Surfaces

Observability has several consumers, but only one collection authority per
process:

| Surface | Reads | May control | Must not own |
|---|---|---|---|
| Editor Console tab | `StructuredLogStore` | filters and local presentation | sink configuration or record retention |
| Editor Performance tab | `MetricsStore`, capture state | profiler start/stop operations | metric sampling cadence from graph redraw |
| Runtime debug overlay | bounded logs, metrics, capture status | profile-allowed overlay toggles | gameplay, renderer, or profiler storage |
| CLI and MCP | typed query/control capabilities | declared diagnostics operations | ad hoc parsing of log files for success/failure |
| Diagnostic bundle | approved stores and manifests | user-approved local export | automatic upload or unrestricted file collection |

All surfaces use typed capabilities. They do not reach into sink internals,
mutate store buffers directly, or turn a presentation refresh into new
instrumentation work.

## How To Read

Choose the shortest path for the task:

| Reader or task | Read |
|---|---|
| Architecture or product review | This document through [Product Profiles](#product-profiles) |
| Add a C++ log | [Logging](./observability-logging.md), then the reference C++ API and level rules |
| Add a metric | [Metrics](./observability-performance.md#metrics-architecture), then the reference metric descriptor and cardinality rules |
| Instrument engine, game, or plugin code | [Developer Instrumentation Guide](./developer-instrumentation.md) |
| Add profiler zones | [Profiling](./observability-performance.md#profiler-traces), then the reference profiler capture contract |
| Propagate request/job parameters | [Diagnostic Context](./observability-logging.md#mapped-diagnostic-context) |
| Build Console or Performance UI | [Ownership And Flow](#ownership-and-flow) |
| Debug storage or support export | [Storage And Diagnostics](./observability-logging.md#storage-locations) |
| Implement or review tests | [Verification Strategy](#verification-strategy) |

## Signal Model

| Signal | Best for | Storage | Default cost |
|---|---|---|---|
| Structured log | Discrete event, decision, failure, lifecycle | JSONL and bounded memory store | Level/category dependent |
| Counter/gauge/histogram | CPU, memory, frame and subsystem trends | Bounded multi-resolution metric store | Low and sampled |
| Profiler trace | Thread, task, GPU, lock and allocation timeline | Bounded capture session | Opt-in, channel dependent |
| Typed error/diagnostic | Operation result and caller control flow | Owning result/job model | Normal application cost |

Typed errors remain the application contract. A caller never parses logs or
metrics to discover whether an operation succeeded.

Renderer-specific findings follow
[ADR-041](../../adr/041-backend-neutral-renderer-diagnostics-model.md). They use a
bounded backend-neutral event/ingestion seam and then project into the same
process-owned structured log stores, retention and privacy policy. This does not
create a renderer-owned sink or make diagnostic events operation results,
per-frame metrics or profiler records.

## Ownership And Flow

Each process composition root creates one runtime before application services:

```text
process composition root
        |
        +-- ObservabilityRuntime
                +-- Logger
                +-- MetricsRegistry
                +-- StructuredLogStore       optional by host
                +-- MetricsStore             bounded
                +-- ProfilerCaptureService   profile gated
                +-- persistent/emergency sinks
```

Modules use stable facades. Platform adapters supply OS process measurements and
file sinks. Application services own diagnostic bundles and profiler capture
operations. GUI surfaces query narrow capabilities.

```text
producer commits log/sample/capture state
        |
        +-- owning store or service updates
        |
        +-- coalesced revision notification
        |
        +-- ConsoleTab / PerformanceTab queries required range
```

`EngineDataBus` and `EditorDataBus` never carry every log line, frame sample,
allocation, or profiler zone.

## Observability Descriptor Registration

Modules do not create categories, metric instruments, profiler zones, or
diagnostic-bundle hooks ad hoc at runtime. They declare observability metadata
through descriptors during module discovery and activation.

```cpp
struct ObservabilityContribution {
    ModuleId module;
    std::span<const LogCategoryDescriptor> logCategories;
    std::span<const MetricDescriptor> metrics;
    std::span<const ProfilerZoneDescriptor> profilerZones;
    std::span<const DiagnosticBundleHookDescriptor> bundleHooks;
};
```

The host validates observability contributions before activation:

- log categories must be stable, lowercase, hierarchical, and owned by the
  declaring module;
- metric descriptors must declare kind, unit, channel, dimensions, availability,
  and series budget;
- profiler zones must declare channel, product-profile availability, and
  expected overhead class;
- diagnostic-bundle hooks must declare privacy level, size estimate, required
  permissions, and whether user approval is required;
- duplicate category, metric, profiler-zone, or bundle-hook IDs fail activation;
- extension packages may contribute observability descriptors only through their
  approved module/extension boundary.

Runtime code may use only validated descriptors or handles derived from them.
Dynamic category or metric creation is forbidden except for explicitly bounded
developer diagnostics.

## Schema Evolution And Compatibility

Persistent observability artifacts are versioned independently from in-process
C++ types.

Versioned artifacts include:

- log JSONL records
- `session.json`
- `shutdown.json`
- `metrics.jsonl`
- profiler `manifest.json`
- diagnostic-bundle manifest
- exported CI/support summaries

Rules:

- adding optional fields is backward compatible;
- removing or changing the meaning/type of an existing field requires a new
  schema version;
- readers must ignore unknown optional fields;
- required-field removal is not allowed within the same schema version;
- enum values may be extended, but readers must handle unknown values as
  `unknown` rather than failing the whole artifact;
- support tools declare the oldest schema version they can read;
- diagnostic bundles record the schema versions of every included artifact.

Migration is a reader concern. Existing logs and captures are immutable and are
not rewritten during normal startup.

## Observability Control Operations

Observability controls are application operations, not direct UI or data-bus
commands.

Supported operations include:

- change effective log level or category filter;
- enable or disable file/console/in-memory sinks where the product profile allows;
- start, arm, stop, or abort a profiler capture;
- start, arm, stop, cancel, inspect, or export a profile-allowed external graphics
  capture through [ADR-047](../../adr/047-renderdoc-pix-and-metal-capture-integration.md);
- capture next hitch;
- export a diagnostic bundle;
- open the current log or capture folder;
- clear a presentation view without deleting persistent records.

Every control operation declares:

- required capability;
- product-profile availability;
- whether user approval is required;
- expected cost and synchronization point;
- result/error contract;
- audit requirement when security- or privacy-relevant.

GUI, CLI, MCP, and Python adapters call these same operations. They do not mutate
logger, metric, profiler, or sink internals directly.

## External Export And Telemetry Boundary

Automatic telemetry upload remains out of scope for this architecture. Local
observability does not require network access. Horo's optional OpenTelemetry
adapter is compiled and activated only by a process composition root, and
runtime export additionally requires explicit approval and an endpoint.

Any external exporter, including the OpenTelemetry adapter, must satisfy:

- user or product approval;
- redaction before export;
- bounded batching and retry;
- offline behavior;
- authentication and endpoint policy;
- schema mapping from Horo records to the external format;
- audit records for enablement and export failures.

No module may send logs, metrics, traces, captures, or diagnostic bundles to a
network endpoint directly.

### Out-Of-Scope Signals

The following observability concerns are intentionally not covered by the core
architecture:

- **Cross-machine trace correlation** (e.g. OpenTelemetry trace IDs across
  dedicated server farms or CI build agents). The optional exporter preserves
  Horo correlation identifiers, but defining a distributed tracing topology is
  owned by the deploying product.
- **Dedicated multiplayer/networking health metrics** such as packet loss, RTT,
  or desync detection. These belong to the networking subsystem; they may emit
  logs or register metrics through the same descriptors but are not defined
  here.

## Logging, Metrics, and Profiling Reference Map

All implementation contracts, API specifications, and testing patterns are located in the sub-contract documents:

- **Logs, Diagnostic Context, & Storage**: Details on log levels, mapping diagnostic context (MDC), Python/C++ logging APIs, rotation, retention, redaction, and support diagnostic bundles are defined in the [Logging, Context, And Diagnostics Contract](./observability-logging.md).
- **Metrics, Profiling, & Performance**: Details on counters/gauges/histograms, GPU memory verification, C++/Python metrics APIs, profiler state machine, game overlays, and performance benchmarks are defined in the [Metrics And Profiling Contract](./observability-performance.md).

---

## Product Profiles

| Profile | Logs | Core metrics | Profiler | External graphics capture |
|---|---|---|---|---|
| Local Debug/test | Trace compiled, Debug default | On | Available, off by default | Explicit local request when a qualified provider is available |
| Local Release/profile | Trace compiled, Info default | On | Available, off by default | Explicit local request when a qualified provider is available |
| Packaged HoroEditor | Debug compiled, Info default | On | Selected channels | Entitlement/profile/provider gated; off by default |
| Game Development | Trace compiled, Debug default | On | Available, off by default | Explicit local request; off by default |
| Game Profile | Trace compiled, Info default | On | Available, off by default | Explicit local qualification request; off by default |
| Game Shipping | Info and above | Low-rate bounded set | Compiled out | Disabled |
| Diagnostics build | Trace available | On | Explicit detailed channels | Explicit entitlement/provider request; off by default |

Shipping observability remains local. It does not enable automatic telemetry,
arbitrary capture paths, source-path disclosure, or an unauthenticated profiler
listener.

---

## Storage Locations

Log sessions, metric files and profiler captures use the existing log/session
roots:

- **macOS logs**: `~/Library/Logs/Horo/`
- **Windows logs**: `%LOCALAPPDATA%\Horo\Logs\`
- **Linux logs**: `${XDG_STATE_HOME:-~/.local/state}/horo/log/`

ADR-047 Horo-owned graphics captures are large restricted native artifacts and use
a separate Platform state subtree that log rotation/support-bundle scanners never
traverse implicitly:

- **macOS graphics captures**: `~/Library/Application Support/Horo/Diagnostics/GraphicsCaptures/`
- **Windows graphics captures**: `%LOCALAPPDATA%\Horo\Diagnostics\GraphicsCaptures\`
- **Linux graphics captures**: `${XDG_STATE_HOME:-~/.local/state}/horo/diagnostics/graphics-captures/`

The Platform `UserDirectories` implementation resolves these logical roots;
callers do not construct them from environment/home strings. Graphics captures
have their own manifest and retention policy and are not ordinary profiler traces
or automatic support-bundle inputs.

The exact directory tree structures, file rotations, and retention configurations are defined in the [Storage Locations](./observability-logging.md#storage-locations) section of the logging contract.

---

## Quick Start

```bash
# Focused editor logs.
HORO_LOG_LEVEL=info \
HORO_LOG_LEVELS=asset.import=trace \
python3 scripts/dev.py run editor

# Twenty-second CPU/GPU/jobs capture.
HoroEditor \
  --profile-capture 20 \
  --profile-channels cpu,gpu,jobs,counters

# One hour of low-rate CPU/memory history.
HORO_METRICS_SAMPLE_MS=1000 \
HORO_METRICS_HISTORY_SECONDS=3600 \
python3 scripts/dev.py run editor
```

All supported options and environment variables are detailed in:

- [Logging configuration](./observability-logging.md#runtime-configuration)
- [Metrics/profiler configuration](./observability-performance.md#metrics-and-profiler-configuration)

---

## Verification Strategy

Observability logic is tested via isolated, deterministic seams:

- **Logging Verification**: Covered in [observability-logging.md#testing](./observability-logging.md#testing).
- **Metrics & Profiler Verification**: Covered in [observability-performance.md#testing](./observability-performance.md#testing).

Additional required cross-cutting tests:

- observability descriptor registration rejects duplicate log categories,
  metric names, profiler zones, and diagnostic-bundle hooks;
- metric dimension registration rejects unknown dimensions and enforces
  per-metric series budgets;
- unavailable metrics are rendered as unavailable, not zero;
- schema readers ignore unknown optional fields and reject incompatible required
  schema changes;
- diagnostic bundle privacy policy excludes `Forbidden` fields and redacts
  `SensitiveRedacted` fields before writing;
- audit records are not sampled away and survive normal low-severity queue
  saturation;
- profiler capture start/stop goes through typed observability operations and
  cannot be triggered by data-bus events;
- support-session verbose logging is bounded, visible, redacted, and audit
  recorded;
- child-process session links connect parent/child logs without copying full MDC
  or environment.

---

## Related Documents

- [Logging, Context, And Diagnostics Contract](./observability-logging.md)
- [Metrics And Profiling Contract](./observability-performance.md)
- [System Design](../foundation/system-design.md)
- [Engine Data Bus](../foundation/engine-data-bus.md)
- [Editor Data Bus](../editor/editor-data-bus.md)
- [Editor Panel Host](../editor/editor-panel-host.md)
- [Runtime Debug Console And Development Overlays](../runtime/debug-console-and-overlays.md)
- [Developer Environment](../delivery/developer-environment.md)
- [Testing Architecture](../delivery/testing-architecture.md)
- [Quality And CI](../delivery/quality-and-ci.md)
- [Release Architecture](../release/release.md)
- [Release Security](../release/release-security.md)
- [Observability Dashboard](../../../mock-studio/designs.md#architecture-observability-observability-dashboard): React mock design
  for logs, metrics, and profiler presentation surfaces.
