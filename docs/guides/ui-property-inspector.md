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
authored revision, selected element and host selection revision, plus actual asset-registry revision, style generation
and resource capability publication. Removing and reinstalling a registry with the
same generation advances the capability fence. Undo/redo can
restore content identity without reopening an earlier publication fence. Selection
A to B to A also rejects an earlier request. Reload, close and moved owners close
stale request admission. Failed validation leaves source, history, selection and
saved-state identity unchanged; Cancel changes only transient inspector values.

The selected record is copied into the host projection only on source or selection
changes. Normal draw frames use retained buffers. Exact signed/unsigned integers,
DIP lengths and normalized percentages do not pass through floating-point widgets.
Grid drawing traverses fixed arrays even when an invalid count has been staged.

The narrow non-installed `HoroUiInspectorInternal` interface shares the private
preparation/command seam with GUI and contract tests. The host resource capability
is owned by EditorServices and listed in the public-header ownership registry,
with a dedicated consumer target; no repository-wide include roots are exposed. The workspace view owns the panel for
its lifetime and releases its detached values when another document is presented.

## Available controls

- All existing `UiLayoutStyle` fields, including constraints, box edges, anchors,
  flex and bounded grid tracks.
- Intrinsic size kind, required policy and fallback extent.
- Style namespace, type class, class references, inline overrides and token IDs.
- All eight existing literal style categories, including typography/image resource
  IDs, linear RGBA, dimensions, shape, scalar, enum/flags and motion references.
- Authored visibility, enabled/focusable policy and typed initial Toggle, Slider or
  TextInput values. Live focus, pointer press and capture remain runtime-owned.
- Canonical localized Text content, message key/fallback/failure policy and all
  eight typed named-argument alternatives, with exact integral/double buffers.
- Accessibility role, localized name/description, hidden/read-only/required policy.
- Apply, Cancel and undoable removal of the presentation override.

All controls use shared editor components, fonts/theme tokens and EN/TR resources.
Unparseable text is retained and blocks Apply without substituting a numeric
fallback. Property/class selection and additions cannot discard invalid retained
text; explicit removal, category replacement and Cancel are deliberate draft edits.

Local validation uses the canonical presentation, element-kind, message, initial
control-value and accessibility-role contracts before publication. The document
owner applies the complete record through its existing transaction and inverse.

## Resources and runtime boundary

The executable composes `UiInspectorResources` per workspace through
`HoroEditorUiStyleHostInternal` and the Editor/GUI-free
`HoroUiStyleWorkspaceApplication` bridge. The factory binds the actual Assets
publisher, native storage, source lease and process scheduler. The inspector lists
real registered `core.ui_style` namespaces before any theme is active. Selecting
one starts owned source/cook/load preparation; Refresh retries the explicit choice.
There is no automatic first-asset or fabricated theme selection.

The producer captures the selected `.uistyle` and declared sidecar/dependency
closure, runs the generic resource-before-dependent cook/catalog/cache/publication
path, verifies the resulting manifest and loads through the actual Assets provider.
One selected-root default policy and core property schema fingerprint bind actual
resource envelopes. The runtime domain validates all namespace semantics through
`RuntimeStyleRegistry::Create`. The owner-thread handoff checks workspace/request,
Assets revision and candidate identity before publishing. Drawing and ordinary
polling perform no file I/O, cooking or registry construction.

The core provider owns property IDs, effects and ranges; source data only supplies
typed style declarations and optional real font/image defaults. Absent defaults
omit those resource properties while width/fill/opacity remain usable. Required
font/image dependencies need their actual domain cookers supplied by the host;
missing cookers/defaults are explicit capability failures. The producer does not
invent resource IDs, opaque replacement cookers or an editor-theme inheritance.
The validation cook target is `headless-null`, independent of rendering.

Changing a style reference/value or its initial Disabled/Checked mask revalidates
the actual cascade before mutation. Registered categories, scalar/enum ranges,
qualified tokens/classes, inheritance and sealed properties are checked. Image/font
values also match declared typed dependencies and real pinned asset records.
Pending, stale or failed preparations close editing admission while retaining
last-good immutable registry/resource leases; a successful refreshed candidate
reopens it. Existing drafts are rejected by monotonically advancing publication
fences. Cancellation and shutdown join accepted work before host services die.

The additive style source schema, cooked framing, stable core property vocabulary,
selection/default policy and migration are specified in the normative
[Runtime UI style asset contract](../architecture/runtime/game-ui-and-hud.md#runtime-style-source-and-cook-contract).
`UiStylePropertyDescriptor::maximumEnumValue` is appended with an unrestricted
32-bit default for existing flag schemas; the core ImageFit provider narrows it to
the actual closed enum. Existing consumers rebuild without changing their authored
values. Both public headers have exactly one target owner and dedicated consumer
coverage; no new generic Application dependency on Runtime was introduced.

The canonical source model owns initial state, message references and accessibility
metadata. Source schema 1.3 / presentation format 2 persist these extended fields;
unchanged old presentations retain format 1 and source 1.2 remains readable. Old
schemas reject extended values rather than silently losing them. Cooked ordinary
records use the same presentation codec and preserve the complete authored record.
No outer cooked-format migration or competing property map was added.

Core projections consume those canonical records for layout, style (including
initial Disabled/Checked), structural visibility, actual owner-bound control
descriptor initialization, resolved semantic metadata and localized content queries.
Control projection checks the exact real tree/document/action owner; it never
patches an active control. Localization resolves messages against its own pinned
catalog/locale, independently of the editor's label locale.

The existing template host consumes separate rich-template attachments and explicit
host control/semantic descriptors. This inspector does not introduce a new runtime
host, automatically rebuild active controls, or reinterpret template property IDs.
The new control/message/semantic projections define the ordinary authored-record
integration seam; application compositions must call them with their actual owners
and resolved text. Source/cooked preservation alone is not evidence of an active
runtime composition using those fields.

## Qualification

Regression sources cover detached validation, exact integers, categories/tokens,
selection/state fences, actual document transactions/history, source save/reload,
shutdown/move, state-only cascade rejection, resource publication ABA, full authored
field undo/redo/save/reload/cook and passive CPU-only shared-control frames. These are distinct from
native editor interaction, hover/focus/popup/active-state and GPU verification.
The new source regressions include actual sidecars/Registry/factory/cook/cache/
manifest/provider preparation, controller handoff and passive visible inspector
frames, semantic cache revalidation, source identity/resource failures, last-good
leases, registry drift and cancel/join. These sources are unexecuted until a
serialized qualification grant. Local builds/tests were not run without the
coordinator's grant. Hosted checks
must be evaluated at the actual pushed head; local source checks are not a build
or quality-gate receipt.
