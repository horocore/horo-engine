# Runtime UI input, focus and event routing qualification

This matrix exercises the existing Runtime UI owners with a deterministic layout,
committed Input snapshots, and virtual devices. The widget and screen adapters
consume the same focus graph, event dispatcher, capture store, and action queue
used during runtime. The SDL row uses an actual native adapter with virtual
hardware.

## Required evidence

| Area | Automated qualification |
| --- | --- |
| Geometry, nesting and clipping | `HoroRuntimeUiHitTestingTests` and `HoroRuntimeUiLayoutClippingTests` cover paint-order overlap, nested clips, safe areas, singular transforms, bounded rays, and exact presentation revision. |
| Propagation, mutation and capture | `HoroRuntimeUiEventDispatchTests`, `HoroRuntimeUiPointerCaptureTests` and `HoroRuntimeUiAnimationIntegrationTests` cover frozen capture/target/bubble order, default prevention, deferred mutation, cancellation, retained control state, and capture release before new geometry becomes eligible. |
| Modal, focus and multiplayer | `HoroRuntimeUiFocusGraphTests`, `HoroRuntimeUiOverlayLifecycleTests` and `HoroRuntimeUiHudAssociationTests` cover explicit and spatial focus, nested modal trapping, player-local restoration, and per-audience cleanup. |
| Text and device lifecycle | `HoroRuntimeUiControlsTests`, `HoroRuntimeUiInputTests` and `HoroInputTests` cover text and composition in the exact focus scope, mixed devices, disconnect neutralization, and player assignment. The two-player virtual case checks concurrent navigation and submission, stable action queues across replacement, and neutral admission for the replacement controller. |
| Reload, takeover and actionable diagnostics | `HoroRuntimeUiHotReloadTests`, `HoroRuntimeUiSceneReconciliationTests` and `HoroRuntimeUiInputDebuggerTests` cover immutable retained evidence, stale owner rejection, capture/default suppression, bounded observation, and a world ray routed through a clipped modal target. |
| Host-adapter evidence | `HoroInputSdlTests` covers SDL virtual gamepad routing, focus loss, unknown mapping fallback, and device replacement through native event dispatch. |

A world hit is qualified against the retained logical layout and its actual
presentation receipt. The world case passes the selected target and logical
point to the existing dispatcher, verifies modal-bounded propagation and
default prevention, and proves that a replacement presentation rejects the old
hit snapshot. XR motion controllers and native world-canvas rendering have their
own device and rendering qualification.

Failures must preserve the last published generation, or cancel the affected
transient owner exactly once. For diagnosis, inspect the exact error domain/code
and retained observation. An unpresented hit snapshot has
`runtime_ui.hit_test.not_presented`. A failed callback retains its bounded phase
observation and cannot be reported as a successful default. Device replacement
must preserve existing queued actions and reject held input until a neutral
sample commits.

## Running the matrix

Configure with `BUILD_TESTING=ON` and `HORO_BUILD_INPUT_SDL3=ON`. Run the matrix in
the normal Linux and Windows builds. For a headless configuration, enable the
SDL adapter independently of the optional editor GUI.

```sh
cmake -S . -B build/ui-input -DBUILD_TESTING=ON -DHORO_BUILD_INPUT_SDL3=ON
cmake --build build/ui-input --config Debug --parallel --target \
  HoroInputTests HoroInputSdlTests HoroRuntimeUiInputTests HoroRuntimeUiInputDebuggerTests \
  HoroRuntimeUiHitTestingTests HoroRuntimeUiEventDispatchTests HoroRuntimeUiPointerCaptureTests \
  HoroRuntimeUiFocusGraphTests HoroRuntimeUiControlsTests HoroRuntimeUiAnimationIntegrationTests \
  HoroRuntimeUiOverlayLifecycleTests HoroRuntimeUiHudAssociationTests HoroRuntimeUiLayoutClippingTests \
  HoroRuntimeUiHotReloadTests HoroRuntimeUiSceneReconciliationTests
ctest --test-dir build/ui-input -C Debug --no-tests=error --output-on-failure \
  -R '^(HoroInput(Sdl)?Tests|HoroRuntimeUi(Input|InputDebugger|HitTesting|EventDispatch|PointerCapture|FocusGraph|Controls|AnimationIntegration|OverlayLifecycle|HudAssociation|LayoutClipping|HotReload|SceneReconciliation)Tests)(::|$)'
```

Record configuration, platform, and CTest totals with the delivery. If the SDL
target cannot be configured for a host, record the missing adapter evidence
separately from the deterministic and renderer-neutral cases.
