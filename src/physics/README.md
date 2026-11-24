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

Start with [PhysicsWorld.cpp](PhysicsWorld.cpp) for world ownership and stepping, or
[PhysicsSceneActivation.cpp](PhysicsSceneActivation.cpp) for the transition from authored scene data into a
runtime world. The [runtime Scene](../runtime/scene/) owns publication of the active scene. For save and
restore, follow [scene state](scene_state/) into the [physics state formats](state/).
[PhysicsStateBuildFacts.cpp](PhysicsStateBuildFacts.cpp) records build identity used by native state compatibility checks.

The [runtime Physics tests](../../tests/unit/runtime/physics/) cover lifecycle, cooking, queries, and restore.

[Public headers](../../include/Horo/Physics/) · [Architecture](../../docs/architecture/runtime/physics-architecture.md)
