# Physics world settings

[PhysicsWorldDescriptor.cpp](PhysicsWorldDescriptor.cpp) validates the requested world limits, gravity,
and fixed step. [PhysicsWorldSettings.cpp](PhysicsWorldSettings.cpp) validates scene bindings and
world settings together. Separate helpers cover budgets, stepping policy, and transform authority.
Live scene updates and tick handling live in [world lifecycle](../lifecycle/).
