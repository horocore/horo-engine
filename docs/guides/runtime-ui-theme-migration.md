# Runtime theme switching and hot reload

`HoroEngine::RuntimeUi` owns `Horo/Runtime/Ui/UiTheme.h`. Existing
`RuntimeStyleRegistry` and `UiStyleResolver` callers keep their contracts. A
canvas that needs atomic theme switching can compose `UiThemeRuntime` instead
of publishing its style, layout and render stages independently.

The theme runtime transfers ownership of the initial validated registry and
reserves the style resolver, declarative layout engine, render extractor and
component scratch at creation. Its style/layout instance, canvas, document and
element capacity must match. The host supplies the exact render view. There are
no editor, platform or concrete renderer dependencies.

Surface components supply revision-frozen styles, layout/intrinsic descriptors
and solid paint defaults in retained-tree preorder. `UiThemeSurfaceProperties`
binds registered property IDs to width, height, uniform padding, fill and opacity.
An absent binding preserves the component's authored value. Dimension bindings
require non-negative measure properties; fill/opacity require paint properties,
with opacity constrained to [0, 1]. All remaining registered computed properties
remain available in the immutable style snapshot for other component consumers.

An absent element style asset selects the current theme. Class references with a
class ID and absent asset select that theme's class namespace. Fully qualified
asset/class references remain explicitly independent of theme selection. Inline,
visual-state and host policy precedence remains owned by the existing resolver.

The host first publishes the initial frame using `Update`. For a switch or asset
hot reload it calls `BeginChange`, loads/cooks a complete definition outside the
frame path, then delivers that definition through `Prepare` with the exact
returned ID. The host owns file watching, asset I/O and its loader cancellation;
the theme runtime owns admission and candidate lifetime. All calls run on the
canvas owner thread. No asynchronous callback, job or borrowed input survives a
call. Each preparation attempt consumes a fresh registry generation, including
retries of the same request.

Removed tokens fail validation when still required. A host may explicitly declare
`UiThemeTokenFallback` literals for removed tokens from the active registry.
Fallbacks must preserve category, cannot override a present or sealed token, and
cannot invent a namespace. The resulting snapshot reports the number of fallback
tokens. There is no guessed transparent color, zero size or editor-theme fallback.
Missing classes, assets, properties, malformed values and cycles still reject the
candidate. A failed `Prepare` discards any previous ready candidate for that request
and can be retried with its ID.

At the VariableUpdate safe point, supply the prepared ID in `Update`. The runtime
resolves styles, projects the actual surface component descriptors, measures and
arranges them, and extracts solid paints from their arranged border boxes. Only
after all stages succeed does it replace the active registry and `Current` with
one `UiThemeSnapshot`. Render uses the exact published layout interaction revision.
Paint-only changes reuse the layout generation; geometry changes advance the
owned layout style revision. Unchanged frames reuse the publication. Fixed storage
exhaustion returns the original stage error and never allocates fallback storage.

The three stage caches are private preparation state. They may advance during a
failed outer transaction, but consumers can only observe the last-good combined
snapshot. `Current` and the active theme remain unchanged on failure. A retry
reuses the ready candidate; cancellation followed by an ordinary update resolves
the active registry again. No gameplay, tree, selection or control owner is rebuilt.

`Cancel`, supersession by `BeginChange`, retirement and shutdown reject late
preparation/publication by exact request ID. `BeginRetirement` closes admission
while retaining the last-good publication. `Shutdown` is idempotent, releases
mutable tables and internal leases, and leaves external style/layout/render
snapshots alive until their readers retire. After shutdown, `IsDrained` reports
whether those external leases remain. Replace the canvas/view owner for viewport
or scope replacement rather than delivering old requests into a new incarnation.

The new header is assigned to the RuntimeUi public header boundary and covered
by the generated `HoroRuntimeUiPublicHeaderConsumer` plus
`HoroRuntimeUiThemePublicHeaderConsumer`. The gameplay and extension SDK contracts
are unchanged; they do not export RuntimeUi. Renderer qualification remains the
existing backend-neutral snapshot consumption contract; the theme tests exercise
headless semantic layout and extraction, not GPU pixel output.
