# VFX runtime

Owns effects and particle simulation contracts, effect instance lifetime and CPU simulation.

- [`api/`](api/) — Effect definitions, instances and runtime contracts.
- [`simulation/`](simulation/) — Particle and effect evaluation.

GPU work is implemented by [renderer backends](../renderer/modules/).

[Public headers](../../../include/Horo/Vfx/) · [Tests](../../../tests/unit/runtime/vfx/) · [Architecture](../../../docs/architecture/runtime/vfx-and-particles-architecture.md)
