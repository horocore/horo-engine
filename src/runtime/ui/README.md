# Runtime UI

Owns game UI execution, presentation state, event routing and accessibility at runtime.

- [`RuntimeUiService.cpp`](RuntimeUiService.cpp) — Runtime UI service entry point.
- [`RuntimeUiScenePublication.cpp`](RuntimeUiScenePublication.cpp) — Scene-bound UI publication.
- [`input/`](input/) — UI input handling.
- [`integration/`](integration/) — UI adapters to runtime participants.

Editor panels and authoring widgets live under [the Editor source](../../editor/).

[Public headers](../../../include/Horo/Runtime/Ui/) · [Tests](../../../tests/unit/runtime/ui/) · [Architecture](../../../docs/architecture/runtime/game-ui-and-hud.md)
