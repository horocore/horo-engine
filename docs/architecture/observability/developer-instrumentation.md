# Observability Developer Instrumentation

## Purpose

Horo exposes one backend-neutral record and instrumentation model for engine,
editor, game, plugin, test, and tooling code. Producers never select a sink and
never depend on OpenTelemetry. A process composition root owns startup,
filtering, local persistence, optional external sinks, flush, and shutdown.

Public entry points are:

- `Horo/Application/HostObservability.h` for graphical/headless host lifecycle
  and privacy-reviewed support-bundle composition;
- `Horo/Foundation/Logging/Logger.h` for structured logs and host composition;
- `Horo/Foundation/Telemetry/Telemetry.h` for counters, gauges, histograms,
  timings, and diagnostic events;
- `Horo/Foundation/Telemetry/Operation.h` for meaningful operation spans and
  cross-thread context;
- `Horo/Foundation/Diagnostics/DiagnosticBundle.h` for explicit local support
  bundles;
- `Horo/Foundation/Telemetry/OpenTelemetrySink.h` only for composition roots
  built with `HORO_ENABLE_OPENTELEMETRY=ON`.

There is one API and one dispatcher. External backends are consumers of Horo
records; they are not alternate instrumentation APIs.

## Host Composition

`HoroEditor` and the terminal `horo-engine` host create the same
`Application::HostObservabilitySession`. The session resolves the host-owned
log directory, starts the common dispatcher, emits process/build/system
identity, and performs reverse bounded shutdown through RAII. Optional sinks
are still selected by the composition root through `LoggerConfiguration`;
Foundation never discovers a GUI, transport, or exporter.

Start observability before services that may log and stop it after all producer
jobs have joined or been cancelled:

```cpp
Horo::Log::LoggerConfiguration configuration{
    .logDirectory = absoluteLogDirectory,
    .baseName = "horo-game",
    .hostName = "MyGame",
    .hostVersion = gameVersion,
    .queueCapacity = 4096,
    .maxFileBytes = 10U * 1024U * 1024U,
    .maxRolledFiles = 5,
};

if (!Horo::Log::Logger::Init(configuration)) {
    // Continue only with the emergency path and expose degraded diagnostics.
}

// Construct services and jobs here.

Horo::Log::Logger::Shutdown();
```

`Logger::Init` composes the common bounded dispatcher, rolling JSONL sink,
bounded `StructuredLogStore` adapter, persistent operation history, and any
host-provided `additionalSinks`. The host writes a versioned session marker at
startup and writes the matching clean-shutdown marker only after a successful
bounded drain. A mismatched or missing marker identifies the previous session
as interrupted.

`LoggerConfiguration` is also the typed runtime policy shared by hosts. It owns
severity and hierarchical subsystem filters, queue/overflow limits, JSONL file
and retention limits, flush/shutdown deadlines, additional sinks, and
`MetricCollectionLevel`. Metric policy is `Off`, `Core`, or `Detailed`;
instruments declare their minimum level and are rejected during registration
when the host policy is lower. Turning metrics off does not disable structured
logging or diagnostic events.

Modules must not call `Logger::Init`, `Logger::Shutdown`, or
`Telemetry::Runtime::Initialize`. Tests may initialize the runtime directly
when exercising an isolated sink or producer contract.

The headless composition can be exercised without a display or GPU:

```bash
cmake -S . -B build/headless \
  -DHORO_BUILD_EDITOR_GUI=OFF \
  -DHORO_BUILD_RENDER_OPENGL=OFF \
  -DHORO_BUILD_RENDER_METAL=OFF
cmake --build build/headless --target horo-engine
./build/headless/apps/horo-engine --emit-observability-smoke
```

Both hosts may generate a local support bundle through the session capability.
The host adapter automatically allowlists current and rolled JSONL logs,
operation history, and session markers. Engine/build/OS/architecture plus
caller-vetted renderer/device/project identity are written to the manifest.
Crash metadata, redacted effective configuration, and package/plugin summaries
must be passed through their typed optional slots. Project files, assets,
environment dumps, credentials, and arbitrary paths are never discovered.
JSON and JSONL inputs selected by the host are parsed into a temporary safe
representation: sensitive keys and absolute paths are redacted before checksums
and ZIP entries are produced. A malformed complete record fails with a typed
bundle read error; an interrupted trailing JSONL fragment is ignored so a
post-crash bundle remains usable.

## Subsystem Namespaces

Subsystems are stable hierarchical identities. Use these roots:

| Owner | Examples |
|---|---|
| Foundation | `Foundation.Jobs`, `Foundation.Paths` |
| Renderer | `Renderer.Frontend`, `Renderer.OpenGL` |
| Editor | `Editor.Project`, `Editor.AssetBrowser` |
| Assets | `Assets.Import`, `Assets.Cook` |
| Game | `Game.Physics`, `Game.Player` |
| Plugin/package | `Plugin.<stable-id>`, `Plugin.<stable-id>.Import` |

Do not include object IDs, filenames, user input, or other high-cardinality
values in a subsystem or metric name. Put those values in log/event/span fields
or inherited diagnostic context. Metric names and dimension keys use lowercase
dotted names such as `jobs.completed` and `result`.

## Structured Logs And Context

```cpp
Horo::Log::LogContext requestContext(
    "correlation.id", requestId,
    "project.id", projectId);

const std::array fields{
    Horo::Telemetry::Field{.key = "retry", .value = std::uint64_t{2}},
    Horo::Telemetry::Field{.key = "cached", .value = false},
};
Horo::Log::Logger::Write(
    "Assets.Import", Horo::Log::Level::Warn, "Import retry scheduled", fields);
```

The level gate in `LOG_TRACE` through `LOG_CRITICAL` runs before formatting.
Use typed fields for values that must be queried. Context snapshots own their
data and can be forwarded safely.

Every typed field carries a `FieldPrivacy` policy. `Public` values are retained,
`SensitiveRedacted` values become `[REDACTED]`, and `Forbidden` fields are
removed before the common record enters the asynchronous queue. Sensitive key
fragments such as password, secret, token, credential, cookie, authorization,
and private-key names are redacted defensively even when a producer omitted an
explicit privacy classification. The same pre-fan-out policy applies to MDC
context, so local JSONL, in-memory stores, and optional exporters observe one
safe record.

## Metrics

Register descriptors during module activation, bind bounded dimensions outside
the update path, and retain the resulting handle:

```cpp
auto completed = Horo::Telemetry::Runtime::RegisterCounter({
    .name = "jobs.completed",
    .subsystem = "Foundation.Jobs",
    .unit = "operations",
    .dimensions = {{.key = "result", .allowedValues = {"ok", "error"}}},
    .maxSeries = 2,
});

const std::array result{
    Horo::Telemetry::DimensionValue{.key = "result", .value = "ok"},
};
const auto completedOk = completed.WithDimensions(result);
completedOk.Add();
```

`Counter`, `Gauge`, `Histogram`, and `Timing` updates are non-throwing. An empty
handle is a no-op. Unknown dimensions, values outside the descriptor allowlist,
duplicate descriptors, stale handles, and exhausted series budgets are rejected
and reflected in runtime health statistics.

## Operations, Events, And Jobs

```cpp
Horo::Telemetry::OperationSpan import{"Assets.Import", "Asset.Import"};

jobs.Submit({}, [&] (const Horo::CancellationToken&) {
    Horo::Telemetry::OperationSpan decode{"Assets.Import", "Asset.Decode"};
    static_cast<void>(Horo::Telemetry::Runtime::EmitEvent(
        "Assets.Import", "asset.decoded", Horo::Log::Level::Info, "Decoded"));
    static_cast<void>(decode.Complete(Horo::Telemetry::SpanStatus::Succeeded));
});

static_cast<void>(import.Complete(Horo::Telemetry::SpanStatus::Succeeded));
```

`JobSystem` captures the active operation and diagnostic context when work is
submitted and restores it only for that job execution. Child spans therefore
retain parent and correlation identity without OpenTelemetry context types.
Destroying an incomplete operation records cancellation. Only the first valid
terminal transition succeeds.

## Optional OpenTelemetry Export

For the normal local editor workflow, copy the safe example configuration once:

```bash
cp .env.example .env.local
python3 scripts/dev.py run editor
```

`HORO_DEV_OTEL_EXPORT` is a developer-runner convenience setting, not a Horo
runtime option. `scripts/dev.py` resolves process environment over `.env.local`
over defaults, configures a Debug `HoroEditor` build on every invocation, and
then maps the enabled setting to the build-time adapter option and the runtime
export approval. When disabled, inherited OTLP approval and endpoint variables
are removed from the editor child process.

`HORO_OTEL_ENDPOINT` is an OTLP HTTP base URL; the exporter appends the signal
paths. The runner's TCP check proves only that the configured host and port are
reachable. An unavailable Collector produces a warning and never prevents the
editor from continuing with local diagnostics.

The equivalent manual build remains available for advanced use:

Build the isolated adapter explicitly:

```bash
cmake -S . -B build/otel \
  -DHORO_ENABLE_OPENTELEMETRY=ON \
  -DBUILD_TESTING=ON
```

Then compose the approved sink alongside local sinks:

```cpp
Horo::Telemetry::OpenTelemetryConfiguration otlp{
    .endpoint = "https://collector.example.com",
    .serviceName = "my-game",
    .exportApproved = true,
};
auto exporter = Horo::Telemetry::OpenTelemetrySink::Create(otlp);
if (exporter)
    configuration.additionalSinks.push_back(exporter);
```

External plaintext HTTP endpoints are rejected. Plaintext localhost is allowed
for a developer Collector only when `allowInsecureLocalhost` is explicitly enabled. Header
injection is rejected, configured sensitive attribute-key fragments are
redacted before serialization, payload/batch/retry/time limits are bounded, and
offline delivery remains best effort. Transport calls occur only on the common
consumer thread.

The optional HoroEditor composition remains disabled unless both the build
option is enabled and the user sets:

```bash
HORO_OTEL_EXPORT_APPROVED=1 \
HORO_OTEL_ENDPOINT=http://127.0.0.1:4318 \
HoroEditor
```

Setting only the build option never enables network export. Standard builds do
not compile or link the OTLP adapter.

HoroEditor registers the following low-cardinality host metrics at startup:

- `horo.editor.frame.number`
- `horo.editor.frame.duration`
- `horo.observability.records.dropped`
- `horo.observability.sink.failures`

The common dispatcher flushes sink-owned sparse batches once per second by
default, so a metric does not need to fill the exporter's record batch before
it becomes visible. OTLP thread identifiers are exported as strings because
they are opaque platform identities rather than signed numeric measurements.

Engine instrumentation and local metric/profiler histories do not require an
external dashboard or collector. The developer runner and `.env.example` keep
OTLP export disabled unless the developer explicitly opts in. The repository no
longer installs, imports, or checks Grafana dashboards; this does not change the
common record model, dispatcher, local persistence, or metric collection policy.

## Performance Contract And Measurement

`HoroTelemetryFastPathBenchmark` tracks allocations by overriding the test
process allocator around producer calls. The qualification workload covers a
bound counter, a short structured log, disabled logging, four concurrent metric
producers, and an intentionally unpaced bounded-queue saturation burst. Every
attempt must be reflected in either the accepted or explicit dropped counters.

The producer budgets are zero heap allocations for bound metric updates, short
structured log records that fit their inline storage, and disabled log calls.
The Release qualification latency guidance on the reference developer machine
is at most 250 ns per single-producer bound metric attempt, 500 ns per short log
attempt, and 500 ns per attempted update in the four-producer contention case.
These are producer-path budgets, not delivery latency guarantees.

The latest Release arm64 macOS run using AppleClang 17 and 100,000 attempts per
workload measured 38.2 ns for the bound counter, 84.0 ns for the short log,
59.5 ns with four concurrent producers, and 0.87 ns for compiled/runtime-gated
logging. All measured producer allocation counts were zero. Saturation accepted
6,231 counter records and dropped 93,769; the short-log workload accepted
12,878 and dropped 87,122; the contention workload accepted 11,802 and dropped
88,198. Counts vary with consumer scheduling and describe the saturation
workload rather than normal sampled cadence.

The disabled build is separately compiled with `HORO_ENABLE_TELEMETRY=OFF`; its
handles and events are inert and perform zero producer allocations. In the same
qualification run the compiler eliminated the no-op update loop (reported
0.00042 ns per source-level call, zero accepted/dropped records). Treat that as
compile-out evidence rather than a physically meaningful sub-nanosecond timing.
Nanosecond results remain environment-sensitive and must be refreshed when
qualifying a release.

Run focused qualification with:

```bash
ctest --test-dir build/skeleton -R \
  'HoroObservabilityTests|HoroTelemetryFastPathBenchmark' \
  --output-on-failure

ctest --test-dir build/opentelemetry -R HoroOpenTelemetryTests \
  --output-on-failure
```
