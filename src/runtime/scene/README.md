# Scene runtime

Owns live Scene entities, component state, structural transactions and publication. Participating subsystems prepare their own state under the same aggregate boundary.

- [`component/`](component/) — Runtime component code.
- [`entity/`](entity/) — Entity operations.
- [`transitions/`](transitions/) — Scene transition operations.
- [`ActivatedSceneInstances.cpp`](ActivatedSceneInstances.cpp) — Activated child Scene ownership.

The [Scene source and cook code](../../scene/) handles authored data before runtime activation.

[Public headers](../../../include/Horo/Runtime/Scene/) · [Tests](../../../tests/unit/runtime/scene/) · [Architecture](../../../docs/architecture/runtime/scene-runtime.md)
