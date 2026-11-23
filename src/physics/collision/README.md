# Collision asset cooking

This directory owns collision cook dispatch, artifact validation, and the cooked shape descriptor and cache in `HoroPhysics`. Start with `PhysicsCollisionCooker.cpp` to trace a cook request and `PhysicsCollisionArtifact.cpp` to trace loading its result.

- [`polyhedral/`](polyhedral/) contains convex hull and compound cooking and codecs.
- [`surfaces/`](surfaces/) contains triangle mesh and height field cooking and codecs.

The [public Physics API](../../../include/Horo/Physics/) defines the requests, results, and limits. The [runtime Physics tests](../../../tests/unit/runtime/physics/) cover cooking, artifact admission, and cache behavior.
