# Runtime UI input debugger integration

`UiInputDebugger` implements HORO-822 / [RUI-013.3] in the existing upper
`HoroEngine::RuntimeUiInput` composition. Link that target and include
`Horo/Runtime/Ui/UiInputDebugger.h`. RuntimeUi remains independent of Input,
editor, renderer and concrete device backends. No GameModule ABI layout or
SDK21 fingerprint changes are required: existing methods/layouts are preserved,
and the new observer lives outside the supported module export structures.

## Host composition and use

After creating the real tree, focus graph, layout/hit stores, presentation tracker,
pointer capture store and `UiNavigationInput`, create the debugger with
`UiInputDebugger::Create(descriptor, focus)`. Set `descriptor.owner` from
`focus.Owner()` and choose finite history/candidate/phase/capture capacities.
Creation validates scope, reserves storage and starts disabled. Enable only when
a debugger view is requested. There is no service discovery or ambient install.

At the host's existing hit-query/dispatch call sites, call the debugger's
`HitTestScreen`/`HitTestWorld`/`Dispatch` wrappers instead of the corresponding
ordinary method. Each wrapper invokes that owner exactly once and returns its
original `Result`, including error domain/code/message. Do not separately invoke
the ordinary method as well. Existing ordinary call sites remain valid and gain
no recording cost. `CaptureState` explicitly borrows current focus, capture,
exact input/view context and navigation owners without pumping input or moving
focus. Canonical `Input::PlayerId` is projected separately from Runtime UI's
player handle; neither can establish the other authority.

`Recent(0)` retains the newest completed record; increasing age walks bounded
history. Draw a target overlay from `Candidates()` using each local rectangle,
`canvasToLocal` inverse affine, clip and hit slop, and color its typed decision.
Use the record's actual screen query or world ray/projection to locate the query.
Draw the propagation timeline from `Phases()` in stored order with each actual
response/failure flag. Use default flags and aggregate dispatch outcome for the
final action. `CaptureState` records focus/modal scope, six resolved direction
edges, actual capture routes and canonical modality/device/player evidence for
an input/focus graph. No draw callbacks or renderer-native handles cross this API.

## Boundedness, failure and lifetime

The descriptor independently caps every row set and rejects reservations over
16 MiB. Capture/dispatch success performs no new C++ allocation or I/O. Full
candidate/phase buffers retain a prefix plus explicit omitted counts; full leased
history increments `Dropped()` and still runs ordinary targeting/dispatch.
Records identify operation failure and failed callback/default attempts; the
original operation error remains the control-flow authority. State capture can
fail on malformed, stale, foreign, stopped or undersized capture evidence and
publishes no partial record. Inspection never reconciles or releases captures.

The debugger and borrowed owners must outlive a synchronous wrapper call and
its handlers. They must not be moved/destroyed inside a callback. Control and
inspection occur on the creating thread, with shutdown outside callbacks. A
snapshot owns immutable evidence, never callback/capture release authority; its
borrowed spans are valid only while a snapshot copy lives. Old snapshots remain
readable after underlying owner retirement, reload, shutdown and destruction.

## Migration and affected callers

This is an additive public contract. Hit-test and dispatch owners add observed
overloads backed by their existing authority paths. `UiFocusGraph::PreviewMove`
uses the same navigation resolver as `Move`; `UiPointerCaptureStore::Inspect`
projects existing leases; `UiNavigationInput` exposes its actual scope/player and
stopped state. Existing methods keep their semantics and require no migration.
The public-header ownership registry assigns the new header solely to
`HoroRuntimeUiInput`; its consumer verifies that target's narrow usage requirements.

On a normal structural/layout reload, publish the new tree/focus/input evidence
through their ordinary owner paths first, then call `Rebind(focus)` with that
actual current graph. Update the actual navigation adapter and capture context
before `CaptureState`; stale owners are rejected. A different canvas, document,
instance, player or presentation layer requires a fresh debugger. Do not infer a
new owner from old history or rewrite historical generations.

The owning regression suite uses actual layout publications, hit stores, trees,
routed handlers, focus graphs, capture tokens and Input routing. It covers
selection/exclusion, world rays, propagation/defaults/failures, capacity overflow,
allocation windows, pinned history, player scope, reload and shutdown. Windows
selects the suite and its explicit public consumer; normal Linux/macOS and Sonar
builds use the same registered targets. Runtime/build results are recorded in the
PR validation receipt after execution; source registration alone is not proof.
