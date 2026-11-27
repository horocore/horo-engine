# Native runtime ownership

The Canonical runtime owner, its private state, and the implementation selected when the native backend is omitted.

[CanonicalPhysicsRuntime.cpp](CanonicalPhysicsRuntime.cpp) creates and owns the native world and
connects its query and diagnostics support. [CanonicalPhysicsRuntimeOmitted.cpp](CanonicalPhysicsRuntimeOmitted.cpp)
supplies the same entry points for builds without the native backend.
