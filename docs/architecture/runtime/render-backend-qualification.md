# Cross-Backend RHI Qualification

## Purpose

This document is the qualification owner for HORO-305 / RND-003.10. It turns
the backend-neutral lifecycle and parity contracts into a reproducible matrix.
It distinguishes an implementation that is tested from an interactive backend
that is actually qualified on a supported host. Missing hardware, an omitted
lane, or an intentionally unsupported operation is never counted as a pass.

The shared behavioral obligations remain in
[Render Backend Parity Contract](render-backend-parity-contract.md). Device-loss
ownership remains in [ADR-179](../../adr/179-device-loss-and-renderer-restart-lifecycle.md).
This document records the evidence and thresholds for the current implementation
stage; it does not promote a planned backend or invent a native qualification
result.

## Result vocabulary

Every matrix row has one of these outcomes:

| Result | Meaning | Counts as a qualification pass? |
|---|---|---|
| `PASS` | Every required case ran and met its threshold. | Yes |
| `EXPECTED_UNSUPPORTED` | The backend intentionally lacks an optional operation and returned the exact registered typed error. | Only for that optional operation; it does not qualify the missing feature. |
| `NOT_APPLICABLE` | The platform or product policy does not include the row. | No; it is excluded by an explicit scope decision. |
| `UNQUALIFIED` | The implementation or required host evidence is not available yet. | No |
| `FAIL` | A required case failed, crashed, timed out, leaked, or was unexpectedly skipped. | No |

An absent display, missing device, unavailable validation layer, or disabled
GPU-smoke registration produces `UNQUALIFIED` for the affected native row. It
must not be converted to `PASS` by a retry or by silently selecting another
backend.

## Current qualification matrix

The deterministic rows use fake/native-port seams and run without a display.
They prove the Horo contract, rollback, capability admission, diagnostics, and
shutdown ordering. They do not prove driver behavior. Native rows require the
listed host and a real compatible graphics runtime.

| Host/configuration | Backend | Deterministic evidence | Native evidence | Current status |
|---|---|---|---|---|
| Linux, macOS, Windows headless | Null | `HoroRenderBackendRegistryTests`, including the shared contract and capability tests | Not applicable; Null makes no native claim | Required `PASS` |
| Linux desktop, X11 or Wayland | OpenGL 4.1 Core | `HoroRenderOpenGLTests` shared contract, context admission, malformed plan, failure rollback, and resource tests | `HoroEditorViewportOpenGLSmoke` on a display-capable lane; X11 and Wayland are separate evidence rows | Required `PASS` when the host tuple is shipped; otherwise `UNQUALIFIED` |
| macOS 14+, native arm64 Apple7 or native x86_64 Mac2 | Metal | `HoroRenderMetalTests` (or `HoroMetalDeviceCapabilityTests` when the Metal target is omitted) shared contract, device admission, malformed plan, failure rollback, and resource tests | `HoroEditorViewportMetalSmoke` plus `MetalUploadShutdownSmoke` | Required `PASS` for each shipped architecture variant |
| Windows desktop | OpenGL 4.1 Core | `HoroRenderOpenGLTests` | A display-capable OpenGL lane; software Mesa is evidence for the software row only | Required `PASS` for the shipped Windows OpenGL tuple |
| Linux or Windows with `HORO_BUILD_RENDER_VULKAN=ON` | Vulkan | `HoroRenderVulkanTests`: loader/device/resource admission and an explicit expected-unsupported interactive-operation case | No Vulkan frame/presentation smoke is eligible while `BeginFrame`, `Execute`, `Present`, and `Resize` return `render.vulkan.unsupported_operation` | `UNQUALIFIED` for interactive parity; the typed unsupported result is required |
| Windows 11 x86_64 | D3D12 | No target is present in this implementation stage | Deferred to the D3D12 component and its hardware lane | `NOT_APPLICABLE` |

The Vulkan row is deliberately visible so a build that enables the component
cannot mistake initialization/resource coverage for interactive qualification.
The current renderer architecture still describes Vulkan and D3D12 as planned
product components; adding a test target does not change that status.

## Required case groups

The shared contract test is parameterized only by backend-neutral expectations:
backend ID, presentation requirement, error domain, synthetic-capability state,
and the exact optional-unsupported operation code. Native API types stay inside
the backend-specific test seam.

Each interactive backend's deterministic suite covers:

1. module identity and presentation metadata before native creation;
2. successful initialization, capability publication, frame begin, graphics
   execution, presentation, resize, and repeated shutdown;
3. unsupported compute/pass admission with the exact backend operation error;
4. malformed configuration, zero extents, duplicate pass IDs, foreign frame
   tokens, and resize during an active frame;
5. abort and frame reuse after a rejected or failed operation;
6. injected native-port failures, initialization rollback, presentation failure,
   and preservation of the typed cause/error domain; and
7. post-shutdown rejection plus idempotent teardown.

Null runs the same lifecycle and malformed-input cases without a GPU and reports
its synthetic capability snapshot. Vulkan runs the applicable loader, adapter,
feature, resource, unsupported-operation, and shutdown cases but is not included
in the successful interactive lifecycle group until its frame submission stage
exists.

### Loss and recovery boundary

The current `IRenderBackend` API has no generation-bound renderer-loss fact or
host recovery controller. The available automated loss-adjacent evidence is
therefore intentionally limited to the safe boundary that exists today:

- an injected Begin/Execute/Present failure retains a typed backend/operation
  cause;
- a caller can abort the active frame exactly once and reuse the backend; and
- shutdown remains safe after a partial or failed initialization.

These cases are required `PASS` criteria for the current implementation. They
are not a device-loss qualification claim. Native `DeviceLost`, `DeviceHung`,
`DeviceRemoved`, old-generation invalidation, and in-process restart remain
`UNQUALIFIED` until the ADR-179 lifecycle is implemented. A future backend may
not report those rows as passed using a fake error or a preserved old handle.

## Diagnostics contract

Qualification compares typed fields, never native message text. Every expected
failure in the shared harness must contain:

- a non-empty stable error code;
- a non-empty owning domain, with the backend-specific domain for OpenGL,
  Metal, and Vulkan and the shared Horo render domain for Null;
- a non-empty operation-specific message; and
- the registered descriptor's remediation information at the production
  boundary.

Representative unsupported diagnostics are exact:

| Backend | Capability/operation | Required code |
|---|---|---|
| Null | Compute pass | `render.backend.unsupported_pass_kind` |
| OpenGL | Compute pass | `render.opengl.unsupported_pass_kind` |
| Metal | Compute pass | `render.metal.unsupported_pass_kind` |
| Vulkan | Interactive frame/presentation stage | `render.vulkan.unsupported_operation` |

The backend ID and operation are therefore available through stable typed fields
even when the human-facing message changes. Native handles, API enums, and
unbounded driver strings are not part of the qualification result.

## Objective pass/fail thresholds

### Deterministic contract rows

A deterministic backend row is `PASS` only when all of the following hold:

- 100% of required cases pass in the same run;
- zero unexpected skips, crashes, timeouts, non-zero exits, or leaked backend
  instances;
- every expected error has the exact code and expected domain, and has a
  non-empty message;
- initialization failure releases every partially retained native resource
  exactly once and permits a clean retry where the backend contract allows it;
- an active frame is either completed or explicitly aborted before reuse or
  shutdown; and
- repeated shutdown is a no-op with no additional native destruction.

An expected unsupported result passes only when the capability is optional or
the row is explicitly the current staged Vulkan boundary. It never turns an
unsupported required baseline operation into a backend pass.

### Native GPU rows

A native row is `PASS` only when `HORO_ENABLE_GPU_SMOKE_TESTS=ON`, the documented
display/device tuple is available, and every required smoke test completes with
zero unexpected skips or warnings promoted to failure. The current image-level
thresholds are intentionally small and explicit:

- OpenGL's 512x384 viewport must contain more than 10,000 pixels that differ
  from the clear background, and its center pixel must satisfy the existing
  blue-lit primitive bounds (`R < 150`, `G > 130`, `B > 150`).
- Metal's 128x128 viewport must have a channel range greater than 32 and the
  same center-pixel bounds; its staging-upload shutdown smoke must drain one
  upload and finish teardown without a crash.
- Resource replacement must publish a new target/texture identity while the
  previous target remains usable until publication, as asserted by both native
  viewport smokes.

Software OpenGL results qualify only the explicitly named software row. They do
not qualify hardware GPU or driver cohorts. Pixel mismatches, device loss,
timeouts, and missing required native lanes are failures or unqualified rows,
not retryable passes.

## Reproduction

The headless qualification composition used for the Linux/portable deterministic
row is:

```bash
cmake -S . -B build/qualification -G Ninja \
  -DBUILD_TESTING=ON \
  -DHORO_BUILD_EDITOR_GUI=OFF \
  -DHORO_BUILD_EXAMPLES=OFF \
  -DHORO_BUILD_RENDER_OPENGL=ON \
  -DHORO_BUILD_RENDER_METAL=OFF \
  -DHORO_BUILD_RENDER_VULKAN=ON \
  -DHORO_ENABLE_GPU_SMOKE_TESTS=OFF
cmake --build build/qualification --target \
  HoroRenderBackendRegistryTests HoroRenderOpenGLTests \
  HoroMetalDeviceCapabilityTests HoroRenderVulkanTests --parallel
ctest --test-dir build/qualification --output-on-failure \
  -R 'HoroRenderBackendRegistryTests|HoroRenderOpenGLTests|HoroMetalDeviceCapabilityTests|HoroRenderVulkanTests'
```

The Metal production target uses `HoroRenderMetalTests` instead of
`HoroMetalDeviceCapabilityTests` on macOS. A native lane adds
`-DHORO_ENABLE_GPU_SMOKE_TESTS=ON`, uses the host's documented display and
device setup, and runs only the registered `gpu`/`display` tests for that exact
backend/platform tuple. The command must record the compiler, OS/architecture,
window-system path, GPU/driver identity, validation setting, and whether the
result is hardware or software.

## Evidence ownership

The deterministic tests are ordinary Catch2 targets and are included in the
repository's CTest registration. GPU smoke tests are opt-in and are never
reported as passed unless the opt-in was enabled and the compatible lane really
ran. Hosted Sonar and CI reports may provide evidence for a lane, but they do
not change the matrix status of an absent or unsupported tuple.
