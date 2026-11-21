# OpenXR adapter

Owns native OpenXR instance and session resources, translates Horo actions and poses, and submits frames to the runtime. The application selects the runtime and graphics binding. The [XR runtime](../README.md) owns the backend-neutral lifecycle and capability contracts.

| If you are changing | Start here |
| --- | --- |
| Discovery, capability census or session setup | [Native census](OpenXRNativeCensus.cpp), [session preparation](OpenXRNativePreparation.cpp), [native session](OpenXRNativeSession.cpp) |
| Action registration, sampling or reference spaces | [Action preparation](OpenXRActionPreparation.cpp), [action sampling](OpenXRActionSampling.cpp), [tracking spaces](OpenXRTrackingSpaces.cpp) |
| View submission, swapchain ownership or Vulkan binding | [Frame compositor](OpenXRFrameCompositor.cpp), [swapchains](OpenXRSwapchain.cpp), [Vulkan graphics binding](vulkan/OpenXRSelectedVulkanBinding.cpp) |
| Meta Quest passthrough | [Optional Meta Quest provider](modules/meta_quest/README.md) |

Keep native handles in this adapter and its private `host/` interface. Build and test through the [XR tests](../../../../tests/unit/runtime/xr/) and use the [native session guide](../../../../docs/guides/openxr-native-session-composition.md) for application configuration and headset checks.
