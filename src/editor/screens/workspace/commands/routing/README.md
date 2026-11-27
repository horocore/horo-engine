# Workspace command routing

[EditorWorkspaceControllerCommandRouting.cpp](EditorWorkspaceControllerCommandRouting.cpp)
selects a handler for each typed view command.
[EditorWorkspaceControllerCommands.cpp](EditorWorkspaceControllerCommands.cpp) implements
common workspace handlers. Feature-specific operations live with their respective owners.
