# ADR-074: Runtime UI Layout Units and Measure-Arrange

- **Status**: Proposed
- **Date**: 2026-09-02
- **Supersedes**: None
- **Scope**: Runtime UI logical units, box constraints, precedence, intrinsic sizing, anchors, aspect ratio, flex/grid participation, measure-arrange phases, overflow, rounding, determinism, failure, and compatibility
- **Issue**: [RUI-002.1](https://github.com/HoroCore/horo-engine/issues/706)
- **Jira**: [HORO-706](https://horo-engine.atlassian.net/browse/HORO-706)
- **Parent**: [RUI-002](https://github.com/HoroCore/horo-engine/issues/705)
- **Related**: [ADR-008](008-error-model-exception-boundary-and-registry.md), [ADR-033](033-presentation-and-display-ownership.md), [ADR-073](073-runtime-ui-ownership-scope-and-update-order.md)
- **Normative documents**: [Game UI and HUD](../architecture/runtime/game-ui-and-hud.md), [Rendering Architecture](../architecture/runtime/rendering-architecture.md)

## Context

Runtime UI must express fixed logical sizes, percentages, intrinsic content,
minimum and maximum bounds, aspect ratios, anchors, flex distribution and grid
tracks. Those inputs frequently overlap. Without one precedence contract, two
containers can interpret the same document differently, editor preview can drift
from play mode, and a resize can oscillate between intrinsic and percentage sizes.

Layout is also observable by input and rendering. ADR-073 requires input to use the
last successfully presented interaction snapshot, while the current VariableUpdate
publishes a new immutable layout generation for extraction. Mutating rectangles in
render code, feeding physical pixel snapping back into layout, or measuring from an
editor/native text object would violate that phase and ownership contract.

Browser CSS is not Horo's implicit specification. Its compatibility behavior,
unbounded feature surface and floating-point/browser differences would make a
small retained game UI runtime dependent on undocumented external semantics. Horo
needs a typed, versioned subset whose conflict, overflow and rounding results can
be tested identically in editor, packaged, headless and renderer-peer compositions.

This decision defines that subset. RUI-002.2 owns canvas reference-resolution and
scale-profile selection. RUI-002.7 owns the typed safe-area, DPI, UI/font-scale,
and downstream pixel-snap presentation inputs that produce the logical viewport
consumed here. RUI-002.3 owns the incremental engine and cache implementation.
RUI-002.6 specializes the post-arrange overflow projection for clipping, bounded
scrolling and focus-driven bring-into-view requests. Later tickets may add
container features and RTL, but they must preserve this precedence and snapshot
model or explicitly revise this ADR.

## Decision

### 1. RuntimeUiService owns logical layout

ADR-073 `RuntimeUiService` is the only semantic layout authority. During
VariableUpdate it consumes one immutable document/runtime-tree revision, one
committed binding/content revision and one resolved logical canvas viewport. It
publishes one immutable `UiLayoutSnapshot` generation. Renderer, Input, gameplay,
editor widgets and backend adapters cannot arrange elements or patch its boxes.

The layout coordinate domain is Horo logical density-independent units (`UiDip`).
RUI-002.2 converts the admitted canvas/viewport snapshot into a finite logical
content extent before layout begins. World-space canvases also lay out in `UiDip`;
their canvas transform projects the finished logical snapshot into world/view
space. Native pixels, window coordinates, backend viewports and ImGui coordinates
are not layout inputs.

Layout state is per runtime instance and viewport attachment revision. Shared
authored constraints may be evaluated for multiple viewports, but mutable caches
and results are never shared without the complete document/content/font/locale/
viewport/policy revision key.

### 2. Lengths and ratios are closed typed values

Authored lengths use a closed Horo-owned variant:

```cpp
enum class UiLengthKind : std::uint8_t {
    Auto,
    Dip,
    Percent,
};

struct UiLength {
    UiLengthKind kind;
    UiScalar value;
};
```

`Auto` carries no numeric payload. `Dip` is an authored logical distance.
`Percent` is a finite normalized ratio resolved against the containing content box
on the same axis. Grid fractional tracks use a separate non-negative `UiFr` weight;
flex grow/shrink and aspect ratio use separate typed finite ratios. Unit kinds are
not strings, CSS tokens or renderer enums.

Width, height, min/max, gaps, padding, margins, offsets and track descriptors
declare which kinds and signs they accept. Size, min/max, padding, gap, flex/grid
weights and aspect ratios reject negative values. Margins and positional offsets
may be negative when the descriptor permits it. NaN, infinity, unknown enum values,
percent overflow and values outside the configured logical extent/capacity bounds
fail validation before activation. Floating source `-0` canonicalizes to ordinary
zero during parsing and cannot
survive into the signed fixed-point representation; it is not a distinct semantic
value or a validation failure.

`Intrinsic` is a measurement source, not another serialized length kind. An `Auto`
axis asks the element/container algorithm for its intrinsic contribution. This
prevents documents from combining a supposedly fixed intrinsic token with stale
font, locale, asset or child revisions.

### 3. Every element uses one explicit box model

Each element produces margin, border, padding and content boxes. Authored width/
height and min/max constrain the content box unless a future schema explicitly
introduces a different sizing mode. Padding and border expand outward; margins
participate in the parent algorithm and are never included in the element's
reported content size.

The containing block is the parent's content box after the parent has resolved its
own size. Percent width, horizontal margins/offsets and horizontal padding use the
containing content width. Their vertical equivalents use content height. A percent
on an indefinite axis behaves as `Auto` for intrinsic measurement, records an
indefinite dependency, and is resolved only when arrange supplies a definite axis.
It never reads the viewport or the element's own tentative size as a fallback.

Borders do not accept percentages. A negative final content extent is clamped to
zero after checked arithmetic and emits a bounded diagnostic; numeric overflow or
an extent above the admitted hard limit is a typed layout failure, not saturation
to an arbitrary renderer value.

### 4. Constraint precedence is normative

For each axis, the engine applies this order:

1. Validate the typed descriptor and normalize absent fields to their declared
   defaults. Invalid structure cannot enter measure.
2. Determine whether the parent algorithm supplies a definite assigned size.
   Flex/grid track assignment is definite; ordinary flow availability is only a
   bound.
3. Resolve an authored definite preferred length (`Dip` or resolvable `Percent`).
   If none exists, use the parent-assigned size when present; otherwise measure the
   `Auto` intrinsic contribution. Retain whether each provisional candidate came
   from an independently definite authored/parent value or intrinsic measurement.
4. Apply a positive authored aspect ratio whenever fewer than two axes are
   independently definite. With one definite axis, derive the other axis from it.
   With two intrinsic `Auto` axes, choose the largest ratio-conforming box that does
   not exceed either non-negative intrinsic preferred extent; a single available
   intrinsic axis is the basis, and two zero extents remain zero. The comparison
   and derivation use checked fixed-point math. Aspect ratio does not override two
   independently definite axes, and the following min/max step remains stronger
   when its clamp cannot preserve the ratio.
5. Resolve min and max against the same containing axis and clamp the candidate.
   If resolved minimum exceeds resolved maximum, minimum wins and the effective
   maximum becomes that minimum, with a diagnostic.
6. Apply the parent container's allocation and alignment. Parent allocation may
   offer less or more than preferred size, but cannot violate the effective
   minimum/maximum unless the container reports diagnosed unsatisfied constraints.
7. Produce margin/border/padding/content boxes and overflow extents using checked
   arithmetic. Visibility, clipping and physical snapping do not alter them.

This is not last-property-wins behavior. Serialization order, inspector edit order
and hash-map iteration cannot change precedence.

### 5. Anchors define a containing segment, not a second size system

Absolute/anchor layout first resolves logical start/end anchor lines in the
parent's content box and then applies offsets/margins. It follows these rules per
axis:

- both anchors plus `Auto` size stretch to the non-negative anchor segment;
- one anchor plus `Auto` size uses intrinsic preferred size and positions from that
  anchor;
- a definite size remains definite; one or two anchors establish the alignment
  segment, and declared start/center/end/stretch alignment positions the clamped
  box within it;
- stretch alignment with a definite size behaves as center only if the schema
  explicitly requests `preserveSize`; otherwise validation rejects the ambiguous
  combination;
- no anchors use the parent's normal placement/alignment rule.

Absolute/anchored children are removed from flex/grid flow and do not contribute
to intrinsic track or flex main size unless a later descriptor explicitly declares
a bounded overlay contribution. They still contribute to the parent's overflow
extent after arrangement.

### 6. Measure and arrange are separate, bounded phases

Layout evaluates one acyclic retained element tree in two logical phases:

```text
Measure(available range)
  -> bottom-up intrinsic min/preferred/max contributions and dependencies

Arrange(final content rectangle)
  -> top-down final boxes, clip/scroll extents and interaction geometry
```

Measure may query only immutable, revisioned intrinsic providers for text, image,
style and child content. Providers return Horo logical metrics and typed status;
they do not expose native font objects, renderer resources or editor widgets.
Measurement cannot emit gameplay commands, mutate bindings, create assets or
publish partial rectangles.

Arrange receives a definite parent content rectangle, resolves remaining
percentages and distributes container space. If late resolution changes an
intrinsic dependency, the engine may perform one bounded remeasure of the affected
subtree. A second change is a diagnosed non-convergent layout failure. Unbounded
fixed-point iteration is forbidden.

The published layout snapshot contains the complete tree's boxes, baselines,
overflow and hit-test geometry plus every source revision. A correlated
`UiLayoutClipSnapshot` derives the immutable clip chain, scroll extents and
bring-into-view projection from that exact layout generation; render and input
consume it without recomputing layout. A failed candidate publishes nothing and
ADR-073 retains the previous active/last-good generation according to
required/optional policy.

### 7. Flow, flex and grid share the same size precedence

Normal stack/flow containers measure children in stable authored child order,
apply padding/gaps once, and arrange along their declared main/cross axes. Margins
do not collapse. Negative free space is overflow unless the container explicitly
supports shrink behavior.

Flex uses the clamped preferred/intrinsic main size as each item's base. It then
distributes positive free space by non-negative grow weights or negative free
space by shrink weight multiplied by the flex base. Items that reach min/max are
frozen and the remaining space is redistributed in stable authored order. Cross-
axis sizing and alignment still follow Sections 4 and 5.

Grid resolves definite `Dip`/percentage tracks first, intrinsic/`Auto` tracks
second and non-negative `UiFr` tracks from the remaining definite space. Spanning
items contribute deficits across eligible tracks in stable track order. Track
min/max constraints freeze reached tracks before remainder redistribution.
Percentage and fraction tracks on an indefinite axis contribute as `Auto` during
intrinsic measure and resolve during arrange.

RUI-002.3 through RUI-002.5 may optimize and extend these algorithms, but cannot
give the same descriptor a different precedence or make container choice alter
the meaning of `Dip`, `Percent`, min/max or aspect ratio.

### 8. Cycles, conflicts and missing intrinsic data are explicit

The retained parent/child tree must be acyclic. Binding or style references cannot
introduce a second layout parent. Self-sizing dependencies such as parent auto size
depending on a child's percentage of that same indefinite parent resolve the
percentage as `Auto` during intrinsic measure, then use the bounded arrange-time
remeasure rule. A remaining dependency cycle fails with element IDs and property
paths.

Required intrinsic content that is missing, stale beyond policy, malformed or
unsupported fails the candidate. Optional content uses a declared finite fallback
metric and diagnostic; it does not ask Renderer for current glyph/image bounds.
Conflicting anchors, invalid grid spans, duplicate placements, non-positive aspect
ratios, impossible required bounds and capacity/depth/pass-limit exhaustion return
typed errors.

### 9. Overflow is geometry; clipping and scrolling are policies

Arrangement always computes a finite overflow extent from arranged descendants,
including absolute children, before clipping. `Visible`, `Clip` and `Scroll` are
typed policies:

- `Visible` preserves overflow for paint/hit-test policy but does not enlarge the
  parent's assigned box;
- `Clip` intersects descendants with the element's declared clip box;
- `Scroll` owns a generation-checked logical offset and a content extent at least
  as large as its viewport; the offset is clamped after each successful layout.

Clip chains and scroll transforms are immutable snapshot data. Input uses the last
successfully presented snapshot's clipped geometry. Renderer consumes the same
clip/transform data and cannot enlarge hit targets or recompute scroll extents.
RUI-002.6 may add detailed scrolling and bring-into-view behavior within this
boundary.

### 10. Logical rounding is deterministic and pixel snapping is downstream

`UiScalar` has a versioned signed fixed-point logical representation with 1/64 DIP
resolution. All authored conversion, percentage multiplication, aspect division,
sum and distribution operations use checked deterministic arithmetic. Division
rounds to nearest with ties to even. Remainder distribution follows stable authored
item/track order and never pointer address, hash iteration, thread completion or
backend order.

Layout clamps negative size only at the explicit box-model boundary and otherwise
preserves signed positions/margins. Intermediate overflow is an error. The same
validated document, provider revisions, logical viewport and policy must produce
bit-identical logical boxes independent of frame rate, CPU thread scheduling,
renderer backend and editor versus packaged host.

RUI-002.7 resolves the physical viewport, safe content rectangle, effective UI
scale, and output/DPI evidence before layout. Its font scale is an explicit input
to intrinsic text measurement; it does not change the meaning of non-text logical
units. It may snap projected edges to physical pixels using the output/DPI policy.
Snapping is derived render data: it cannot feed back into measure, alter scroll
extent, become serialized state or replace logical hit-test geometry. If a product
policy requires hit testing snapped geometry, Renderer must return the presented
interaction geometry as a new immutable revision through ADR-073, never mutate the
logical snapshot in place.

### 11. Activation, errors and compatibility are transactional

Layout descriptors are versioned cooked document data. Unknown required unit,
container, track, alignment, overflow or rounding semantics reject the artifact.
Optional forward-compatible fields may be ignored only when their schema marks
that behavior and the cooked compatibility contract records it. No adapter guesses
CSS, platform or legacy-editor defaults.

Document/style/content/font/locale/canvas-policy changes prepare a candidate
layout generation. Publication occurs only at ADR-073's VariableUpdate snapshot
boundary after complete success. Older render and interaction generations retain
their immutable boxes until their frame/presentation leases retire. Cancellation,
scope unload, viewport replacement and shutdown prevent late candidates from
publishing into a new owner or attachment generation.

Errors use ADR-008 typed results with element/document IDs, property path, source
revision, phase, finite operands and a stable reason code. Diagnostics are bounded
and redact user text/content. Required layout failure deactivates or retains the
previous generation according to the instance activation policy; optional element
fallback must be declared in the cooked descriptor.

### 12. Verification is part of the contract

Required coverage includes:

- every legal/illegal unit and sign combination, source negative-zero
  canonicalization, finite limits and checked numeric overflow;
- definite/auto/percentage/intrinsic sizing with min/max and the `min > max` rule;
- zero/one/two anchors, definite versus auto size, alignment and negative offsets;
- aspect ratio with zero, one or two independently definite axes, two-`Auto`
  intrinsic containment, a single intrinsic basis and min/max clamping;
- flow/flex/grid free-space distribution, freeze/redistribution, spans and stable
  remainder assignment;
- indefinite percentage dependencies, one bounded remeasure, cycles and diagnosed
  non-convergence;
- margin/border/padding boxes, zero extents, overflow, clip chains and scroll clamp;
- locale/font/content/style revision changes, missing required/optional intrinsic
  data and last-good retention;
- equivalent editor, packaged, ModelOnly/headless and renderer-peer logical
  snapshots at multiple frame rates and thread schedules;
- resize/DPI/safe-area/output/backend changes with old frame leases and no physical
  snap feedback;
- malformed/version-skewed cooked descriptors, cancellation, scope/viewport unload,
  partial activation and repeated shutdown.

Golden/property tests compare logical fixed-point boxes and stable diagnostics.
Native screenshot comparison may qualify later presentation policies, but cannot
replace logical contract tests.

## Consequences

Dependent runtime and editor work receives one portable meaning for layout values
and conflicts. Input and rendering observe the same immutable geometry, resize and
locale changes publish transactionally, and bit-identical logical results can be
tested without a window or GPU.

The cost is a Horo-specific typed schema, fixed-point checked arithmetic, explicit
indefinite dependencies, bounded remeasure, stable distribution order and strict
artifact versioning. Container implementations cannot borrow browser behavior
without translating it into this contract.

## Rejected Alternatives

### Treat CSS as the implicit layout specification

Rejected because Horo would inherit a much larger compatibility surface and
browser-specific edge behavior without a browser engine or versioned artifact
contract.

### Use unrestricted floats and epsilon comparisons

Rejected because cross-platform rounding, hash order and vectorization could move
edges, flex/grid remainders and hit targets. Layout uses checked fixed-point logical
arithmetic.

### Let the most recently authored property win

Rejected because inspector edit order and serialization order are not semantics.
The normative precedence is independent of authoring history.

### Combine measurement and arrangement in one recursive pass

Rejected because intrinsic content, parent assignment and percentage resolution
have different information boundaries. A single pass either guesses or mutates
already-consumed child results.

### Make every canvas or container choose its own conflict rules

Rejected because reusable elements would change meaning when reparented. All
containers share the same unit, aspect and min/max precedence.

### Perform layout in physical pixels

Rejected because DPI/output/backend replacement would become semantic state and
world-space/headless UI would lack one portable domain. Physical snapping derives
from the immutable logical snapshot.

### Recompute layout in Renderer or Input

Rejected because that creates multiple authorities and can target geometry that
was not presented. Both consume generation-correlated immutable snapshots.
