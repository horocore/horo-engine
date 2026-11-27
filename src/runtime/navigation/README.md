# Navigation

Owns navigation-world lifetime, agent registration, path queries and movement intent. Hosts select providers and bind the active topology to the Scene.

- [`api/`](api/) — Backend-neutral navigation identities and contracts.
- [`backends/`](backends/) — Concrete navigation providers.
- [`runtime/`](runtime/) — World lifecycle, agent registry and query coordination.
- [`integration/`](integration/) — Scene and Character adapters.

AI consumes the navigation contract for decisions. Character and Physics own movement application and simulation.

[Public headers](../../../include/Horo/Navigation/) · [Tests](../../../tests/unit/runtime/navigation/) · [Architecture](../../../docs/architecture/runtime/navigation-and-ai-architecture.md)
