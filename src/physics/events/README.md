# Physics event projection

This directory turns physics contact and trigger state into public event records. It also captures and restores the event pair state needed at world snapshot boundaries.

Start with `PhysicsEventProjection.cpp` for event production and `PhysicsWorldStateEventCapture.cpp` for snapshot capture. The world and snapshot owners call these helpers through private headers. [Event projection tests](../../../tests/unit/runtime/physics/PhysicsEventProjectionTests.cpp) cover contact and trigger transitions.
