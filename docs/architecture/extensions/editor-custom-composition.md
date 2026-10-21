# Custom GUI composition boundary

`EXT-003.15` follows the closed, host-rendered semantic schema policy selected by
EXT-003.1 and ADR-056. `EditorUiForm` is the only composition graph. Advanced
extensions select supported presenters and semantic resources; they do not install
executable layout, paint or input callbacks. Extending the catalog requires a
reviewed typed schema and a host adapter.

## Contract and migration

`EditorUiCustomPresentation` decorates canonical nodes with a font role, icon role,
accent role, motion role and bounded theme-relative control rows for the plot and
scalar presenters. Other node/profile pairings are rejected. The scalar dial
is a presenter for an existing decimal `EditorUiNumberNode`, with finite minimum
and maximum; it introduces no competing value or binding authority. A plot is
2..256 copied normalized samples with strictly increasing x and one declared
action. The same form can contain standard Horo controls and custom presenters.
Stack, Group, Row and Grid use the common validated layout model. The host owns
node bounds, clipping, typography, hit testing and focus traversal.

The Extensions target remains the sole public-header owner. No new public header
or repository-wide include path is introduced. Existing schema1 activity snapshots
retain their original outer size, offsets and node stride. Snapshot schema2 uses
`HoroEditorActivityNodeV2` for every array element, passed through the unchanged
node pointer. The receiver checks the version before recovering that stride.
Schema2 adds copied presentation, description/accessibility keys, Grid columns
and optional plot samples. Mixed strides are invalid. The session advertises
supported node bits; old hosts reject schema2. Schema2 was introduced by this
change and has no released intermediate record layout to preserve.

Inspector schema1 retains its exact descriptor and field strides. Inspector
schema2 extends the descriptor prefix with a bounded list of styles keyed by
existing node ID; presentation nodes use the activity schema2 stride. It does not
extend the scalar field array. Missing or duplicate style targets, unsupported
versions/roles and incompatible presenters are rejected. Existing schema1 callers
require no migration; schema2 producers must pass the complete extended descriptor.

## Admission, commands and input

The application explicitly advertises native `editor.custom-composition` support
and seals `horo.editor.custom-composition` into its UI admission policy. Declaring
standard activity or inspector support does not imply this grant. Advanced native
packages must declare the feature in their module requirements; the host separately
checks actual capability admission and exact activation generation. Registry form
publication also checks the custom grant. Inspector discovery and drawing recheck
live publication, provider status and required capabilities.

Plot activation uses the standard exact provider/node/action/revision envelope,
command allowlist and worker queue. It supplies no document value or document
pointer. A scalar gesture retains a host-local preview and proposes a typed value
only on commit. `InspectorContributionView::Queue` uses the existing approval and
owner fences, then the workspace controller executes the canonical inspector
document transaction. Both inspector and custom-composition capability leases remain held and are
rechecked through transaction publication. The same existing history owner performs
undo/redo. Providers
cannot replace the transaction or write authored state through presentation data.
Native modules remain trusted native code, not a memory sandbox.

The workspace input router supplies the actual modal admission decision to both
activity and inspector adapters. Losing input drops pending actions and scalar
previews. Native disabled scopes apply only to the contribution controls, preserving
modal controls. Keyboard arrows are owned by the focused native widget; pointer
selection/dragging is scoped to its host hit target. Disabled ancestors cannot
activate controls. There are no custom global shortcuts, event subscriptions or
provider callbacks in draw traversal. Stable identity combines provider generation,
surface, node and the inspector selection epoch. Locale/theme changes do not alter
node identity; provider/selection/document replacement discards local drafts. A host
frame-continuity fence also cancels a drag when its control was hidden; reopening
a panel cannot commit a stale preview.

## Theme, accessibility and budgets

The live `EditorThemeFrame` supplies colors, metrics, font/icon roles, DPI and motion.
The private resource adapter borrows only existing host fonts and the canonical
host icon registry; no atlas, renderer/platform handle or provider texture enters
the custom schema. Unsupported roles resolve through the shared fallback contract.
Motion durations and reduced-motion policy come from the selected editor theme;
reduced motion resolves immediately and cannot be overridden by a provider.

The common render snapshot contains a semantic accessibility record per node:
stable ID/parent, localized name and description intent, role, enabled/read-only
state, focus order and current typed value. Plot selection and scalar preview
update preallocated records. Host views expose borrowed live records and withdraw
them on retirement. These are accessibility hooks, not a claim that a native
screen-reader adapter has been implemented or manually qualified.

Default per-form custom budgets are 16 custom nodes, 4096 plot samples, 4096 bytes
of charged transient state and 8192 semantic draw segments. Hard ceilings are
64 nodes, 16384 samples, 65536 state bytes and 32768 segments; hosts may tighten
the default limits. The normal form limits remain 256 nodes and depth16. Custom
admission charges the aggregate before host publication or render preparation.
Text, actions, registrations and native callback queues retain their existing
separate bounds. Style records are copied at registration, and layout/semantic
caches rebuild only on declaration, width or theme changes. Activity layout rebuilds
at owner Update; inspector layout preparation reuses its cached snapshot until an
input changes. Custom state vectors allocate only at projection capture. Steady
widget preview/animation uses bounded loops, stack text and existing host draw
buffers. Layout cache keys retain the offered physical width directly, avoiding
DPI round-trip drift. Activation/commit leaves the pure model: existing host command
envelopes may copy bounded node/action IDs or selected inspector targets once per
gesture, under their existing queue/selection limits. No additional callback, job, GPU resource or subscription is created by drawing.
These are structural budgets, not a measured frame-time claim.

Failure, disablement, shutdown and retirement withdraw input/publication before
releasing resources. Outstanding native callbacks retain their executable lease
until they drain; late output is rejected. A failed callback discards copied output
and revokes its activity and drawer together. Custom UI does not install a second
docking/layout persistence owner. The existing SVG activity-icon RAII lifetime is
unchanged; custom presenters add no owned GPU resource.

## Acceptance and qualification map

| Requirement | Implemented boundary and regression coverage |
| --- | --- |
| Custom layout/drawing with standard controls | Common Row/Grid layout, host-clipped plot/scalar painters; actual C activity fixture combines Row, Horo action/text and styled plot. |
| Input, animation and local state | Pure bounded plot/scalar gesture models, host focus/key ownership, theme motion policy; FormKit state and allocation-failure regressions. |
| Tokens, fonts, icons, clipping and DPI | Shared theme resolution and private borrowed resource adapter; projection tests and actual activity live-theme/narrow-width GUI tests. |
| Focus, accessibility and stable identity | Native item identity plus semantic snapshot/hooks; plot/scalar value, focus and selection/retirement tests. |
| Capability admission | Separate native feature and sealed custom grant; real native activity/inspector packages reject missing host grants. |
| Document commands and undo | Actual scalar GUI gesture reaches the approved workspace transaction; test checks unchanged owner before dispatch, one revision, undo and redo. |
| Modal ownership | Input withdrawal cancels pending activity actions and scalar previews; native GUI regression checks no commit after withdrawal. |
| Failure, disable and unload | Existing callback failure/cancellation/late-result and live provider retirement tests; views recheck current host availability. |
| Budgets/resources/subscriptions | Aggregate admission limits, bounded samples/state, no executable presenter callback or subscription API; allocation-forbidden scalar model and stable fractional-DPI activity-cache tests. |

Qualification targets are `HoroExtensionManagerTests`, `HoroEditorActivityBoundaryTests`,
`HoroEditorActivityTests`, relevant editor activity/inspector/page tests and affected
public-header consumers. Hosted and local validation evidence belongs in the PR;
this contract does not assert that tests, native-device checks, screen readers or
manual visual states were run. GPU/device validation requires an available editor
composition and separate evidence.
