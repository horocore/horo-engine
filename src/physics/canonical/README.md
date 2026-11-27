# Canonical solver runtime

Houses the native solver and the entry points used when the native backend is omitted.
Start with [CanonicalPhysicsRuntime.cpp](runtime/CanonicalPhysicsRuntime.cpp) for runtime ownership and
[CanonicalSolver.cpp](solver/CanonicalSolver.cpp) for stepping the solver. The world-facing API lives in
the [world layer](../world/).

- [`diagnostics/`](diagnostics/) · Native diagnostics
- [`query/`](query/) · Native physics queries
- [`runtime/`](runtime/) · Native runtime ownership
- [`scene/`](scene/) · Native scene updates
- [`solver/`](solver/) · Solver configuration
