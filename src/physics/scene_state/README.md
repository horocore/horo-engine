# Physics scene state

This directory owns physics state capture and restore at the scene boundary. Start with `PhysicsSceneStateCapture.cpp` for capture, `PhysicsSceneStateRestore.cpp` for restore, and `PhysicsSceneStateRebind.cpp` for rebinding restored identities to active scene owners.

[`projection/`](projection/) adapts scene identities and owner information to the physics state model. The existing private scene-host headers expose the capture and restore contracts to application composition.
