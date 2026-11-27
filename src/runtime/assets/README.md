# Runtime assets

Owns asset catalog publication, import and cook services, runtime asset loading and the resource lease boundary.

- [`registry/`](registry/) — Asset records, paths and registry snapshots.
- [`importer/`](importer/) — Source import and format adapters.
- [`cooker/`](cooker/) — Cooked artifact production.
- [`runtime_provider/`](runtime_provider/) — Runtime asset access and load providers.
- [`hot_reload/`](hot_reload/) — Asset replacement and invalidation.

[Public headers](../../../include/Horo/Assets/) · [Tests](../../../tests/unit/runtime/assets/) · [Architecture](../../../docs/architecture/runtime/asset-pipeline.md)
