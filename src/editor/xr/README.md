# Editor XR tools

Owns the editor simulator panel, its controls and status, and the adapter that supplies Scene data to the simulator. The [application XR host](../../../apps/HoroEditor/composition/xr/) owns Play session setup. The [XR runtime](../../runtime/xr/README.md) owns tracking and frame execution.

| If you are changing | Start here |
| --- | --- |
| Simulator controls or displayed status | [Simulator panel](EditorXRSimulatorPanel.cpp), [simulator model](EditorXRSimulatorModel.cpp) |
| Scene data supplied to a simulator session | [Simulator Scene source](EditorSimulatorSceneSource.cpp) |
| End-to-end simulator workflows | [Simulator workflow tests](../../../tests/integration/xr/XRSimulatorWorkflowTests.cpp) |

Use the editor design system and localization for panel changes. Keep headset-specific capabilities and native handles in the [OpenXR adapter](../../runtime/xr/openxr/README.md).
