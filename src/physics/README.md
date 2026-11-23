# Physics

Owns bodies, constraints, queries, stepping and native physics state.

- [`canonical/`](canonical/) contains the native solver runtime.
- [`character/`](character/) contains character controllers, movement, and queries.
- [`collision/`](collision/) owns collision cooking dispatch, artifact admission, and cooked shape caching.
- [`events/`](events/) owns event projection and the event state used by world snapshots.
- [`internal/`](internal/) contains the private scene and character host contracts.
- [`model/`](model/) contains shared body, constraint, identity, and material code.
- [`scene/`](scene/) owns activation planning and scene physics publication.
- [`scene_state/`](scene_state/) owns capture, restore, and rebinding against active scene owners.
- [`state/`](state/) owns state codecs, restart, and solver continuation.
- [`world/`](world/) contains world settings, queries, lifecycle, and snapshot helpers.

`PhysicsWorld.cpp` and `PhysicsSceneActivation.cpp` are the primary entry points. The [runtime Scene](../runtime/scene/) owns Scene publication. See the [runtime Physics tests](../../tests/unit/runtime/physics/) for lifecycle, cooking, and query examples.

[Public headers](../../include/Horo/Physics/) · [Architecture](../../docs/architecture/runtime/physics-architecture.md)
