# Prefab runtime

Owns prefab documents, dependency resolution, cooked templates and transactional instance preparation.

- [`CookedPrefab.cpp`](CookedPrefab.cpp) — Cooked template representation.
- [`CookedPrefabCodec.cpp`](CookedPrefabCodec.cpp) — Cooked serialization and validation.
- [`PrefabAssetDependencyClosure.cpp`](PrefabAssetDependencyClosure.cpp) — Prefab asset dependency traversal.

The [Scene runtime](../scene/README.md) owns aggregate publication. AI, physics and gameplay contribute their own prepared state to that transaction.

[Public headers](../../../include/Horo/Prefab/) · [Tests](../../../tests/unit/runtime/prefab/) · [Architecture](../../../docs/architecture/runtime/prefab-architecture.md)
