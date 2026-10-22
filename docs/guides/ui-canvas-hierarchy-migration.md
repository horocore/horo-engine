# UI Canvas hierarchy and typed presentation

HORO-784 makes `UiCanvasDocument` the command authority for retained UI creation,
selection, hierarchy moves, ordering and authored presentation edits. The workspace
view consumes cached rows and returns IDs plus an exact `UiCanvasDocumentStateId`;
it never owns or mutates Runtime UI source data. The cache refreshes after source
state/search changes. Drawing clips the at-most-4096 rows to the visible range.
Hierarchy depth is at most 64; semantic history admits at most 128 transactions.
Exhaustion is an explicit error rather than eviction of unacknowledged history.

`UiElementCatalog` in `HoroEngine::RuntimeUi` implements the ADR-176 kind vocabulary.
Its inert records own the `horo.ui.*` type mapping and child-containment rules.
The editor palette enumerates those descriptors. Numeric kind values are explicit;
serialized source continues to identify a type through the single durable `type`
token. Unknown core types fail validation. Existing non-core document types retain
their old serialization behavior; this does not register or activate a package type.

`UiDocumentElement::presentation` is the optional, document-owned source of layout,
intrinsic and inline style values. It uses `UiLayoutStyle`, `UiLength`,
`UiLayoutIntrinsicSource`, `UiStyleAssignment` and `UiStyleValue` directly. It stores
no runtime handle, provider, callback, editor selection, viewport or backend data.
A presentation record and legacy generic properties cannot coexist on one element;
there is no precedence rule or guessed string-to-property migration. Core element
records do not admit generic properties. Legacy property-bearing elements require
an explicit caller-owned conversion before acquiring a presentation record.

The local validator admits only bounded, valid layout/intrinsic values, exact style
references, unique classes and unique inline property IDs. Inline authoring cannot
seal host policy. RuntimeStyleRegistry remains responsible for resolving class,
token, property category and inheritance compatibility against the actual host
registry. Typed presentation introduces no alternative style-property ID list.

`ProjectUiElementLayout` copies authored values into the actual declarative layout
input. `ProjectUiElementStyle` produces the existing style input and borrows source
class/assignment spans only until synchronous resolution returns. The caller must
retain the immutable document for that interval. `ProjectUiElementSurface` combines
those inputs for `UiThemeRuntime`; the existing host-supplied
`UiThemeSurfaceProperties` continues to map computed width/height/padding/fill/
opacity to layout and solid paint. Text, imagery and interactive kinds keep their
own intrinsic, rendering and control owners; they are not coerced into solid paint.
Primitive creation authors descriptors and never activates gameplay actions.

Source schema 1.2 adds `presentation`; its nested codec version is explicitly 1.
Absent presentation retains the legacy neutral default. A typed edit/creation
rebuilds a detached candidate at schema 1.2. Schema 1.0/1.1 candidates containing
the new record fail rather than silently losing values. The shared presentation
codec encodes all canonical fields, including 16 slots for each grid axis, and
bounds each record to 256 KiB. Cooked format 3 adds a presence byte and bounded
presentation payload per element; decoders retain format 1/2 support with absence.
No raw C++ memory layout is serialized.

The `.uicanvas` editor envelope is now version 2, wrapping exactly one canonical
Runtime UI source document. Version 1 canvas/dependency-only files remain readable.
New saves use version 2, so retained elements, routes, localized data and typed
presentation are no longer lost. Old editors reject the new envelope. There is no
parallel editor copy of the Runtime UI source fields. A first hierarchy creation
materializes legacy canvas roots in the same atomic transaction; undo restores the
original canvas-only model.

`UiCanvasDocument::Execute(expectedState, UiCanvasSetPresentation{element, value})`
is the inspector integration boundary. An absent `value` explicitly clears the
record. The owner validates and rebuilds privately, then publishes once. Every
failure preserves current source, selection and history. Undo/redo retain semantic
inverse operations and advance the notification fence while restoring semantic
content identity for dirty tracking. Reload clears history/selection and invalidates
old requests. The command fence includes the exact `DocumentInstanceId` as well
as the monotonic publication value; history content IDs never serve as command
fences. Reopening allocates a new workspace instance. Layout-only edits require
no style registry or assignment. Close rejects dirty state unless the caller chooses discard; closed
owners reject all commands and queries requiring source data.

Affected consumers include the UI Canvas owner/workspace, Runtime UI source and
cooked codecs, public header consumers, RuntimeUi theme/layout/style projection,
and callers copying `UiDocumentElement`. Existing aggregate initializers remain
source-compatible because the optional field is appended. Existing generic
serialization callers preserve absence; copies must preserve the complete record.
Regression coverage includes source/cooked round trips, actual theme/layout/paint
consumption, malformed/duplicate/version/bounds rejection, hierarchy cycles,
stable ordering, stale commands, semantic history, reload and shutdown.
