# Physics scene integration

Turns authored scene bodies and controllers into a plan and prepares their runtime publication.
Start with [PhysicsSceneActivationPlan.cpp](activation/PhysicsSceneActivationPlan.cpp) for planning and
[PhysicsSceneActivationLifecycle.cpp](activation/PhysicsSceneActivationLifecycle.cpp) for lifecycle transitions.
Capture and restore of an existing scene live in [scene state](../scene_state/).

- [`activation/`](activation/) · Scene activation planning
- [`publication/`](publication/) · Scene physics publication
