# Meta Quest OpenXR provider

Owns the OpenXR vendor passthrough implementation in [MetaQuestPassthrough.cpp](MetaQuestPassthrough.cpp). Keep vendor-native handles and calls here. Shared session, action and frame contracts belong in the [XR runtime](../../../README.md).

`HORO_BUILD_XR_META_QUEST` includes the provider in the OpenXR build as the internal `HoroXRMetaQuestInternal` target. When excluded, the adapter uses the [unavailable provider](../../OpenXRPassthroughUnavailable.cpp) to report passthrough availability. The application owns runtime selection and session startup.

For headset qualification, check the OpenXR runtime, graphics binding and device together. Follow the [native session composition guide](../../../../../../docs/guides/openxr-native-session-composition.md) for the host path.
