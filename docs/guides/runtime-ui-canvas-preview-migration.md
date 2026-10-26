# UI Canvas device preview and visual layout authoring

The workspace controller derives `UiCanvasPreview` from the canonical `UiCanvasDocument` on source or device/orientation changes. It creates an isolated bounded retained tree, resolves the existing Runtime UI canvas policy from physical resolution, rational DPI and safe-area evidence, selects authored responsive alternatives, resolves the actual host-selected style registry/theme and runs `UiDeclarativeLayoutEvaluator` through `UiLayoutEngine`. The view draws copied content bounds, device/safe-content outlines, rulers and guides. This is layout inspection, not a backend pixel-rendering or asset-texture preview claim. Missing required intrinsic providers and unsupported world-space resolution return their original typed failures and remove stale interaction geometry.

Desktop (1920x1080, 1x), phone (1170x2532, 3x) and tablet (2048x2732, 2x) evidence are explicit preview presets, with clockwise orientation and rotated safe-area insets. Device, orientation, zoom, pan, tool, grid and up to eight guides per axis are workspace values. They never enter serialization, dirty state or undo history. Pan/zoom/tool/guide changes reuse evaluated layout; source/device/orientation and style resource publication/theme changes re-evaluate it outside drawing. Explicit computed width/height/padding are projected after responsive bases through `ProjectUiThemeSurface`, which delegates to the same surface projection used by `UiThemeRuntime`. The preview admits these automatic layout bindings only when the actual computed property's typed provenance differs from `RegisteredDefault`. Schema defaults are fallback metadata, so they preserve authored percentages, responsive alternatives and padding; explicit asset, class, inline, state, animation and policy values still override, including zero. This is binding admission over the sole computed snapshot, not another cascade. Generic runtime callers with explicitly declared bindings retain their registered-default behavior. Missing style capabilities or invalid references fail explicitly; the preview never creates a substitute registry.

A narrow document pane keeps a positive preview height derived from shared control metrics even when wrapped instructions consume the available height. The existing document scroll container exposes that preview; it is not silently removed. Selection uses the actual evaluated bounds in reverse paint hierarchy order. Move, resize and anchor tools operate on absolute layout elements; flow elements require an explicit positioning change in the existing inspector. Logical deltas use the scale captured at gesture start, and preview-scale/source/selection/device changes cancel the gesture. Escape, host input cancellation, project reset and document disappearance also cancel. Shift-click adds a vertical guide; Ctrl-Shift-click adds a horizontal guide; Clear guides removes them. Middle-drag pans. Grid and nearest-guide snapping happen in canvas logical coordinates; nested elements retain their authored parent-relative offsets. Resize authors content width/height, preserving box-model fields. Anchors use normalized Runtime UI units derived from parent content extents.

A completed drag also retains the selection publication revision, so selection A to B to A cannot revive an earlier gesture even when the selected ID and document state match again. A completed drag submits one `UiCanvasCommitLayout` typed intent. The controller checks the document instance/state and selection, prepares a complete canonical presentation preserving unrelated style/control/accessibility values, then calls `UiCanvasDocument::Execute`. Semantic history supplies undo/redo; reload/close fences reject old gestures. Preview failure does not change the document.

## Additive runtime API

`UiPresentationProfileTable::CreateForAuthoring(document, tree, canvas)` is the source-authoring ingress. It verifies exact document/revision, canvas root, complete source subtree membership and parent relationships before using the same canonical bounded table validation, selection and layout application as runtime `Create`. The existing cooked-generation ingress and all existing callers remain unchanged. Editors migrate to the source ingress instead of fabricating a cooked Assets closure. The existing public header remains owned by its original CMake target; the consumer contract covers the additive declaration.

`ProjectUiThemeSurface` is an additive copied authoring projection over an actual computed publication. Its existing owning header/target does not change. It delegates to the runtime theme projection, preserves typed failure, and leaves inputs untouched. Existing runtime callers retain their original path; the editor calls this public ingress rather than including private Runtime UI helpers. Public consumer and actual selected-theme regression coverage exercise both additive ingresses.

## Source registry and cooked profile integration

The canonical asset registry recognizes `.uistyle` sources and their `.uistyle.horo` identity sidecars as one asset. The prior source whitelist omitted `.uistyle`, which skipped styles and misclassified the sidecar as an ordinary `.horo` source. Native filesystem coverage includes spaces and non-ASCII source names.

Cooked document format four writes its profile trailer as a JSON array, including the neutral empty array. That trailer now uses a narrow internal array parsing ingress with the existing UTF-8, byte, duplicate-key and depth checks. Ordinary authored documents and presentation records retain object-only parsing. Canonical profile re-encoding, sorted identity/priority rules and the outer binary bounds remain unchanged. Real cook/decode and asset-loader regression targets cover both neutral and authored alternatives plus malformed trailers.

## Ticket audit boundaries

- HORO-785 / #785 / [RUI-009.3]: device, DPI, safe-area, orientation and responsive layout preview surface.
- HORO-786 / #786 / [RUI-009.4]: canonical visual layout commands, select/move/resize/anchors, rulers, guides, zoom/pan and snapping.
- HORO-752 / #752 / [RUI-006.6]: router-owned operations now survive interaction-only layout publications and the actual VariableUpdate path stages busy/progress/error copies before style preparation. Expanded current-head host/action execution remains the qualification gate.
- HORO-770 / #770 / [RUI-008.3]: existing actual overlay lifecycle tests cover modal input exclusion, default/prior focus, destroyed targets, cooked reload, suspension and shutdown. Current-head execution evidence must qualify those existing contracts before closure.
- HORO-760 / #760 / [RUI-007.4]: geometry carries clip/mask references and per-command opacity, but runtime UI geometry is retained for submission lifetime without a native backend draw workload. Nested stencil/rounded-mask/opacity-group pixel realization remains open. Layout outlines do not satisfy it.

## Private workspace ownership

`EditorWorkspaceView` value-owns `UiCanvasDocumentView`, which owns the existing inspector widget and revocable pointer gesture. The input owner forwards cancellation, and source-session teardown clears widget drafts before the source owner leaves. The adapter emits the same typed commands; it never owns canonical document history or async operations. Preview session state, its copied layout and its diagnostic are one `UiCanvasPreviewPresentation` value in the cached document projection.

`EditorWorkspaceDependencies` groups the existing individually optional world-source, world-bake and procedural capabilities. `EditorWorkspaceScreenComposition` remains the explicit host composition root. Controller and test callers migrate to these typed groups; pointer/shared-capture ownership, null-member availability checks and default catalog revision are unchanged. No public header ownership or ambient service lookup is added.

Held pointer gestures capture the document viewport extent and preview transform
origin as well as scale. A resized or repositioned document viewport cancels the
gesture even when the narrow-pane minimum keeps the preview size unchanged;
release cannot author a delta against a different visible surface.

The existing ImGui primary-pointer capture blocks keyboard activation of another
button while that pointer is held. Canvas verification does not bypass that
input-admission policy to manufacture a simultaneous toolbar rotation or reload.
Actual widget regressions cover Escape and viewport-resize cancellation, source
save/reload, device controls and reverse-paint selection; canonical session/model
fences separately cover stale presentation and source replacement.
