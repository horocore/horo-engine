# Custom GUI composition boundary

`EXT-003.15` implements the specialized-view path selected by EXT-003.1 and
ADR-056. Extensions compose standard Horo nodes with `EditorUiPlotNode`;
arbitrary drawing/layout callbacks are not an admitted capability. A plot is
normalized ordered data, not a draw command stream. The host chooses geometry,
lines, clipping, selection markers, focus visuals and typography. Further advanced
views require a reviewed typed schema rather than generic draw primitives.

## Published contract and migration

`EditorUiForm.h` remains the canonical copied presentation vocabulary and the
Extensions target remains its sole public-header owner. `EditorUiPlotNode`
adds a specialized plot with stable node identity, localized accessible label,
normalized samples and one admitted semantic action. Existing callers are the
activity C transport, the registry permission validator, the common headless
projection and the private editor adapter. Settings/modal and inspector profiles
continue to reject unsupported specialized nodes explicitly.

The activity snapshot outer struct and the descriptor embedding it retain their
binary sizes and offsets. Snapshot schema1 retains its original fixed-stride
`HoroEditorActivityNode` array. Schema2 uses a fixed-stride
`HoroEditorActivityNodeV2` array, passed through the unchanged `nodes` pointer;
the receiver casts back only after checking the explicit schema version. Every
record contains its v1 node and typed sample pointer/count. Non-plot nodes carry
no samples. All bytes are copied and validated before publication. A provider
requires the PLOT bit in `supportedNodeMask`; old hosts reject schema2 before
attachment. Existing schema1 packages require no migration. Publishers upgrade
all array elements together; mixing strides is invalid. No engine-version string
or C++ ABI inference substitutes for the schema negotiation.

## Ownership, input and domain commands

Only the application owns `EditorActivityHost`, surface contexts and native-code
retirement. Initial action/localization allowlists come from the copied initial
form; later snapshots cannot add unapproved actions. Registration still requires
the manifest activity/panel claims and `editor.activity` admission. Plot activation
uses the same exact provider/node/action/revision envelope and worker queue as a
standard action. It cannot write document values, files, undo history or layout.
Project-affecting work must use the owning application command/undo capability;
a plot activation grants no additional mutation capability. Native modules remain
trusted native code, not a memory sandbox.

The workspace input router supplies the host admission decision before pumping
queued actions and before drawing. Exclusive modal ownership discards queued
workspace interactions. The host performs button hit testing, focus traversal,
left/right sample navigation, pointer selection and keyboard activation. There
are no global event subscriptions, shortcuts or module callbacks in drawing.
Disabled nodes and their ancestors cannot activate a plot or standard action.

Selection and bounded focus animation live in the host entry, keyed by stable
node identity within one exact provider generation. Locale changes do not change
identity. State is discarded when the entry disappears; it is not persisted as
project data or used as capability evidence. A text sample index/value supplements
the marker so selection is not encoded only through color. The public headless
projection exposes the plot kind, stable ID, focus order, semantic token roles and
complete immutable theme alongside the copied sample data. Native screen-reader
integration remains separate from these test-visible semantics.

## Theme, resources and budgets

Every frame reads the shared `EditorThemeFrame` captured from the active editor
palette and geometry. Fonts, icons, clipping and renderer resources remain host
owned. The activity icon uses the existing verified SVG resource lifetime; the
plot uses host font roles and geometric markers without adding an atlas or GPU
resource. Theme revision, DPI, high-contrast roles and reduced-motion metadata
use the existing token contract. Unsupported motion resolves to instant; an
extension cannot choose a private animation clock or override accessibility.

Each plot has 2..256 finite normalized samples with strictly increasing x.
The existing form budget admits at most 256 nodes and depth 16, bounding total
sample storage to 65536 pairs per form. Activity registration is capped at 128
sessions. Text and command allowlists retain their existing byte/count limits.
Schema updates allocate/copy outside draw traversal. Steady plot traversal uses
one fixed presentation record per node, a bounded sample loop, stack text and
host draw buffers; it creates no extension job, heap container, resource or
subscription. No performance measurement is claimed by these bounds.

Disable, shutdown and failure revoke action admission before detaching both
activity and drawer, cancel pending work, invalidate contexts and reject late
results. Native executable leases remain pinned until scheduled callbacks drain.
A callback failure discards any previously copied output and revokes its surface;
it cannot leave a partially updated custom widget live. Layout persistence stays
with the existing registry and no custom docking/input ownership is installed.

## Reference and evidence

`tests/fixtures/extensions/EditorActivityModule.c` is an actual loaded C package
fixture combining Horo text/action controls and a schema2 plot in one drawer.
The editor activity-view tests draw that fixture, including narrow geometry and
provider retirement. Form tests exercise live theme/DPI projection and bounded
keyboard/pointer/reduced-motion state. Native boundary tests reject legacy plot
injection, malformed/unbounded samples and verify copied ownership. Activity host
tests cover modal withdrawal, stale/replayed actions and retirement.

Build the existing `HoroExtensionManagerTests`, `HoroExtensionsPublicHeaderConsumer`
and editor UI test target to qualify this change; GUI tests require an available
editor composition. Source inspection is not a manual interaction or native-device
validation result.
