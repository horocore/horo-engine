# Workspace screen lifecycle

[EditorWorkspaceScreenComposition.cpp](EditorWorkspaceScreenComposition.cpp) binds
the controller to borrowed application services.
[EditorWorkspaceScreenLifecycle.cpp](EditorWorkspaceScreenLifecycle.cpp) coordinates activation,
leave, and retirement. The screen actions and input routing pair frame input with the active
workspace, and the XR adapter observes the current Play connection.
