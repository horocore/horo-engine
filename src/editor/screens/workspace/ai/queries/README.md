# Environment query authoring

[EditorWorkspaceEnvironmentQueryDocument.cpp](EditorWorkspaceEnvironmentQueryDocument.cpp)
handles query documents, with separate adapters for external updates and recovery.
[EditorWorkspaceEnvironmentQuery.cpp](EditorWorkspaceEnvironmentQuery.cpp) coordinates
preview and checks its document fence before publishing results.
[Preview adapters](preview/README.md) connect available physics and navigation services.
