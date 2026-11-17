# Prefab metrics and logical operation events

The graphical process calls RegisterPrefabMetricHandles once in its telemetry
composition and passes numeric handles to authoring capture and the Play/prefab
stage policy. The network executable similarly registers once after its process
observability session and passes handles through real headless Scene resources to
PrefabTemplateProvider. Providers do not register or discover instrumentation.
PrefabSceneCookRequest.metrics is the explicit cook-host composition input; it is
not hashed into any source, resource, template or generation identity.

Existing callers remain source compatible through defaults. To observe direct
source parsing, pass PrefabSourceParseLimits.metrics for that synchronous call.
BuildPrefabSourceResolverSnapshot accepts the host handles and copies them into
owned immutable results; resolution, scene expansion, value rebase and template
cooking reuse that capture. Cache evidence retains numeric handles separately from
its semantic equality. Runtime providers own their copied configuration, and pending
spawn operations own prepared observations independently of provider/host lifetime.
No callback, logger, sink, native backend or source owner is retained.

| Instrument            | Unit    | Closed dimensions | Maximum series |
|-----------------------|---------|-------------------|----------------|
| horo.prefab.operation | Count   | phase x outcome   | 32             |
| horo.prefab.duration  | Seconds | phase             | 8              |
| horo.prefab.items     | Count   | kind              | 7              |
| horo.prefab.bytes     | Bytes   | kind              | 3              |
| horo.prefab.cache     | Count   | outcome           | 4              |

Phases are parse, graph, resolve, expand, cache, cook, rebase and spawn. Outcomes
are success, failed, cancelled and overflow. Precise existing resource-denial codes
map to overflow; queue/argument admission limits are identified at their real owner
branch. Error text and raw codes do not create series. Count kinds are source and
expanded objects, components, graph nodes/edges, overrides and runtime entities.
Byte kinds are source, cooked and cache. Cache outcomes are hit, miss, evicted and
rejected. Internal storage deduplication emits no extra hit/miss. Different caches,
projects, roots, instances, paths and user/package types share these bounded series.

A phase counts actual measured boundaries, not unique user requests. A cook can
contain a graph build, resolver attempt, typed scene expansion and template cook;
cache lookup and storage are separate cache work. Measurements are aggregate
observations, never a second source of correctness state.

Core policy registers counters/item distributions. Detailed policy additionally
admits durations and optional logical SpanRecord events. Process policy and
subsystem filtering remain authoritative. Registration and span preparation are
best effort. Off/stale construction gates before clock/string/span/MDC work;
empty Advance polling does not create observations. Spawn admission prepares its
owned terminal record; successful receipt observation, failure, cancellation and
shutdown complete it at most once. Moving the empty-context/field record at terminal
avoids producer-side field construction and context redaction allocations.

Foundation provides the canonical operation-ID allocator and a generation-scoped
EmitRecord overload. Zero/stale generations reject; both overloads share the same
filtering, redaction and bounded queue path. A retained old observation cannot emit
into a replacement Telemetry runtime. Queue loss, allocation failure and unavailable
metrics do not alter parsing, cache publication, cooking, spawning or teardown.

Focused behavior tests cover real source/resolver/expansion/cook/rebase/cache paths,
real gameplay spawn receipts and cancellation/shutdown, identity cardinality,
producer-side allocation windows, registration allocation failure, queue loss,
runtime restart and the actual cook host. No CPU/GPU capture backend, backend visual
parity, throughput improvement or exhaustive profiling performance is claimed.

```sh
cmake --build build/dev-editor --target HoroPrefabMetricsTests HoroPrefabSceneCookHostTests --parallel 1
./build/dev-editor/tests/HoroPrefabMetricsTests '[prefab-metrics]'
./build/dev-editor/tests/HoroPrefabSceneCookHostTests '[prefab-metrics]'
```
