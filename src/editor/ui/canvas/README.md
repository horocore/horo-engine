# UI Canvas documents

[UiCanvasDocument.cpp](UiCanvasDocument.cpp) owns the canvas authoring session,
including its working revision and persistence.
[UiCanvasDocumentCommands.cpp](UiCanvasDocumentCommands.cpp) applies changes to
the hierarchy and its properties. Inspector, resource, and preview adapters
are in the [parent directory](../).

[UiCanvasDocumentSerialization.cpp](UiCanvasDocumentSerialization.cpp) reads
and writes the authored format. The asset cooker uses this same codec through
a build-only header configured by [CMake](../../../CMakeLists.txt).
