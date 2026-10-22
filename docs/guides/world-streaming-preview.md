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

World Composition exposes an explicit project-relative `world.index` path and
Mount/Stop actions. The controller reads exact bytes through its retained project
file authority, enforces the artifact/time bounds, decodes and verifies the
canonical index, and requires the active source partition and supported coordinate
convention. It prepares a `WorldStreamingPreviewRuntime` privately. That owner
instantiates the actual canonical registry, source planner, source registrations,
scheduler and residency ledger. This is the sole inspection streaming owner; it
does not duplicate the viewport, camera or editable document authority.

The runtime uses the application-composed bounded inspection profile and the
native steady clock. It registers the canonical authored relevance sources with
the real planner and services finite quanta. Only after an actual publication is
ready does the controller call `BindRuntime` with this owner's actual registry,
planner and diagnostic value. Explicit stop, document replacement/close and
workspace teardown call `UnbindRuntime(runtime.Owner())` before native shutdown.
Replacement prepares the new owner first, unbinds the exact old generation,
binds the candidate, then retires the old owner. Failed parse/preparation preserves
the prior active mount. Stale unmounts cannot revoke a newer owner. Source changes
withdraw actual native registrations before recomputation and invalidate the old
planner publication.

Inspection does not perform cell I/O or activation and does not mutate packaged
policy. Actual residency is read from the canonical ledger; absent records remain
unobserved rather than fabricated Unloaded/Resident/Active. The owner has no
payload-budget observation capability, so its canonical state-only diagnostic
snapshot retains no budget policy or sample. No measured zero or fabricated
sampling window is substituted. An external native world with full observations
may still bind its actual canonical owners to the same bridge.

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
`WorldStreamingDiagnosticSnapshot::BudgetPolicy()` and `BudgetSample()` now return
nullable const pointers. This deliberate public API evolution prevents inspection
from inventing policy/sample facts. The only repository getter callers were two
assertions in `WorldStreamingDiagnosticSnapshotTests.cpp`; they now verify presence
before dereferencing. New native tests require both getters to be null for the
state-only capture. Existing measured-budget `Create`/`Replace` inputs retain their
strict validation. Public-header consumer coverage includes these pointer contracts,
the additive ledger-record view and the immutable bridge result. Native types and backend handles do not cross this boundary.

## Verification limits

Regression sources exercise native file-to-index mount/replacement/unbind, real planner publication and residency-ledger failure
transitions, ownership after unmount, source withdrawal, shutdown, wrong-thread
access, stale/invalid/capacity/cancellation rejection, controller detach, canonical
origin geometry and both supported locales. No local configure/build/test or GPU
run is authorized for this freeze; hosted checks and manual viewport qualification
are separate evidence requirements.
