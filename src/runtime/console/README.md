# Runtime console

Owns command registration and execution, command admission, and runtime console projections.

- [`CommandRegistry.cpp`](CommandRegistry.cpp) — Command storage and lookup.
- [`CommandExecutor.cpp`](CommandExecutor.cpp) — Dispatch of registered commands.
- [`ConsoleBuiltins.cpp`](ConsoleBuiltins.cpp) — Built-in commands.

[Public headers](../../../include/Horo/Debug/) · [Tests](../../../tests/unit/runtime/console/) · [Architecture](../../../docs/architecture/runtime/debug-console-and-overlays.md)
