# Player HUD and viewport association

`UiHudAssociation` belongs to `HoroEngine::RuntimeUi`. Consumers include
`Horo/Runtime/Ui/UiHudAssociation.h` through that target's staged public headers.
The facade qualifies an existing Assets-loaded `UiHotReload`; the surrounding
player/scene owner continues to own its publisher, gameplay providers and lifetime.
It implements the association seam of HORO-773 rather than creating another
RuntimeUiService, scene registry, renderer, input stack or widget tree.

## Host composition

1. Load/cook the document through the existing Assets closure and prepare the real
   canvas tree, focus, bindings, layout, clipping and presentation trackers. Activate
   the authored nonmodal `Hud` route through its actual `UiScreenStack`.
2. Issue a fresh `UiHudAssociationId` in the publisher ownership generation. Resolve
   the exact player/session's `UiFocusPlayerId`, input context, logical
   `UiOverlayViewportId` and admitted `UiRenderViewId`. Logical viewport and render
   view are distinct: several views can share a viewport without sharing input.
3. Resolve each gameplay provider for that exact player/session in the host. Copy
   its provider incarnation and explicit `Player` or `GameInstance` scope into the
   identity-sorted bounded descriptor prefix. No nearest-provider discovery occurs.
   `HasProvider` checks that the actual binding store still owns this registration;
   it does not call gameplay code or grant write permission. Scene/entity providers
   are rejected here and remain under their separate scene lifetime contracts.
4. Supply drawable pixel extent, optional DPI and physical safe-area insets.
   `ScreenSpaceCamera` requires a valid host-issued `UiHudCameraId`; overlay mode may
   omit it. Native camera objects and projection resources remain host/Renderer-owned.
   World-space canvases use their existing world/XR contracts and are rejected here.
5. Create the association. A failed admission mutates neither its borrowed publisher
   nor provider, focus, capture or route state.

## VariableUpdate, extraction and input

At the VariableUpdate cutoff, the host publishes gameplay deltas through the real
binding store, then calls `ResolveCanvas`. Use its logical extent as the layout
root content rectangle with origin zero, and its separate font scale for text
measurement. The renderer uses `ContentPixelRect` for the physical safe-area offset;
do not add that offset a second time to logical layout. Publish actual layout and
source-aligned clipping through their existing owners, keeping focus/input geometry
on the same interaction revision.

Reserve the exact renderer submission revision and call `PrepareFrame`. The returned
ticket leases the real whole generation, immutable layout and clipping, and copies
the exact player/viewport/view/camera/metrics evidence. Extract render commands from
those immutable inputs with the existing `UiRenderExtractor`, then submit them through
the existing renderer path. Return its completion receipt to `ApplyPresentation`
with the same ticket. The ticket's association revision, exact publisher generation,
canvas, view, layout interaction and submission revision must all match. Skipped or
failed receipts consume the existing presentation tracker's revision and close HUD
input admission; a delayed completion from a prior attachment cannot reopen it.

Before dispatching the admitted input context, combine `InputEligible` with the
existing overlay/input routing gates. HUDs default to passive. Successful HUD
presentation never overrides modal exclusions, context priority or other players.
The borrowed publisher remains accessible to its owner; directly dispatching it
without these gates bypasses the association contract.

## Replacement and retirement

`enabled` and `visible` are independent. Hiding suppresses extraction/presentation;
disabling suppresses frame preparation. Neither destroys gameplay bindings or
changes semantic player ownership. The host may keep binding updates and layout
planning active while hidden/disabled. Every policy change invalidates old tickets,
cancels actual captures for the exact context, and requires a fresh presentation.

Use `Reassociate` for viewport extent/insets, admitted view, camera incarnation,
provider evidence or policy changes. The association, player, authored canvas and
HUD route stay exact. Validate a new attachment before committing it; invalid
safe-area evidence, provider loss, foreign owners and unadmitted views leave the old
association unchanged. Layout must be rebuilt for new metrics before extraction;
stale root geometry is rejected explicitly rather than silently stretched.

After the external owner commits a whole asset reload or scene reconciliation, call
`Refresh`. The old lease must still belong to that exact publisher, and the new
generation must preserve runtime instance, Assets identity, document, player, route,
view and live provider contracts. A failed/cancelled external reload leaves the
current association usable. An incompatible committed replacement fails closed.
Scene changes do not turn this player-owned HUD into scene ownership; attachment
and semantic lifetime remain separate.

Check `Shutdown(publisher)` before removing the player or context. It cancels exact
captures, closes admission and releases the facade's generation lease without
shutting down the external publisher.

Shutdown during a deferred binding `Abandon` callback inside `CollectRetired`
returns a busy `InstanceStateInvalid` failure without changing association, lease
or captures. The owner retries after collection returns and waits for success
before retiring the player/context. Foreign publisher provenance returns
`HandleOwnerMismatch`; it cannot discard an association's real cancellation work.

The host also closes or reassociates the HUD before retiring its viewport or camera
incarnation. Camera/view tokens are host-issued attachment evidence; the facade
cannot query native renderer or camera lifetime from their value representation.
The destructor only releases owned evidence; it cannot safely call a publisher that
was never retained. Move construction
transfers identity and closes the source; live move assignment is deliberately
unavailable. Previously copied frame tickets retain historical assets/layout/clip
storage through owner retirement and shutdown, but cannot admit new input or render
work. Collect publisher generations through the existing explicit deferred drain.

Successful frame preparation, completion and queries use copied fixed-capacity
evidence and existing immutable leases, with no new allocation, gameplay callback,
I/O, lock, native handle or global service lookup. At most 64 explicitly registered
providers are checked per operation. Load-time composition and deferred collection
remain outside the frame-hot path.

## Cooked canvas policy compatibility

Binary `HOROUIC` format 2 writes every canvas's existing `safeArea`, `uiScale`,
`fontScale` and `pixelSnap` policy after its projection/scaling fields. This fixes
the format 1 cook/load path that discarded authored presentation settings. The
source schema stays 1.1; JSON canvas serialization already owns these fields.
`CookedUiDocument::Decode` still accepts format 1 with its original neutral policy
defaults, and rejects future formats or malformed/truncated format 2 policies.

Recook and republish documents whose old payload discarded nonneutral settings;
those values cannot be recovered from format 1 bytes. Assets continues to verify
the actual new payload digest. Older runtimes reject format 2 and require a
compatible engine. New cooks and existing Runtime UI Assets loaders use the same
owning writer/decoder; HUD composition does not substitute authoring descriptors
after loading. Responsive profile tables remain outside this format change.
