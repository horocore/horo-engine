# World streaming

Owns World cell identity, attachment, origin frames and rebase transactions. Application composition chooses how World cells enter the runtime Scene.

- [`CookedWorldIndexManifest.cpp`](CookedWorldIndexManifest.cpp) — Cooked World index metadata.
- [`CellAttachmentManifest.cpp`](CellAttachmentManifest.cpp) — Cell attachment metadata.
- [`OriginFrame.cpp`](OriginFrame.cpp) — Origin-frame state and generation identity.
- [`OriginRebaseTransaction.cpp`](OriginRebaseTransaction.cpp) — Coordinated origin replacement.

Mounted World composition lives in [the shared World host](../../../apps/shared/world/). For camera and renderer rebasing see [coordinate precision and origin rebasing](../../../docs/architecture/runtime/coordinate-precision-and-origin-rebasing.md).

[Public headers](../../../include/Horo/WorldStreaming/) · [Tests](../../../tests/unit/runtime/world_streaming/) · [Architecture](../../../docs/architecture/runtime/world-streaming-architecture.md)
