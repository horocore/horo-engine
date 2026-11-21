# Frame scheduling

Owns runtime phase dispatch, fixed-step accumulation and simulation control.

- [`FrameScheduler.cpp`](FrameScheduler.cpp) — Frame scheduling and phase progression.
- [`FrameSchedulerDispatch.cpp`](FrameSchedulerDispatch.cpp) — Dispatch to registered phase participants.
- [`RuntimeSimulationAccumulator.cpp`](RuntimeSimulationAccumulator.cpp) — Fixed-step accumulation.
- [`RuntimeSimulationControl.cpp`](RuntimeSimulationControl.cpp) — Simulation admission and control.

[Public headers](../../../include/Horo/Runtime/) · [Tests](../../../tests/unit/runtime/frame/) · [Architecture](../../../docs/architecture/runtime/runtime-lifecycle.md)
