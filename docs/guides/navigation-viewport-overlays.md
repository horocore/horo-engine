# Navigation viewport overlays

Enable **Navigation overlays** in a rendered viewport. The options popup controls
surface bounds, walkable faces, tile edges, modifiers and directed off-mesh links.
Profile, area, surface and evidence filters are local presentation state. Changing
these controls never edits a Scene/definition or changes a bake/runtime provider.

The active Scene is matched by its exact project-relative path and Scene bake
scope. Repeated surface identifiers in other Scenes cannot leak into the view.
Save Scene edits, then reload saved choices in Navigation or start a fresh bake to
refresh source evidence. Unsaved edits and external conflicts display a saved-capture
notice. Existing verified cooked publications are loaded on the host preparation
worker; successful new publications replace the displayed cooked geometry.

## Evidence and ownership

`NavigationViewportSnapshot` is an additive EditorServices public contract. Its
factories copy saved typed Scene bounds/volumes/endpoints and verified
`NavigationCookedTileSet` polygon, tile and link tables. It retains no source
capture, provider storage, topology lease, callbacks or native handles. The only
borrow during rendering is into the immutable copied snapshot. Scene digests,
object/modifier/link identities, profiles, areas, surfaces and tile/polygon indices
remain available as provenance. Existing consumers require no migration.

Evidence filters distinguish saved authored intent, cooked topology, disabled
intent and unvalidated authored links. These categories do not claim a runtime
world is active or that a saved authored link passed collision clearance and
endpoint projection. Live path/agent inspection remains the separate runtime
inspection authority; this surface does not manufacture its generations.
ObjectSubtree surface bounds come from the aggregate actual cooked tile closure;
explicit LocalBounds surfaces also show their exact transformed authored box.
Cylinders retain cylindrical rings/ribs; link arrowheads preserve direction.

The executable composes the same navigation bake owner used by the Navigation
panel. The dedicated `NavigationViewportAccess` geometry query and read-preparation
capability are injected into the viewport attachment, without bake submission or
cancellation authority.
Source refreshes run on the existing capture worker and publish an immutable
mailbox; the host loads it on its owner lane. Worker shutdown drains before
mailbox destruction. Closing the viewport only drops presentation state.

## Bounds and rendering

Capture defaults to 16,384 primitives and 262,144 work units across the complete
project, with hard ceilings of 65,536 and 1,048,576. Work counts include rejected
input inspection and hierarchy resolution, not just emitted geometry. Truncation
is explicit and deterministic. Frame drawing separately inspects at most 16,384
primitives and emits at most 1,024 draw operations, including link arrow strokes.
The draw cap also bounds generated GUI vertices. No provider paging, file access,
job submission, or source capture happens during geometry drawing.

Triangles and segments clip against all six homogeneous planes before perspective
division, supporting both peer depth conventions. Colors resolve from current
accent/success/warning/text/muted theme tokens every frame. Filled walkable faces,
wire boxes/cylinders and directed links provide shape cues alongside colors.

## Qualification

Snapshot tests cover independent lifetime, Scene isolation, transformed authored
geometry, cylinders, directed links, every filter, disabled state, graph rejection,
work/geometry truncation and release of cooked publication storage. The actual
executable-owner bake test checks that generated walkable faces reach the exposed
geometry query. Viewport tests exercise crossing/fully clipped geometry for both
depth conventions. Header consumer coverage includes the additive public types.

Native build/test execution and interactive theme, popup, keyboard, narrow-width
and localized-copy checks must be reported separately from source validation.
