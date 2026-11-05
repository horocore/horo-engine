# Scene color pipeline migration

HORO-394 / #394 / RND-013.3 is in progress. This document separates the
backend-neutral contract work from the native rendering changes still required
before the ticket can move to review.

## Existing behavior and migration boundary

The Editor viewport currently applies Reinhard-style display conversion and
approximate gamma inside mesh shaders, then writes an RGBA8 target. Adding a
final exposure pass after that target would operate on already clipped,
display-encoded values. It would not implement scene-referred exposure.

## Current foundation

`Exposure.h` defines finite stop-based settings, exact view/device/settings
generations and an 80-byte portable dispatch-parameter layout. Automatic state
identifies producer submission N and consumer N+1 of the same view. Validating
that metadata does not prove that GPU work ran or completed. Manual/initial
exposure carries its selected CPU value; it cannot claim an automatic GPU buffer.
Dispatch preparation admits at most 16,777,216 source samples and a caller-selected
finite delta between zero and one second. It neither reads pixels nor submits work.

`ColorPipeline.h` admits immutable, bounded cubic RGB32F payloads and a cold color
plan over exact scene, exposure, output and history identities. Payload SHA-256
uses little-endian IEEE754 float bytes, including signed zero. The factory copies
the payload; hashes establish integrity only. Configuration and reference digests
are required identity fields, not independent proof of cook trust, an Academy
conformance test or a native renderer result. The production output role never
silently falls back to `NeutralDiagnostic`.

The foundation was compiled on macOS Debug together with the RenderApi public
header consumers. Twenty focused cold-value tests passed with no failures or
skips, covering finite bounds, enum vocabulary, region overflow/work limits,
submission compatibility, payload corruption, ownership, exact output admission,
cancellation and prior-candidate preservation. All 55 measured functions in the
nine new C++ files have cyclomatic complexity at most 10; clang-format 19 passed.
These are local foundation results, not native rendering or full-ticket clearance.

## GPU exposure work in progress

The closed compute family in `assets/shaders/runtime/exposure.hlsl` clears 256
integer bins, builds them from bounded RGBA16F scene tiles, then performs a fixed
prefix/reduction and rate-limited EV adaptation. It uses AP1 luminance, the Y row
of AP1-to-XYZ in the [Khronos Data Format Specification, section 14.7](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.pdf).
Finite negative scene components remain legal; non-finite samples are excluded.
Percentile selection weights the overlap of each histogram bin, so bin-center
quantization is part of this versioned meter, not an exact continuous-luminance
measurement.

The device output is 32 bytes and retains split 64-bit generation/submission
identity. Dispatch constants are 96 bytes. The following view consumes the GPU
buffer; recording a graph or supplying matching ordinals does not establish native
submission. The native owner must independently authorize the exact preceding
buffer, view, device and genuinely committed same-queue command.

Offline `HoroExposureGpuShaderCook` uses explicitly authenticated compiler tools,
the real shader compilation/reflection pipeline and atomic package publication.
Package creation requires all three genuine stage artifacts and a successful
decode of the written bytes. All three current stages were genuinely compiled,
reflected, native-compiled and atomically published on the local Mac. No catalog,
trust policy or analyzer configuration was changed to admit them.

The actual Metal fixture passed 12 exposure cases with zero failures/skips:
AP1 luminance, partial tiles, percentile weighting, EV sign/adaptation, finite and
empty samples, next-submission identity including 32-bit carry, invalid-state
propagation, explicit reset, uncommitted/abandoned/foreign/stale evidence and
last-good preservation. A real render-attachment clear feeds the meter earlier
in the same native command buffer. A different unsent command remains rejected.
Two existing native buffer-copy/color-clear regressions also passed. Execution
was explicitly selected with `[exposure]` and the two regression names, using
the genuine package through `HORO_EXPOSURE_GPU_PACKAGE`; this does not claim
that GPU CTest discovery was enabled globally.

The cold API suite passed 27 cases and the captured compiler-evidence suite
passed nine cases. Those exact executables and their inputs were unchanged by
the final native resource/test correction; their prior passing execution is
retained rather than counted again. Public RenderApi consumers compiled, as did
the owning Metal/frontend and explicit unsupported Null/OpenGL paths.

Successful resource creation now associates only its own GPU upload. Empty
buffers cannot inherit an unrelated texture's pending upload; the global upload
retention needed for shutdown remains intact. Journal pruning retains the last
actually accepted state while a replacement remains unsubmitted, so abandoning
the replacement preserves retry from the accepted predecessor.

The native encoder packs explicit reset in bit 31 of the GPU clock lane while
leaving the portable authored clock enum unchanged. Without reset, an invalid
GPU predecessor produces `Invalid` state and stays invalid until explicit reset.
No mismatching predecessor silently becomes an initial-value success.

This is scoped exposure qualification using the concrete private native owner.
It does not establish public frontend import/handle association end to end, an
interactive Editor result, the final ACES/look/LUT color pass, calibrated HDR
presentation, or complete HORO-394 acceptance. Local managed Sonar was waived;
format and complexity checks do not constitute full Codacy/coverage/hosted gates.
The added backend virtual and graph workload require engine, frontend and
concrete renderer consumers to rebuild together; old C++ binary compatibility
is not claimed.

The migration therefore needs a retained RGBA16F linear ACEScg scene target.
Mesh shading, scene-referred VFX and other scene effects must write that target
without output encoding. Exposure and creative look processing consume scene
color before one final pinned output transform. Display-referred UI joins in
display-linear space afterward; accessibility processing covers the complete
composition before final transfer encoding. UI and editor selection decoration
must not contribute to the scene luminance meter.

Presentation output negotiation remains owned by
`RenderPresentationOutput`. A successfully negotiated native encoding does not
mean the source image has received a scene-to-display conversion. The color
pipeline must target that exact admitted encoding; it cannot silently select a
different transfer function, gamut or precision.

## Full-ticket acceptance checklist

- [x] Establish finite, typed exposure and color-plan inputs with complete view,
  output, transform and resource-generation identities.
- [ ] Cook and verify the exact pinned ACES 2 output transform and creative LUT
  data. A diagnostic identity transform is never a production fallback.
- [x] Meter actual GPU scene color and adapt exposure with bounded work, retaining
  one published state per view for the next submitted view. Same-frame CPU
  histogram readback and CPU metering are outside this production contract.
- [ ] Preserve scene-linear FP16 values through the actual viewport render path;
  remove per-mesh display conversion when the final native path takes over.
- [ ] Execute the admitted look and output transform with the selected backend,
  preserving native resource and completion leases through in-flight work.
- [ ] Integrate actual host publication, diagnostics, reset, cancellation, resize,
  output change and device-loss behavior. Failed candidates retain a compatible
  previous plan or return an explicit typed unavailable state.
- [ ] Run native pixel comparisons with real cooked transforms, HDR input values,
  identity and nonidentity LUTs, successive-frame adaptation and lifecycle
  transitions. Unit and graph-plan tests alone do not establish this result.

The canonical policy is ADR-037 and the scene-color section of the rendering
architecture. No native backend, calibrated HDR display, full Editor rendering
or complete ticket acceptance is qualified by this in-progress migration note.
