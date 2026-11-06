# Scene color pipeline integration

HORO-394 / #394 / [RND-013.3] introduces a backend-neutral color contract and an
explicitly admitted Metal Editor path. The implemented output is ACES 2 SDR100
Rec.709 into display-linear Rec.709, followed by SDR transfer encoding. The
Editor presents an explicitly diagnosed unmanaged SDR preview. This is not
calibrated display or HDR/PQ/EDR qualification.

## Scene and overlay interpretation

`SceneColorEncoding.h` requires a versioned source policy for canonical light
construction, including an empty light set. Linear ACEScg passes through;
version 1 can interpret legacy linear Rec.709/D65 or decode sRGB Rec.709/D65
before a pinned AP1 conversion. Nonfinite, unspecified and unrepresentable inputs
return typed errors; finite negative working components remain legal. The
serialized scene's separate prohibition on negative authored light RGB remains.

The Editor explicitly interprets its existing untagged authored light and
preview material colors as legacy linear Rec.709/D65 version 1. This is a new
host interpretation, not historical provenance or a persisted format change.
Extracted light tables and scene snapshots retain that source policy. New import
paths must declare their actual encoding.

With the admitted color family, mesh shading writes unexposed linear AP1 into
RGBA16F, without per-mesh Reinhard/gamma or selection decoration. BaseColor is
logical parameter 20; Metal SceneColorPolicy is parameter 21. Existing clustered
parameters 16–19 retain their identities. Current Metal lit uniform blocks are
704 bytes clustered and 1664 bytes forward; old cooked interfaces must be rebuilt.
The old BaseColor-only raw compiler fixture remains separately preserved.

The frame graph runs scene shading, bounded GPU exposure, optional creative
look, the pinned analytic output, display-linear grid/selection, and final SDR
encoding. Overlays therefore cannot affect the meter. Existing Editor UI tint
values are interpreted as sRGB code values by the explicit versioned overlay
adapter and decoded before display-linear composition; they are not AP1 scene
colors. Scene clear color is converted at the host boundary as well.

Without an admitted color family the existing bootstrap raster path remains.
Requesting a new color/exposure setting without its required package returns a
typed failure, rather than silently activating that legacy path.

## Exposure, history and resource ownership

`Exposure.h` describes finite stop-based settings, exact view/device/settings
identities and an 80-byte portable dispatch layout. Dispatch preparation admits
at most 16,777,216 source samples and a finite delta from zero to one second.
It does not read pixels or submit work.

The genuine three-stage GPU family clears 256 bins, meters bounded RGBA16F tiles,
then reduces percentiles and performs rate-limited EV adaptation. AP1 luminance
uses the Y row of AP1-to-XYZ. Finite negative components remain legal; nonfinite
samples are excluded. Bin-center quantization is part of this versioned meter.
Native constants are 96 bytes; the output state is 32 bytes with split 64-bit
submission/generation identity. No CPU histogram readback drives production.

Accepted submission N supplies the GPU exposure consumed by N+1 of the same
view. Metadata or a successful graph encoding alone does not establish acceptance:
actual Present and the native same-queue submission journal do. Unsent/cancelled
candidates preserve accepted history. Scene-domain, settings, output and device
identities govern reset and compatibility. The Editor uses accepted view-clock
cursors; its simulation-clock mode returns explicit unsupported status.

Four bounded color slots retain in-flight resources without ordinary frame waits.
Resource preparation gates each actual creation result; rollback retires only
that creation. Candidate resets defer to the acceptance boundary. Closed/failed
owners cannot be initialized again; recreate the owner through host lifecycle.
Native completion remains separate from CPU upload readiness.

Serial material draws track only their validated attachments, geometry buffers
and actual material resources. They must not mark later graph resources, such as
the exposure reset seed, as used by an unsent earlier draw. Genuine parallel graph
submission retains its separate whole-graph tracking. Strict seed completion and
foreign/predecessor rejection remain intact. The frontend aggregate retains up
to 1024 draws across individually bounded 128-draw passes; failed admission rolls
back material references and pool occupancy.

## Analytic output and creative look data

`CookedColorOutputProgram` owns original generated HLSL and two auxiliary tables,
with pinned CG config 4.0.0 / OpenColorIO 2.5.0 identities. The analytic output is
not a baked cubic approximation. Source/table hashes establish integrity, not
publisher trust, ACES conformance or native execution.

`CookedColorTransform` is creative-look-only. Executable descriptors use schema 2
and `RejectOutsideV1`; recreate obsolete schema-1 look descriptors from their
original samples. The analytic processor remains schema 1. A 2–65 cube retains
original finite red-fastest RGB and a derived RGBA32F atlas at `(r + N*g, b)` with
alpha-one padding. Trilinear and tetrahedral modes are explicit; exposure is
applied once before the look, and the original analytic processor follows it.
Outside-domain samples produce the declared diagnostic instead of implicit
clamping. Disabled look mode does not require look storage.

`PrepareColorTransformSelection` is the shared cold validator for immutable
analytic/look pins, matching configuration and complete storage budget. It does
not invent history or output identities. Nondefault numeric grading controls
currently return `UnsupportedGrading`; they are not silently ignored.

`scripts/renderer/color_pipeline_cook.py` produces the original analytic source,
tables and direct CPU reference corpus. `color_look_cook.py` accepts explicit
linear-AP1/red-fastest JSON samples and writes a closed three-file package:
`manifest.json`, `look.rgb.f32le`, `reference.json`. It uses the pinned actual OCIO
CPU processor for reference values; it does not import arbitrary cube formats or
claim GPU execution. The runtime loader validates fixed paths, sizes, hashes,
finite values and the canonical transform factory.

Authenticated offline `HoroColorOutputShaderCook --with-look` compiles the actual
third program; its schema-2 generation contains 22 files. Legacy schema-1,
18-file two-program generations remain readable. Exposure has three genuine
programs in its separate package. Runtime `HoroColorOutputPackageInternal` links
no compiler or process runner. Its ordinary-file checks reject observed symlinks
and static escapes; the host must keep the explicit cold-load root stable. They
are not a race-free rooted-file capability or publisher authentication.

## Editor configuration

Provide `--color-pipeline-root ABS` with complete `color/` and `exposure/` packages.
Add `--color-look-root ABS` to enable a separate three-file creative look package;
absence explicitly selects Disabled. Missing or foreign requested look data fails
with a typed error. There is no runtime cook or ambient SDK discovery.

Use `--lighting forward` with a genuine three-raster baseline package, or
`--lighting clustered` with genuine raster plus compute programs. Both feed the
same color graph; the baseline does not manufacture a culling kernel. Select the
package with `--lighting-root ABS`; `--lighting-fallback forbid` makes selection
explicit. `--exposure-mode automatic` enables the bounded GPU meter.

The optional CMake setting `HORO_EDITOR_COLOR_PIPELINE_ROOT` stages and installs
only the declared inventory under `assets/color-pipeline/`. Configure checks are
structural; runtime still validates the generation. Project renderer settings
must be compatible with the requested profile. An incompatible project restart
returns an observable typed error rather than dropping color/lighting options.

Presentation negotiation remains owned by `RenderPresentationOutput`. The
current Editor path requires the actual unmanaged native output contract and
reports it as an unmanaged SDR preview. HDR, PQ, EDR, and unsupported backend
profiles return typed errors; no different output is substituted.

## Local qualification and limits

Actual authenticated Metal cooks and raw compiler captures establish the current
shader interfaces. Separate native tests cover the public frontend, analytic
F32/F16 precision, both LUT interpolation modes, signed/HDR inputs, prior GPU
exposure consumption, pending uploads, cancellation, reset and in-flight resize.
CPU reference generation alone is not counted as GPU evidence.

The full viewport fixture executes both genuine forward and clustered recipes:
raw mesh FP16 and final pixels against independent pinned OCIO references,
actual directional HDR values above one, visible/occluded selection, grid depth
and complete raw-scene equality across overlay changes, and nonidentity LUTs in
both modes. UI tint expectations use independent IEC sRGB inverse-transfer
constants. FP16 storage and final one-LSB budgets remain separate; no universal
GPU rounding mode is claimed. Copies use admitted CopySource storage and actual
completion through the selected queue; no portable public readback API is added.

Real Editor runs opened a Metal project and its default scene with one mesh.
Both clustered/trilinear and forward/tetrahedral profiles completed 60 presented
frames, with 52 accepted color/exposure generations. An incompatible OpenGL
project produced the expected typed profile-preservation denial. These are local
macOS results, not hosted CI or other-platform clearance.

Physical calibrated display/HDR acceptance and actual hardware device-loss
injection are not established by these runs. Native generation rejection and
explicit shutdown/owner replacement are separate contracts. Local managed Sonar
is waived; no full Codacy, vulnerability or coverage clearance is claimed. The
canonical policy remains ADR-037 and the rendering architecture's scene-color
section. No persisted scene migration or old C++ binary compatibility is implied.
