# XR runtime

Owns XR capabilities, session and frame lifecycle, tracking and interaction contracts. Application hosts choose a backend and admit the runtime for a particular Play session.

- [`api/`](api/) — Backend-neutral identities, poses, views and capability snapshots.
- [`runtime/`](runtime/) — Session, reference-space and frame admission.
- [`input/`](input/) — Action bindings and tracking publication.
- [`openxr/`](openxr/) — Native discovery, spaces, actions, swapchains and compositor. The optional passthrough provider lives in [`openxr/modules/meta_quest/`](openxr/modules/meta_quest/).
- [`simulator/`](simulator/) — Simulated XR input and tracking for development and tests.

## Where to start

| If you are changing | Start here |
| --- | --- |
| Session state, shutdown or frame ordering | [Session lifecycle](runtime/XRSessionLifecycle.cpp), [frame lifecycle](runtime/XRFrameLifecycle.cpp) |
| Controller actions or tracking delivery to the engine | [Action bindings](input/XRActionBindings.cpp), [tracking publication](runtime/XRTrackingPublication.cpp) |
| Native views, swapchains or composition | [OpenXR frame compositor](openxr/OpenXRFrameCompositor.cpp), [Vulkan adapter](openxr/vulkan/) |
| Quest passthrough | [Meta Quest provider](openxr/modules/meta_quest/README.md) |
| Reproducing input with the simulator | [Simulator runtime](simulator/XRSimulatorRuntime.cpp), [simulator tests](../../../tests/unit/runtime/xr/XRSimulatorRuntimeTests.cpp) |
| Backend selection and the Editor Play lifecycle | [Editor Play host](../../../apps/HoroEditor/composition/xr/OpenXRPlayHost.cpp), [host composition guide](../../../docs/guides/openxr-native-session-composition.md) |

Keep native handles in the OpenXR adapter. Keep host selection and Play composition in the application. Use the [Editor XR tools](../../editor/xr/) for simulator UI and editor-side authoring.

[Public headers](../../../include/Horo/XR/) · [Tests](../../../tests/unit/runtime/xr/) · [Architecture](../../../docs/architecture/runtime/vr-ar-architecture.md)
