# XR runtime

Owns XR capabilities, session and frame lifecycle, tracking and interaction contracts. Application hosts choose a backend and admit the runtime for a particular Play session.

- [`api/`](api/) — Backend-neutral identities, poses, views and capability snapshots.
- [`runtime/`](runtime/) — Session, reference-space and frame admission.
- [`input/`](input/) — Action bindings and tracking publication.
- [`openxr/`](openxr/) — Native discovery, spaces, actions, swapchains and compositor. The optional passthrough provider lives in [`openxr/modules/meta_quest/`](openxr/modules/meta_quest/).
- [`simulator/`](simulator/) — Simulated XR input and tracking for development and tests.

The [Editor XR host](../../../apps/HoroEditor/composition/xr/) owns Play composition. Native handles stay inside the OpenXR adapter. `HORO_BUILD_XR_META_QUEST` includes a provider. Headset qualification also requires an admitted runtime, graphics backend and device.

[Public headers](../../../include/Horo/XR/) · [Tests](../../../tests/unit/runtime/xr/) · [Architecture](../../../docs/architecture/runtime/vr-ar-architecture.md)
