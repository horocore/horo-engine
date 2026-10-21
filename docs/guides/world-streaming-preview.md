# Viewport streaming preview integration

`WorldStreamingPreview` is an EditorModel-owned immutable projection. It joins the
canonical controller document lease with an exact mounted registry, source plan
and runtime diagnostic publication. It owns bounded row copies and retains the
source lease; it does not register sources, change packaged policy, or infer
residency from an open authoring page.

## Actual host capture and viewport route

`HoroEditorApp` owns and registers one `WorldStreamingPreviewHost`. The workspace
screen borrows that instance through `EditorWorkspaceDependencies`. Each controller
refresh reads the native capture, using its existing `WorldViewportBinding` and
canonical document owner. The registered `ViewportPanel` consumes the resulting
immutable geometry. It does not query runtime services or allocate cell geometry.

A native mounted-world producer supplies its existing owners to:

```cpp
bridge.BindRuntime(registry, planner, diagnostics);
```

`registry` is the actual `WorldPartitionRegistry`; `planner` is the actual
`StreamingSourcePlanner`; `diagnostics` is the authority-owned
`WorldStreamingDiagnosticSnapshot` whose address remains stable while its revision
advances. All three remain alive until the native mount calls
`bridge.UnbindRuntime(exactOwner)` before destroying them. Bind, capture, publication
replacement and unbind occur on the bridge's constructing lane, after the streaming
safe point and before editor presentation. The bridge reads `Snapshot()` and
`Plan()` directly, and checks their owner/publication correlation. It owns no
planner, source registration, camera, residency ledger, package policy or loader.
A different mounted owner requires explicit unbind; stale unmounts cannot revoke a
new owner. Failed capture clears the displayed cache while retaining native
anti-rollback revision floors. Historical immutable leases remain readable.

Opening `.horoworld` source metadata does not mount cooked runtime content. An
unbound native runtime is explicitly unavailable, not an empty observation asserted
as unloaded. This repository's source-open path has no native mount invocation:
that mount must call the contract above when it exists. This is a production
producer prerequisite, not evidence that the source-open route supplies residency.
The PR must remain draft without closing HORO-1634 until that live producer and
current-head validation have been qualified.

## Evidence and geometry

The join preserves desired state, pinned floor, priority/stability, current and
predicted source contributions, distinct actual cell-attempt generations, typed
failure causes and authority-authored decision events. Missing plan rows remain
absent demand; missing attempts remain unknown actual state. Disabled event
instrumentation does not suppress failure causes.

The controller rebuilds only when native/document/view publications change.
Geometry is capped at 256 local boxes, with no partial result on capacity failure.
Corners use the same exact global-origin conversion as source tools. Cells outside
the local precision window are counted explicitly. The viewport checks the exact
source lease, global origin and view revision before drawing. Desired boundaries
and actual-state dots have separate visual roles; bounded localized tooltips show
source, attempt, failure and decision evidence. Close/detach/unmount or failed
capture clears current presentation. The source working set and packaged policy
are unchanged.

## Public API migration

`WorldPartitionRegistrySnapshot::CellBounds(handle)` is additive. It validates the
exact handle and returns the existing spatial index's inclusive millimeter bounds.
New viewport consumers reuse that authority instead of repeating grid/LOD/overflow
arithmetic. Existing registry consumers need no migration. Public ownership remains
WorldStreaming; projection, geometry and the host bridge belong to EditorModel.
Public-header consumer coverage includes the additive API and immutable bridge
result. Native types and backend handles do not cross this boundary.

## Verification limits

Regression sources exercise real planner publication and residency-ledger failure
transitions, ownership after unmount, source withdrawal, shutdown, wrong-thread
access, stale/invalid/capacity/cancellation rejection, controller detach, canonical
origin geometry and both supported locales. No local configure/build/test or GPU
run is authorized for this freeze; hosted checks and manual viewport qualification
are separate evidence requirements.
