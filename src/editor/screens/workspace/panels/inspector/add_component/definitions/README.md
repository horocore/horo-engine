# Built-in component creation definitions

The [registry](../AddComponentRegistry.h) uses stable contribution identities and evaluates
availability from a read-only context. [Physics entries](physics/README.md) define detached
physics creation actions, and [scene entries](scene/README.md) cover audio, camera, lighting, and scripts.
The menu invokes the chosen action through the owning workspace.
