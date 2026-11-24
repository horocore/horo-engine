# Terrain authoring

[Workspace commands](workspace/README.md) connect terrain documents and tools to the editor.
The [document](document/README.md) holds the workspace's connection to the authoritative source.
[Assets](assets/README.md) handles creation and format, [brushes](brushes/README.md) produces edits,
and the [foliage palette](foliage/README.md) supplies paint selection.

In the [viewport tools](viewport/), [TerrainViewportTool.cpp](viewport/TerrainViewportTool.cpp)
coordinates the editing tool, while [TerrainViewportPicking.cpp](viewport/TerrainViewportPicking.cpp)
and [TerrainViewportProjection.cpp](viewport/TerrainViewportProjection.cpp) handle picking and
the data displayed during an edit.
