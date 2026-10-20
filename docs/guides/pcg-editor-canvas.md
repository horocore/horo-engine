# PCG graph canvas

Opening a registered `.hpcg` asset through the existing embedded source route opens
one PCG document tab. Reopening focuses the same writable session. The workspace
host composes the built-in node catalog once and supplies matching source support
and palette recipes to the controller. A host without recipes keeps unknown nodes
readable and removable, preserving their source payloads without inventing schemas.

The default panel registry registers `horo.pcg_graph` as a document contribution.
The document view resolves that presenter for active PCG tabs; registration alone
opens no generic tab. Attach/detach controls drawing admission, never canonical
document ownership. The presenter composes the palette, canvas and selection inspector through
shared localized controls and theme tokens. The palette filters localized names;
its recipes use the real catalog pin schemas and canonical settings codecs for
all 17 built-ins. Initial spatial settings name authored element 1; that is an
editable source default, never evidence of provider availability. Input/property
binding, preview and bake remain their separate PCG workstreams. The inspector
shows stable identity, schema version, settings size, typed pins and incident
connections without interpreting opaque settings bytes.

Click a node header to select it, or Ctrl-click to extend selection. Drag headers
to move nodes, middle-drag to pan, and Ctrl-wheel to zoom around the pointer.
Shared keyboard-focusable buttons also expose zoom, reset, focus, endpoint selection,
removal, disconnect, undo, redo and save. Click an output then an input to connect;
Escape or Cancel connection aborts without a graph command. Narrow layouts stack
the three sections without reducing typography.

## Ownership and admission

`PCGGraphDocument` remains the sole source and semantic-history authority.
`PCGWorkspaceDocument` retains an immutable snapshot and separate bounded workspace
presentation: node positions, selection, pan, zoom, filter and incomplete gestures.
Presentation never enters serialized graph source, changes dirty state or creates
undo entries. Panel movement and refocusing retain it; source/catalog publication
cancels pending gestures and prunes stale identities. Closing or shutting down the
canonical session retires its projection; a new session starts fresh.

The view submits `PCGEditorRequest` through the existing workspace command route.
The controller checks active document, exact session/revision/catalog, cancellation
and finite presentation limits before accepting it. It expands node deletion into
one transaction including incident edges and exposed bindings. Creation allocates
node and globally unique pin IDs with overflow rejection, then selects the new
node at the current viewport origin. Connections use the canonical detached-source
validator before publication, protecting endpoint existence, direction, type,
cardinality, duplicate and DAG invariants. A rejected edit leaves canonical root,
revision, dirty state and history unchanged. Large deletions exceeding the existing
256-operation command budget reject atomically; there is no partial-delete path.

Undo/redo and save call the existing document services. Common workspace undo, redo
and save intents route to the active PCG session, preserving scene history. Save failures remain visible
and do not mark the graph clean. Workspace navigation, modal interruption, focus loss,
Escape and source replacement cancel transient gestures, restoring a drag origin.
No new public API, serializer, backend dependency, evaluator or graph history is added.

## Regression scope

`HoroEditorWorkspaceControllerTests` contains production-route cases for catalog
recipes, creation, selection/navigation separation, typed connections, removal
reference closure, undo/redo, stale/unauthorized/cancelled requests, malformed
presentation, replacement, close/reopen and shutdown. Headless ImGui cases draw
the same surface at narrow/wide widths in English/Turkish and dark/light themes.
`HoroEditorLocalizationServiceTests` checks every canvas and palette message.
These cases require native compilation and execution; source checks alone do not
prove them passed. Interactive pointer/keyboard, modal, high-DPI and popup-state
qualification still needs a running editor.
