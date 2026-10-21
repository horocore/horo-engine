# UI presentation inspector integration

HORO-787 consumes the canonical UI Canvas producer from HORO-784. The inspector
is available beside the active UI Canvas hierarchy, with a vertically stacked
layout when the available width cannot accommodate both regions.

## Ownership and commands

`UiDocumentElement::presentation` remains the sole authored presentation record.
The inspector owns detached values and bounded input text, not a document, history,
registry, dirty flag or runtime instance. Apply prepares `UiCanvasSetPresentation`
and routes it through the controller to `UiCanvasDocument::Execute`.

The command checks document instance, the monotonically advancing `CurrentState`,
authored revision, selected element and host selection revision. Undo/redo can
restore content identity without reopening an earlier publication fence. Selection
A to B to A also rejects an earlier request. Reload, close and moved owners close
stale request admission. Failed validation leaves source, history, selection and
saved-state identity unchanged; Cancel changes only transient inspector values.

The selected record is copied into the host projection only on source or selection
changes. Normal draw frames use retained buffers. Exact signed/unsigned integers,
DIP lengths and normalized percentages do not pass through floating-point widgets.
Grid drawing traverses fixed arrays even when an invalid count has been staged.

The narrow non-installed `HoroUiInspectorInternal` interface shares the private
preparation/command seam with GUI and contract tests; it introduces no public API
headers or repository-wide include roots. The workspace view owns the panel for
its lifetime and releases its detached values when another document is presented.

## Available controls

- All existing `UiLayoutStyle` fields, including constraints, box edges, anchors,
  flex and bounded grid tracks.
- Intrinsic size kind, required policy and fallback extent.
- Style namespace, type class, class references, inline overrides and token IDs.
- All eight existing literal style categories, including typography/image resource
  IDs, linear RGBA, dimensions, shape, scalar, enum/flags and motion references.
- Apply, Cancel and undoable removal of the presentation override.

All controls use shared editor components, fonts/theme tokens and EN/TR resources.
Unparseable text is retained and blocks Apply without substituting a numeric
fallback. Property/class selection and additions cannot discard invalid retained
text; explicit removal, category replacement and Cancel are deliberate draft edits.

Local validation uses the canonical presentation and value contracts before
publication. An explicitly supplied registry additionally checks registered property
categories, ranges and token resolution in the detached preparation helper. The
current workspace has no composed style-registry capability: resource IDs remain
authored references and availability/category resolution stays with the runtime
resolver. The UI says so; it does not construct a synthetic registry or claim that
an ID-shaped value proves resource availability.

## Remaining HORO-787 acceptance

This change is a draft delivery and does not close HORO-787. The current canonical
presentation record has no per-element localization or accessibility authoring
fields and no authored interactive-state producer/validator. The inspector does
not invent property keys or another state authority to represent those categories.
Their canonical producers and runtime projections must be delivered before full
ticket acceptance. Resource availability and registered-schema validation also
require a real host-composed capability before they can be guaranteed at editing.

## Qualification

Regression sources cover detached validation, exact integers, categories/tokens,
selection/state fences, actual document transactions/history, source save/reload,
shutdown/move and passive CPU-only shared-control frames. These are distinct from
native editor interaction, hover/focus/popup/active-state and GPU verification.
Local builds/tests were not run without the coordinator's grant. Hosted checks
must be evaluated at the actual pushed head; local source checks are not a build
or quality-gate receipt.
