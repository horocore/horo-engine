# Physics world lifecycle

Scene attachment, containment, and shared tick behavior behind the physics world.

[PhysicsWorldScene.cpp](PhysicsWorldScene.cpp) prepares and publishes scene-body changes.
[PhysicsWorldTickHelpers.h](PhysicsWorldTickHelpers.h) supports fixed ticks, and
[PhysicsWorldContainmentInternal.h](PhysicsWorldContainmentInternal.h) supports world failure containment.
Initial Scene activation is handled by the [activation layer](../../scene/activation/).
