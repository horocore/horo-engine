# Renderer

Owns rendering contracts, resource and frame lifetime, render-graph execution and the concrete graphics backends.

- [`api/`](api/) — Renderer-facing data, capabilities and resource contracts.
- [`frontend/`](frontend/) — Frame and resource ownership, backend admission and graph execution.
- [`render_graph/`](render_graph/) — Pass and resource dependency compilation.
- [`modules/`](modules/) — Concrete renderer implementations.
- [`shader_cook/`](shader_cook/) — Offline shader compilation and reflection handling.

Editor viewport composition lives in [the editor renderer](../../editor/renderer/). Backend work follows the [backend parity contract](../../../docs/architecture/runtime/render-backend-parity-contract.md).

[Public headers](../../../include/Horo/Runtime/Render/) · [Tests](../../../tests/unit/runtime/renderer/) · [Architecture](../../../docs/architecture/runtime/rendering-architecture.md)
