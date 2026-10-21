# Viewport overlay and gizmo contributions (ABI 1.7)

`EditorViewportHost` owns the viewport contribution boundary. Native packages
negotiate ABI 1.7, the complete `registerEditorViewport` prefix and
`HORO_EXTENSION_REQUIRES_EDITOR_VIEWPORT`. The interactive composition must
advertise `editor.viewport`; headless hosts never advertise it. Older clients
retain their existing field offsets and callbacks. Existing inspector/page SDK
consumers rebuild against the appended host table; no native field is removed.

Declare `editor.viewport_overlay` or `editor.gizmo` with an exact contribution ID
and owning module. Overlay registration requires the independently approved
`editor.viewport-overlay` permission and `horo.editor.viewport-overlay` capability;
gizmos require `editor.gizmo` and `horo.editor.gizmo`. Package artifact trust,
module role validation and whole-activation rollback still run before publication.
A descriptor does not grant authority or execute callbacks.

The initial geometry profile is normalized logical viewport line segments, with
semantic color roles and thickness 1–4. Each contribution has at most 32 lines;
the host admits at most 64 contributions and 256 lines in total. Registration
rejects excess before publication. Geometry, IDs and permissions are copied at
registration. Per-frame extraction uses caller-owned fixed storage. Picking and
liveness checks retain only revocable activation state and never copy capability
IDs, allocate or call module code. GUI draw-buffer growth remains owned by the
existing GUI renderer; no provider allocation or native resource enters a frame.

Overlays are passive. Gizmos bind `horo.scene.transform`, a canonical editable
scene-transform type, and one typed Move/Rotate/Scale operation. Axis handles
select X/Y/Z. Providers do not get scene objects, setters, renderer pointers,
textures, GPU handles, raw input, callbacks or ImGui. The viewport panel resolves
current visible/unlocked selection, tool, focus, modal priority and topmost
six-logical-pixel line picking. Toolbar controls, other captured interactions and
asset drops retain their routing priority. The existing transform controller
owns axis math, local/world orientation, snapping, exclusive pointer capture and
preview/commit/cancel commands; the document owner retains undo authority.

The host sorts contribution IDs deterministically. User visibility, provider
availability, capability revocation, activation generation, selection admission
and current tool gate presentation/input. Native load stages every declaration
and requires exact manifest claim coverage; only successful aggregate activation
publishes it. Registration reset, provider retirement and host shutdown remove
future discovery/picking before provider code leases release. Already extracted
frames contain scalar values only, so GUI submissions cannot retain a module or
GPU resource. A stale pick never authorizes a new transaction. An active provider
interaction is cancelled through normal preview rollback when its publication,
permission, visibility, focus, selection or renderer is lost.

All host methods, provider revocation and panel attachment run on the editor owner
lane. Registry availability queries retain the existing bounded registry lock;
providers do not execute while that lock is held. No GPU waits or renderer-specific
branches are added. The same GUI command path is used by all concrete backends.

Regression coverage lives in `EditorViewportHostTests`, the real C11
`EditorViewportModule` loader fixture, public-header consumers, and
`ViewportPanelRenderTests`. It includes transport compatibility, denied policy,
malformed geometry, activation rollback, allocation measurement, shared frame
capacity, passive overlays, stale generations, visibility, unload/revoke, picking,
capture, focus loss and transform commit/cancellation. GPU smoke and manual
native-device checks remain separate qualification evidence.

`IWorkspacePanel::OnPresentationHidden` is an additive default no-op lifecycle
hook. The document presenter calls it when source/navigation/sequence content
replaces panel presentation. Existing panels remain source-compatible; panels
owning transient capture can override it. The viewport uses it to release only
its provider interaction and roll back its preview, preserving an already emitted
document command (which itself cancels transform preview at the document boundary).
