# Physics diagnostics

[PhysicsErrors.cpp](PhysicsErrors.cpp) defines the error codes and remediation text shared by physics
callers. [PhysicsMetrics.cpp](PhysicsMetrics.cpp) and [PhysicsDiagnostics.cpp](PhysicsDiagnostics.cpp)
cover counters and diagnostic facts. [PhysicsDebugSnapshot.cpp](PhysicsDebugSnapshot.cpp) handles the
debug snapshot projection, with native readback implemented in the [canonical runtime](../../canonical/diagnostics/).
