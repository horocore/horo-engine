# Responsive Presentation Profiles

- **Decision status**: Proposed; this change records a reviewable contract, not
  human acceptance of an ADR or evidence that the complete runtime host exists.
- **Issue**: [RUI-002.9](https://github.com/horocore/horo-engine/issues/713)
- **Jira**: [HORO-713](https://horo-engine.atlassian.net/browse/HORO-713)

## Authority And Current Implementation

This document is the single decision source for responsive presentation profiles.
[ADR-073](../../adr/073-runtime-ui-ownership-scope-and-update-order.md) retains
semantic ownership, identity and frame ordering;
[ADR-074](../../adr/074-runtime-ui-layout-units-and-measure-arrange.md) retains
layout units, constraint precedence and measure/arrange;
[ADR-080](../../adr/080-runtime-ui-presentation-scope-layer-and-route.md) retains
routes, audience, visibility and presentation bands. Profiles specialize their
inputs; they introduce none of those authorities again.

At committed main `05141339924630b8df8e5c6129b114ccd727b6dd`,
`UiLayoutStyle`, `UiLayoutEngine`, `UiResolvedScreenCanvas`,
`UiDocument` source schema 1.1 and `CookedUiDocument` payload format 1 exist.
There is no responsive profile table/selector in those public contracts, nor a
shipping aggregate `RuntimeUiService` implementing the complete ADR-073 lifecycle.
The actual document cook implementation is `src/runtime/ui/UiDocumentCook.cpp`;
this decision does not posit an existing `UiAssetCook` service. Names introduced
below describe the required Horo-owned contract for dependent implementation,
not headers added or APIs already callable by this documentation change.

## Decision: Constraints Plus Finite Typed Layout Variants

Ordinary resizing uses the existing base constraints, intrinsic measurement,
wrapping, flex/grid and anchors. A profile is required only when product intent
changes a discrete layout policy: a horizontal inventory becoming a vertical
stack, a split-view HUD using a compact grid, or larger minimum control targets
for declared touch or TV presentation. Width alone cannot establish touch
availability or a living-room presentation policy. Constraints do not currently
select container kinds/orientation from typed host evidence.

A document may therefore declare one finite profile table over its existing
stable elements. Each selected profile supplies complete `UiLayoutStyle` values
for a subset of `UiElementId` values. Untouched elements use the base style.
There is exactly one selected profile, or the neutral base, per canvas attachment.
Profiles are alternatives, not stacked layers, cascades or inherited variants.
They do not change ADR-074 precedence after the effective style is selected.

A profile cannot insert/remove/reparent elements, change element/control kind,
replace an action/binding/asset/locale reference, change intrinsic provider,
create a route, alter owner scope/audience/band, or hide/disable a control.
Visibility and semantic content changes use their existing explicit authorities.
This restriction keeps stable semantic state, text, accessibility and control
identity independent of responsive geometry. Alternative semantic screens remain
ordinary route/document composition, not a back door through layout profiles.

## Typed Data And Ownership

Required boundaries are Horo-owned finite value types:

| Value | Owner and meaning |
|---|---|
| `UiPresentationProfileId` | Stable nonzero authored identity, local to one document; never a runtime element handle. |
| `UiPresentationProfileTable` | Immutable document-owned rows, priorities, selectors and element/style overrides; retained by its exact cooked revision. |
| `UiPresentationProfileEvidence` | Immutable attachment-owner input with generation/revision and the admitted canvas logical content extent. |
| `UiPresentationProfileSelection` | Derived selection for an exact document/canvas/attachment/evidence revision; transient, never serialized or used as an owner identity. |

The application/viewport owner supplies resolved evidence through a narrow
Runtime UI capability. Renderer/platform adapters may normalize facts for that
owner, but Runtime UI never queries devices, native windows, backend names or an
editor widget. The same selector consumes explicitly supplied evidence in runtime,
editor preview, CLI/headless qualification and each renderer-peer composition.

Evidence includes positive logical safe-content width/height, a closed form-factor
classification (`Unknown`, `Desktop`, `Handheld`, `Television`), a finite set of
admitted interaction capabilities (`Pointer`, `Keyboard`, `Gamepad`, `Touch`),
and a view role (`Single`, `Split`). These are independent facts: handheld does
not imply touch, television does not imply gamepad, and split view does not imply
handheld. Unknown form factor is legitimate absence of classification, not a
platform name or a guessed category. Interaction evidence describes available
admitted modalities, not the last input event; pointer movement cannot repeatedly
switch a touch-capable UI back to a different profile.

Screen-space extent is taken from the existing `UiResolvedScreenCanvas` after
scale/safe-area resolution. Profiles do not reapply DPI, safe area, UI scale,
font scale or pixel snapping. Split-screen extent is the exact local attachment's
resolved content rectangle, not the full display or another player's rectangle.
World-space canvases use their authored logical extent and explicitly supplied
presentation classification; they never infer a profile from projected pixel size,
camera distance or native display evidence. Different viewports may select different
geometry for the same semantic instance without transferring its ownership.

## Deterministic Selection And Fallback

A row contains a stable profile ID, unique unsigned priority and a selector.
Selectors are conjunctions of optional half-open width, height and width/height
aspect-ratio intervals; optional exact form factor and view role; and required
interaction capabilities. An omitted condition imposes no restriction. Unknown
form factor cannot satisfy an exact non-Unknown condition. Interaction requirements
match only when all required capabilities are present. An empty requirement set
matches every admitted capability set.

Extent bounds and evidence use the exact existing `UiCanvasLogicalExtent`
positive `int32_t` raw units at 1/64 DIP. The existing canvas resolver owns checked
integer/rational conversion and ties-to-even quantization; profile selection copies
that canonical result without another conversion, quantization or DPI/scale pass.
Floating logical dimensions are not accepted at this boundary. Lower bounds are
inclusive and upper bounds exclusive. Aspect bounds have positive reduced
`uint32_t` numerator/denominator and use checked `uint64_t` cross-products against
positive raw extents. Compare `width * denominator` with `height * numerator`;
the common 1/64-DIP scale cancels. The admitted operand bounds fit those products,
but every conversion/product still checks representability before comparison.
There is no floating division, epsilon, saturation or backend-pixel comparison.
Missing upper bounds mean unbounded within the admitted logical domain.
Negative extents, zero denominators, reversed/empty intervals, unknown enum values,
unsupported capability bits and unrepresentable comparison products reject the
candidate table/evidence with a typed error. Overflow is not a failed match.

Validate the complete table first, sort rows by increasing unique priority, then
choose the first row whose complete selector matches. Profile ID and source vector,
file, hash-map, thread or renderer order are never tie-breakers. Overlapping
selectors are intentional only with explicit distinct priorities; duplicate
priorities or IDs reject admission. A breakpoint belongs to the interval beginning
there; there is no hidden epsilon, hysteresis, cooldown or previous-selection bias.

The neutral base layout is mandatory and is the only automatic fallback. If no row
matches, select base successfully and report that fact in the typed selection
result. Missing optional evidence causes selectors requiring it not to match; it
does not trigger platform guessing. A selected row's malformed data, layout error,
missing required dependency or capacity failure never silently tries another row
or base. That is a failed candidate, not absence of a match.

| Presentation evidence | Authored policy example | Fallback |
|---|---|---|
| Narrow/tall or wide aspect | Exact rational breakpoint selects vertical or horizontal container style. | Base constraints if no interval matches. |
| Handheld | Form factor plus a declared compact interval selects tighter geometry. | Base when classification is Unknown; never infer from OS. |
| Television | Explicit TV row increases control minimums and adjusts margins. | Base; TV does not replace safe-area/font-scale policy. |
| Touch available | Required Touch row increases control minimums; priority expresses its relation to TV/compact rows. | Base when Touch is absent; last input event is irrelevant. |
| Split view | Split row and local safe extent select a compact HUD grid independently for each attachment. | Base; no full-display dimensions or other-player state. |

Priorities are authored product decisions. Runtime has no implicit ordering such
as touch-over-TV or split-over-handheld; an author must encode that ordering.
A row's priority and selector appear in preview/diagnostics so an overlap has an
explainable winner instead of a hidden specificity heuristic.

## Publication, Focus And Retirement

The existing semantic owner retains the document and control/binding state.
Selected effective styles and layout caches are attachment-local derived state.
The complete cache/publication key includes document revision, profile table
revision, selected profile/base identity, evidence revision, attachment generation,
and the existing content/font/locale/style/viewport/policy revisions. Neither a
selected profile ID alone nor raw width/height is a valid cache key.

During ADR-073's owner-thread VariableUpdate boundary, coalesce admitted evidence
changes, validate selection, prepare effective styles and run existing bounded
measure/arrange. Publish selection, layout, interaction geometry and corresponding
focus/navigation projection as one complete generation. Input still uses the last
successfully presented interaction snapshot; renderer extraction consumes the
matching immutable candidate. Draw/backend code never selects a profile, invokes
providers or mutates control/layout state.

Changing profile preserves semantic element/control/action/binding identity and
state. Preserve focus on the same exact live element when it remains eligible;
otherwise reconcile through the existing input/focus authority and declared
navigation policy atomically with the new interaction geometry. Pointer capture
must retain its generation fence or be explicitly cancelled by that authority;
it cannot start targeting another element because its new rectangle occupies the
same pixels. No profile invents focus or reuses a stale raw handle.

A stale document, scope, attachment or evidence generation rejects completion
before publication. Required failure retains the previous last-good generation
for the same still-live attachment; it does not retain input authority after that
attachment has been invalidated. Zero-sized/minimized attachments are suspended
by the presentation owner before selection; Runtime UI does not divide by zero or
create fabricated positive dimensions. Resume revalidates current evidence.

Resize, reassignment, detach, reload, cancellation and shutdown use ADR-073's
existing cutoffs. Revoke attachment admission before draining pending selection/
layout work; release immutable document/projection leases only after consumers and
queued renderer references retire through their existing lifetime contracts.
Profiles own no renderer resources, device handles, independent worker/service,
process-global registry or alternate shutdown sequence.

## Validation, Limits And Errors

The initial contract permits at most 32 non-base rows per document and 4,096 total
element/style overrides across all rows, within existing document element/byte
ceilings. A row references each stable element at most once and only elements in
its document/canvas. Every complete style must pass the existing typed layout
validation. Caller budgets may lower but never raise these compiled ceilings.
Admission checks counts and bounded lengths before allocation; it rejects overflow
rather than truncating profiles or applying a partial patch.

Selection scans at most 32 immutable rows without allocation, I/O, callbacks,
locks or sorting in the frame-hot path. Canonical ordering and style projections
are prepared at admission/control boundaries. Layout work retains existing layout
capacity/measurement budgets; a profile cannot multiply element count or introduce
an unbounded dependency traversal. Evidence changes invalidate only at the owning
update cutoff, not separately during draw or input dispatch.

Expected failures return the existing Horo `Result`/error model with document,
profile and stable element correlation where applicable. The implementing module
must register stable profile-validation/evidence errors in its UI error domain;
malformed selectors, duplicate identity/priority, missing targets, invalid styles,
capacity, stale generation and unsupported version remain distinguishable. Preserve
nested layout/asset/provider failures unchanged when adding profile context.
No log string, exception, empty optional or generic boolean substitutes for those
errors. No new descriptor is registered by constructing inert profile metadata.

## Source, Cook And Compatibility

Profiles are authored document data; selected IDs, evidence, runtime handles,
focus/capture, caches and editor preview dimensions are not serialized. Cook
validates every row and target against the final expanded ordinary element tree,
using [ADR-083](../../adr/083-ui-template-identity-schema-and-expansion.md)'s exact
accepted template revisions. Stable IDs are resolved after expansion; template
local IDs cannot alias runtime slots. Editor save/undo and CLI/MCP use the same
revision-checked document transaction and profile validation, not a separate
responsive configuration file.

The existing source schema 1.1 and cooked payload format 1 have no profile table.
Implementation must explicitly version the durable schema and cooked layout before
adding those fields, update source/cook readers, limits, canonical encoding and
public consumers together, and provide migration coverage. The next assigned
versions belong to that implementation review; this proposal does not silently
reinterpret existing bytes or reserve an uncoordinated version number.

Migrating an old document creates an empty profile table and retains its exact
base constraints and behavior. Old cooked format 1 remains read through its
explicit supported decoder with neutral base selection. A profile-bearing payload
must require the new supported format/capability; an old loader must reject it,
not discard profiles and claim compatibility. An unsupported selector/style kind,
unknown required field or profile contract version rejects the entire candidate
and requests migration/recook. Canonical cook/cache identity includes table,
selectors, priorities, target IDs, complete style values, selection algorithm
version and ordinary dependency digests; source order does not alter bytes.

Reload prepares the complete new table, base, dependencies and projections before
atomic owner commit. Failure preserves the old source/runtime generation and
reports the exact typed cause. Editor preview may override evidence explicitly in
its transient preview context; it cannot overwrite saved authored policy or
packaged runtime evidence. No native/editor state leaks into the artifact.

## Rejected Alternatives And Consequences

- **Constraints alone for every target**: retain them for continuous resize, but
  reject this as the sole policy because modality/form factor and discrete
  container changes are not scalar geometry constraints.
- **Separate documents/trees per device**: reject as responsive fallback because
  replacement changes state/identity and creates divergent binding, focus and
  accessibility definitions. Explicit different semantic routes remain supported.
- **CSS/media-query strings, specificity and cascading inheritance**: reject the
  unbounded language, ambiguous precedence and backend-dependent comparisons.
- **Native-device/platform/backend detection in layout**: reject dependency
  inversion, headless divergence and implicit fallback. The application owns
  typed presentation facts.
- **Last-input-driven switching or automatic hysteresis**: reject hidden temporal
  policy and replay/preview divergence. Input modality and explicit evidence
  changes retain their owning contracts.
- **Runtime callbacks, arbitrary asset substitutions or structural variant
  patches**: reject expanded authority and lifecycle cost; profile metadata is
  finite inert layout data.

The cost is a finite table, explicit author priorities and additional versioned
source/cook admission. Benefits are one semantic tree, explainable deterministic
selection and independent per-viewport geometry. Authors still must validate base
and declared profiles with localized text, accessibility font scale, safe areas,
normal/narrow/long content and available input modalities; profiles do not prove a
layout is usable merely because a row matched.

## Required Dependent Qualification

Implementation must prove exact breakpoint inclusion, rational aspect comparisons,
Unknown classification, missing Touch, multiple admitted modalities, overlapping
priorities, base fallback, malformed/duplicate/oversized tables and arithmetic
failure. The same evidence/table must select identically across headless/editor/
renderer peers. Exercise simultaneous split attachments, stale completions, active
focus/capture, rapid resize/coalescing, suspension/resume, reload rollback,
cancellation and shutdown without partial publication or ownership transfer.

Golden source/cook roundtrips must preserve stable target IDs and canonical
selection ordering; legacy documents keep exact base behavior and unsupported new
payloads fail explicitly. Allocation/work bounds must be measured in the actual
selector/update phases, with scope stated. This documentation ticket adds no
runtime implementation/tests and claims no manual GPU or host lifecycle proof.
