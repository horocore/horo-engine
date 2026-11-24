# Physics world

Implements the public world operations. [PhysicsWorld.cpp](../PhysicsWorld.cpp) owns the world and is
the entry point for callers. [settings/](settings/) validates descriptors and policy at admission.
[query/](query/) handles live query requests, while [snapshot/](snapshot/) and [state/](state/) handle
capture, decoding, and restore for an existing world.

- [`lifecycle/`](lifecycle/) · Physics world lifecycle
- [`query/`](query/) · Physics world queries
- [`settings/`](settings/) · Physics world settings
- [`snapshot/`](snapshot/) · Physics world snapshots
- [`state/`](state/) · Physics world state
