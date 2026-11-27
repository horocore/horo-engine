# Navigation inspector

[InspectorPanelNavigation.cpp](InspectorPanelNavigation.cpp) draws navigation component controls
for scene objects and prefab members. [NavigationInspectorSession.h](NavigationInspectorSession.h)
keeps a detached draft bound to the current selection and descriptor publication.
Apply sends one typed command to the [navigation workspace](../../../navigation/README.md).
