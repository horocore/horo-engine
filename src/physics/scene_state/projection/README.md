# Physics scene state projection

This directory maps persistent scene entities, keys, and active owners into the physics state representation used for capture, restore, and rebinding.

Start with `PhysicsSceneStateProjection.h` for the private contract. `PhysicsSceneStateProjectionKeys.cpp` owns key mapping and `PhysicsSceneStateProjectionOwners.cpp` gathers owner information.
