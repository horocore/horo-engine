# AI runtime

Owns agent activation, blackboards, decision graphs, perception and environment queries. Scene composition grants capabilities and publishes agent generations.

- [`api/`](api/) — Agent, blackboard, decision, perception and query contracts.
- [`compile/`](compile/) — Decision graph compilation and runtime graph instances.
- [`integration/`](integration/) — Scene activation, prefab population changes, scheduling, perception and environment query execution.
- [`registration/`](registration/) — AI registration and descriptor contributions.

Use [Navigation](../navigation/README.md) for pathfinding and movement authority, and [Gameplay integration tests](../../../tests/integration/gameplay/) for prefab ownership across subsystems.

[Public headers](../../../include/Horo/AI/) · [Tests](../../../tests/unit/runtime/ai/) · [Architecture](../../../docs/architecture/runtime/navigation-and-ai-architecture.md)
