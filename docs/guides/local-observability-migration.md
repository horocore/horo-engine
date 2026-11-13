# Local observability migration

The engine no longer ships an OpenTelemetry/OTLP network exporter. This removes
an unused external-service integration while retaining local logs, metrics,
operation history, profiler data and explicit diagnostic bundles.

## Removed contracts

- `HoroOpenTelemetry` / `HoroEngine::OpenTelemetry` and its public
  `Horo/Foundation/Telemetry/OpenTelemetrySink.h` header are removed, including
  its configuration, transport, statistics and export-error types.
- `HORO_ENABLE_OPENTELEMETRY` is no longer a CMake option. Remove it from local
  configure arguments and presets; it cannot enable an exporter.
- `HORO_DEV_OTEL_EXPORT`, `HORO_OTEL_ENDPOINT` and `HORO_OTEL_EXPORT_APPROVED`
  are no longer read. Remove these keys from shell profiles and `.env.local`.
- The editor module graph no longer describes an optional exporter, and the
  private host-selection struct no longer has `includeOpenTelemetry`.

The affected in-tree callers were editor startup, host module composition,
the developer runner, CI/preflight configuration and the dedicated exporter
tests. Public-header ownership and target dependency inventories are updated
with the removal. There is no compatibility alias or replacement exporter.
External composition code must remove the exporter include/link edge and its
construction. Keep `HostObservabilitySession` or the existing local logger
composition; producers continue to use the same Horo instrumentation types.

`HORO_ENABLE_TELEMETRY` still controls local instrumentation. Local storage,
retention, filtering, correlation, support-bundle privacy and shutdown semantics
are unchanged. The runner's dotenv parser remains for local quality-tool
credentials; editor launch no longer depends on export settings or a collector.
