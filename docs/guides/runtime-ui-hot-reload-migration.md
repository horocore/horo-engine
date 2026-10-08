# Runtime UI hot reload migration

`UiHotReload.h` belongs only to `HoroEngine::RuntimeUi`. Link that target and use
its staged public headers. The coordinator is a backend-neutral owner of a whole
cooked asset closure and actual canvas tree, focus, control, route, binding,
capture, layout and presentation owners. It does not select a renderer, discover
services or install callbacks. Creating descriptors remains inert.

## Preparation and publication

Use the existing `UiRuntimeAssetLoadService` to load a complete immutable Assets
closure off the frame path. Construct all replacement typed canvas owners
privately, then transfer the result and owners into `UiReloadGeneration::Create`.
The real cooked document supplies canvas roots, element IDs/parents/types, route
metadata, required dependency leases and source revision. The coordinator checks
that composition rather than trusting a separate audience/provenance DTO.

Keep the same actual `RuntimeUiInstanceId`, `UiCanvasInstanceId` and focus
player/presentation-layer scope. Supply strictly newer authored, tree and
interaction revisions. Scope migration is a separate host authority operation;
reload cannot fabricate or rebind a game/player/layer identity. Asset registry
revision cannot move backwards. Every canvas in the cooked document must be
present, and every actual retained element must match its cooked hierarchy.

`Prepare` performs load-time reconciliation against a pinned old generation and
copies bounded source evidence solely for candidate admission. It is not another
property/state owner. An actual draft, Cancel baseline, focus/modal, route,
binding publication or scroll/layout change between preparation and publication
makes `Commit` fail with a stale source. Cancellation and failure leave the old
active generation intact. A rejected candidate's admitted element namespace is
burned; it cannot be recycled by a restarted allocator.

`Commit` runs on the serialized Runtime UI owner at an ADR-073 structural safe
point. It closes old admission and publishes all replacement canvases together.
Successful publication uses reserved retention slots and does not load assets,
allocate, invoke a binding authority or destroy the last generation pin. When
retention capacity is full, publication fails before changing either generation.
Use `CollectRetired` outside frame work to abandon deferred binding reservations
and reclaim drained owners. Authority/module leases remain retained until this
operation or owner destruction has abandoned each reservation exactly once.

Keep a `UiReloadLease` throughout render/input extraction. `IsCurrent` is the
publication fence; the lease also retains the entire dependency closure. A lease
of a retired generation permits retained inspection, but its mutable owners have
closed input/action admission. Shutdown denies new leases/candidates, retires the
active owner into an extra reserved slot and waits for candidates, generation
leases, captures and immutable layout/clip leases to drain before `CanReclaim`.
Destroy the coordinator only outside frame work. Do not retain unpinned pointers
returned by `Current` or `Canvas` across an owner operation.

## Compatible logical state and transient identity

Stable element IDs preserve control values only when authored types and actual
control kinds, semantic owner, action IDs/arguments and new value constraints
agree. Text drafts retain the original Cancel baseline and editing state when
focus remains compatible. A narrower text/range contract uses its authored
initial value instead of truncating or misapplying a draft. Press, repeat,
pending defaults, async projections, native IME and capture state do not migrate.
Canonical surviving route definitions replay into new route incarnations;
compatible scroll offsets are projected/clamped by the actual new clipping
owner. Focus remains inside the same authoritative player/layer audience.

ADR-073 forbids reusing identities observable by retired commands or leases.
Modal IDs now use the actual modal-root element reservation as their slot and a
graph-wide EVER-issued incarnation as their generation; popping/reloading never
resets that counter. Route instance IDs use the never-reused stack namespace and
a stack-wide EVER-issued incarnation. Treat these IDs as opaque. Do not derive
modal depth or activation order from their slot. Raw old handles fail directly
against replacement owners, in addition to the generation lease fence.

The aggregate requires each stack slot to equal its real canvas-root element
slot. Retain one `UiElementSlotAllocator` per ownership generation, including
unused/tombstoned reserved ranges. If a host deliberately reinitializes that
allocator, pass its entire EVER-reserved high-water mark to `Create`; never seed
from currently live elements. Exhaustion returns `GenerationExhausted` without
wrapping or modifying the active generation. Optional initial modal/route
high-water marks have the same history contract for a reinitialized namespace.

New presentation trackers start empty. Old receipts cannot grant eligibility to
a replacement interaction. `ApplyPresentation` admits only exact current layout
interaction evidence, and `InputEligible` requires successful presentation for
the exact admitted view/canvas.

## Composition boundary and verification

The shipping aggregate `RuntimeUiService` and host scope registries described by
ADR-073 are not implemented on this main revision. This change provides their
typed RuntimeUi-owned reload responsibility and tests it with the real Assets
loader and actual owner implementations. It does not claim editor/game host
wiring or native renderer/IME behavior. Host composition must supply its existing
authoritative scope and presentation bindings; no new unchecked identity adapter
or global allocator is introduced.

`HoroRuntimeUiHotReloadTests` covers actual loaded replacement generations,
compatible drafts/Cancel baselines, focus/routes/scroll, narrowed constraints,
source changes, direct stale handles, deep/repeated ABA, overflow, cancellation,
retention, held snapshots and shutdown. The dedicated public-header consumer
uses only `HoroEngine::RuntimeUi`; existing owning suites and consumers continue
to qualify changed handle and retirement contracts.

## Publisher logical constness

The publisher facade now retains its existing shared state through a private, non-template const-propagating holder. A const facade borrows const publisher storage; only a mutable facade can obtain the real mutable publisher pin. Prepared candidates still retain that same single pin, and generation leases keep their existing lifetimes. This representation adds no allocation or parallel authority.

Prepare, Commit, ApplyPresentation, CollectRetired and Shutdown remain nonconst owner commands. They reconcile/publish state, advance interaction eligibility, drain deferred producer reservations or revoke admission; readonly facade access does not grant those operations. Acquire, InputEligible, IsCurrent and CanReclaim retain their const read contracts. Existing caller signatures need no migration. Private compile traits and the standalone public consumer qualify both sides of this boundary.
