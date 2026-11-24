# Scene document commands

[EditorWorkspaceControllerComponentCommands.cpp](EditorWorkspaceControllerComponentCommands.cpp)
routes component changes. Dedicated persistence and recovery handlers apply the
[editor document model](../../../document/) at the workspace boundary.
Scene views and inspection read the [scene projections](../inspection/README.md).
