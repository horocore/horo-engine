# Presentation Qualification

The renderer qualification covers the host-owned presentation contract from
[ADR-033](../adr/033-presentation-and-display-ownership.md).
Record evidence separately for the platform display source, renderer backend,
and native surface. Validate the actual selected backend for every native run.
The same application may have different availability on a different window
system, driver, adapter, or monitor.

## Validation Matrix

| Behavior | Automated evidence | Pass threshold |
| --- | --- | --- |
| Present mode | `HoroPresentModeTests`, Vulkan WSI contract tests, SDL OpenGL exact-interval smoke | A supported explicit request returns the exact mode. An unsupported request returns its typed failure. Auto selects only from its declared order and reports the selection. A failed native interval query or mismatched readback cannot publish Exact. |
| Display and output negotiation | `HoroRenderDisplayTests`, including presentation output tests, and `HoroRenderFrontendTests` | Invalid, stale, contradictory, and detached facts leave the previous publication intact. Explicit HDR requires the exact encoding and qualified display evidence. Unsupported color or transfer requests preserve the previous output. |
| Pacing and latency budget | `HoroFramePacingTests` and backend admission tests | The host cap accepts 0 through 1000 Hz and waits in slices no greater than 2 ms. Late work cannot cause a catch-up burst. Resize, focus change, and suspension discard the timing baseline. Native time has exact provenance. CPU return alone supplies no display timestamp or refresh ordinal. Frame-count budget rejection cannot silently reduce the requested bound. |
| Resize, monitor, DPI, and fullscreen | `HoroRenderSurfaceLifecycleTests`, `HoroEditorPresentationSurfaceTests`, native SDL Vulkan and OpenGL smoke, native Metal resize smoke | Each accepted transition advances once after realization. A stale, superseded, or active-frame operation cannot change the output. A same-size display change still advances the exact display revision. Native resize resumes presentation only at the observed pixel extent and denies the retired one. |
| Suspension, failure, and shutdown | Shared surface and pacing tests, Vulkan WSI failure fixtures, SDL OpenGL editor recovery, native Vulkan/Metal teardown | An absent drawable admits no frame. A recoverable failure carries a typed cause and either preserves the old active output or explicitly withdraws it. All acquisition, submission, and presentation debt retires before native parents. The Vulkan smoke requires resumed acquisition and terminal drain within separate 5-second test budgets, with zero retained renderer allocations at terminal drain. |
| Physical HDR and VRR | Metal linear EDR lane and documented physical-display procedure below | A physical claim needs the active encoding and native output facts from that display. Current linear EDR headroom must exceed 1.0 where EDR is claimed. No current desktop presentation contract publishes active VRR, so no current run qualifies physical VRR. FIFO, Immediate, host frame caps, and refresh estimates are distinct evidence. |
| Multiple native surfaces | [Desktop multi-surface contract](../architecture/runtime/rendering-architecture.md#multiple-presentation-surfaces) | Qualify only the current single-surface path. Multi-surface qualification requires a per-surface frame identity, isolated resize/loss/close, bounded resources, and independent progress for every unaffected output. The backend-global frame token cannot establish these properties. |

## Reproducing The Automated Lanes

Run the shared contract suite from a normal test-enabled build on each desktop
platform. Test discovery must fail the run if it selects zero cases.

```sh
cmake -S . -B build/presentation-contract -DBUILD_TESTING=ON
cmake --build build/presentation-contract --parallel --target HoroRenderDisplayTests HoroPresentModeTests HoroFramePacingTests HoroRenderSurfaceLifecycleTests HoroEditorPresentationSurfaceTests HoroRenderFrontendTests HoroRenderBackendRegistryTests
ctest --test-dir build/presentation-contract --no-tests=error --output-on-failure -R '^(HoroRenderDisplayTests|HoroPresentModeTests|HoroFramePacingTests|HoroRenderSurfaceLifecycleTests|HoroEditorPresentationSurfaceTests|HoroRenderFrontendTests|HoroRenderBackendRegistryTests)(::|$)' --output-junit presentation-contract.xml
```

| Native lane | Reproduction and evidence boundary |
| --- | --- |
| SDL OpenGL on Windows or Linux | Configure the native GUI build with `BUILD_TESTING=ON`, `HORO_BUILD_RENDER_OPENGL=ON` and `HORO_ENABLE_GPU_SMOKE_TESTS=ON`. Build `HoroOpenGLSurfaceLifecycleSmoke` and `HoroEditor`. On an admitted display/device, run `ctest --test-dir build/presentation-gl --no-tests=error --output-on-failure -R '^(HoroOpenGLSurfaceLifecycleSmoke::|HoroEditorOpenGLSurfaceRecovery$)'`. Record renderer, driver, and SDL window system. Xvfb or software coverage can establish native API lifecycle but cannot establish physical display behavior. |
| SDL Vulkan on Linux or Windows | Configure the native GUI build with `BUILD_TESTING=ON`, `HORO_BUILD_RENDER_VULKAN=ON` and `HORO_ENABLE_GPU_SMOKE_TESTS=ON`. Build `HoroEditorVulkanPresentationSmoke` and `HoroRenderVulkanTests`. Run the latter with `[renderer][vulkan][wsi]` for scripted debt/failure checks. Run the smoke on a Vulkan display with validation layers and the [locked viewport cook](vulkan-editor-presentation-integration.md). Record the exact ICD/device and validation log. The Linux shader job already uses Xvfb and retains the driver facts, cook catalog, CTest XML, and native log. Reject validation errors, warnings, and VUID diagnostics. |
| Metal on macOS | The `ci-macos-debug` producer builds the native GUI with `HORO_ENABLE_GPU_SMOKE_TESTS=ON`. The `macos-metal` job consumes the verified same-commit artifact and runs the packaged native tests. For local hardware, run `ctest --test-dir build/ci-metal --no-tests=error --output-on-failure -R '^HoroEditorViewportMetalSmoke::'` after the producer build. Retain the native result and display facts. A linear EDR skip leaves that row unqualified even if SDR and drawable lifecycle pass. |

The standalone Vulkan resize case uses a single real SDL window, independent of
viewport shader cooking. It can run on its own with the configured Vulkan
validation layer and a display or Xvfb.

```sh
ctest --test-dir build/presentation-vulkan --no-tests=error --output-on-failure -R '^HoroEditorVulkanPresentationSmoke::Native Vulkan keeps frame ownership'
```

## Physical Display Lane

Run each physical row with the exact supported backend and display combination.
Record the source commit, build mode, OS/window system, GPU and driver,
monitor mode, scale, requested policy and resolved output. For each operation
record the surface generation, logical and pixel extents, display revision, and
success or registered error domain and code. Keep the native error message
and original cause available in the test report.

- Move the window across two different DPI or output modes at constant logical
  size. Change the active monitor and toggle fullscreen. Require the next
  presented frame to use the latest pixel extent and display/output pair.
  The old extent or generation must not admit work after the new publication.
- Minimize and restore, switch away and back, and disconnect the active display
  while a second display is available. Require no frame admission while the
  surface is suspended. Resume with a fresh, current publication on a surviving
  output, or record the typed loss. No timeout or implied fallback counts as a
  successful transition.
- Exercise one supported and one unsupported mode or output request. Record
  the requested and observed active modes through the backend, and the native
  display facts at the transition. For HDR, also record exact primaries and
  transfer, the active pixel format, and the display's applicable headroom.
  If a physical confirmation is missing, report only the successfully observed
  API or layer realization.

Use **passed**, **failed**, or **unqualified** per row. A row is unqualified
when the required hardware, supported path, or provenance is missing. Report
software Vulkan/OpenGL, scripted native dispatch, and physical GPU/display
runs separately. Include an actionable next run when a row is unqualified.
