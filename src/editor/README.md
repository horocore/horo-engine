# Editor

Owns editor documents, screens, authoring tools and editor-only runtime adapters.

Start at [`screens/`](screens/) for screen and workspace flows, [`document/`](document/) for editable Scene state, [`design_system/`](design_system/) for shared controls, or [`renderer/`](renderer/) for viewport composition. See the [Editor XR tools](xr/README.md) for simulator authoring and [XR runtime](../runtime/xr/README.md) for session and tracking implementation.

The [workspace guide](screens/workspace/README.md) maps document commands, panel views and viewport tools to their owners.

For tool-specific document owners, see [source documents](source_document/) for
text edits and saves, [UI Canvas documents](ui/canvas/) for canvas authoring and
serialization, and [project collision authoring](physics/) for editing and
reviewing layers, profiles, and query channels.

[Public headers](../../include/Horo/Editor/) · [Tests](../../tests/unit/editor/) · [Architecture](../../docs/architecture/editor/editor-document-model.md)
