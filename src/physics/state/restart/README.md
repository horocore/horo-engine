# Physics restart contracts

Restart inputs and restore-candidate admission shared by the physics state owners.

[PhysicsStateRestartInput.cpp](PhysicsStateRestartInput.cpp) allocates and decodes bounded restart
input before preparing a restore. [PhysicsStateRestoreCandidate.cpp](PhysicsStateRestoreCandidate.cpp)
owns the candidate until publication or release. Scene-owner rebinding lives in
[scene state](../../scene_state/).
