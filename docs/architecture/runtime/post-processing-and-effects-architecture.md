# Post-Processing And Effects Architecture

## Purpose

This document defines the post-processing stack and screen-space effects for
Horo Engine. It covers the post-process volume system, individual effects
(bloom, depth of field, motion blur, ambient occlusion, tonemapping, color
grading), effect ordering, feature tiers, and GPU performance budgets.

XR session/view/composition ownership is deliberately outside this subsystem under
[ADR-157](../../adr/157-xr-ownership-runtime-composition-and-capability-tier.md).
Foveation is an admitted XR/Renderer plan and runtime asynchronous reprojection/timewarp
belongs to the native XR compositor; neither is a generic post-process effect. This
stack may consume an already admitted XR view like any other Render view, but it cannot
discover/select an XR runtime, choose a view configuration or submit composition layers.
The external-target, dynamic-resolution, foveation, auxiliary-input and layer boundary is
fixed by [ADR-160](../../adr/160-xr-rendering-openxr-compositor-and-renderer-ownership.md).
Post-processing cannot acquire/release runtime images, enable VRS/density/space warp,
fabricate missing depth/motion inputs or model guardian/chaperone as an effect.

## Scene Color And Output Contract

[ADR-037: Scene Color and HDR Architecture](../../adr/037-scene-color-and-hdr-architecture.md)
is the color authority. Post processing consumes linear ACEScg scene color in
canonical `RGBA16F`, one versioned `ExposureState` per view, and typed optional
semantic inputs from the active raster recipe. It does not infer color space from
a native texture format, maintain a private exposure value, or choose an output
curve from backend/display APIs.

Exposure and user compensation use base-2 stops. `+1 EV` doubles scene-linear RGB;
`-1 EV` halves it. Scene values are relative and may be finite, negative or above
one. Nits occur only in the resolved display-output plan. Effects declare whether
their resources are unexposed scene-linear, pre-exposed scene-linear,
display-linear, or transfer-encoded; graph validation rejects incompatible edges.

The production view/output transform is a pinned, cooked ACES 2 configuration.
Baseline SDR uses sRGB/Rec.709 primaries with sRGB encoding; the first HDR output
uses a Rec.2020 container and BT.2100 PQ under an admitted ADR-033 presentation
contract. The same scene result feeds either output. HDR capability never changes
the working gamut or enables an effect by itself.

## Post-Process Volume

Post-processing settings are defined by post-process volumes in the scene:

```cpp
struct PostProcessVolume {
    float             priority;
    PostProcessBlendRadius blendRadius;  // 0 = hard cut, >0 = smooth blend
    PostProcessSettings settings;
};
```

The active settings are computed by blending all volumes overlapping the
camera, weighted by priority and blend radius.

### Settings Structure

```cpp
struct PostProcessSettings {
    // Bloom
    std::optional<BloomSettings> bloom;

    // Depth of Field
    std::optional<DepthOfFieldSettings> depthOfField;

    // Motion Blur
    std::optional<MotionBlurSettings> motionBlur;

    // Ambient Occlusion
    std::optional<AmbientOcclusionSettings> ambientOcclusion;

    // Screen-Space Reflections
    std::optional<ScreenSpaceReflectionsSettings> ssr;

    // View/output transform and stop-based compensation
    ColorTransformId colorTransform;
    float            exposureCompensationEv;

    // Color Grading
    std::optional<AssetId> colorGradingLUT;   // 3D LUT texture
    ColorGradingSettings colorGrading;

    // Vignette
    std::optional<VignetteSettings> vignette;

    // Chromatic Aberration
    std::optional<ChromaticAberrationSettings> chromaticAberration;

    // Film Grain
    std::optional<FilmGrainSettings> filmGrain;
};
```

## Effect Pipeline

### Render Graph Integration

Post-processing is implemented as typed render graph passes:

```text
Raster semantic inputs ──→ Scene-referred effects ──→ Exposure + creative look
    ──→ ACES target output transform ──→ Display-referred UI
    ──→ Accessibility ──→ Gamut containment + dither + transfer encoding
    ──→ Typed presentation output
```

Effects declare typed reads/writes, color representation, exposure generation and
history dependencies. The graph orders only required edges; SSAO, for example,
produces an input consumed during scene lighting rather than pretending every
effect is a serial color filter. Enabling/disabling settings requests a new graph
plan at a render safe point. Compatible passes may be fused only when precision,
color/exposure semantics, diagnostics and observable output remain equivalent.

### Pass Dependencies

| Stage | Required inputs | Representation/output |
|---|---|---|
| Ambient occlusion | Depth, normal | Scene-linear scalar/visibility input consumed by lighting/composition. |
| Screen-space reflections | Scene color, depth, normal, roughness and declared history | Scene-linear reflection contribution plus confidence. |
| Depth of field | Scene color, linear depth, camera/lens state | Scene-linear color with explicit history/exposure metadata. |
| Motion blur | Scene color, velocity, depth and declared history | Scene-linear color with explicit history/exposure metadata. |
| Bloom | Scene color and exposure state | Scene-linear additive contribution; thresholds use declared scene/exposure semantics. |
| Creative grading/look | Exposed ACEScg scene color, cooked transform | Exposed scene-referred color before the output transform (ADR-037 step 3). |
| Target output transform | Graded color, output contract | Display-linear target-gamut color. |
| Display-referred UI compose | Display-linear scene, UI at reference white | Complete composed display-linear image. |
| Accessibility color transform | Composed image, `IColorAccessibilityQuery` snapshot | Complete composed image including HUD (ADR-037 step 6). |
| Final encode | Accessible composed image, output contract | Dithered SDR/PQ encoded presentation image. |

Performance budgets are finite product settings measured for a stated backend,
resolution, content fixture and build mode. This architecture does not assign
universal millisecond values to an effect or infer cost from a profile label.

## Individual Effects

### Bloom

```cpp
struct BloomSettings {
    float  thresholdEv;     // exposed luminance stops relative to neutral 0.18
    float  intensity;
    float  scatter;         // bloom spread (controls downsample chain)
    uint32_t quality;       // downsample levels
};
```

Bloom uses a downsample-upsample chain with separable Gaussian blurs.
`thresholdEv` is evaluated against exposed ACEScg luminance. The linear threshold
is `0.18 * exp2(thresholdEv)`, so view exposure changes the measured luminance and
preserves the intended photographic look. A separate absolute scene-linear
threshold would require a different typed setting; an unqualified scalar is not
accepted.

### Depth Of Field

```cpp
struct DepthOfFieldSettings {
    float  focusDistance;    // world units from camera
    float  apertureFStop;    // lower = shallower DoF
    float  focalLength;      // mm
    uint32_t quality;        // sample count for bokeh
};
```

DoF uses a circle-of-confusion based approach with bokeh shape sampling.

### Motion Blur

```cpp
struct MotionBlurSettings {
    float  intensity;
    float  maxBlurPixels;
    uint32_t sampleCount;
};
```

Motion blur reconstructs per-pixel velocity from the previous frame's camera
matrix and object transforms. It consumes the active raster recipe's declared
velocity resource; Forward/Forward+ may provide a velocity prepass, so motion blur
does not select or require Deferred implicitly.

### Ambient Occlusion

```cpp
struct AmbientOcclusionSettings {
    SSAOMode mode;            // GTAO, HBAO+, SSAO
    float    radius;
    float    intensity;
    float    power;
    uint32_t quality;         // sample count
};
```

### Tonemapping

The production transform is the pinned ACES 2 configuration from ADR-037, not an
unversioned “ACES filmic” approximation. `Neutral` is a separately versioned
diagnostic transform. Reinhard, Uncharted-style and arbitrary backend curves are
not automatic fallbacks; an explicit creative transform requires cooked identity,
target qualification and project migration.

### Color Grading

```cpp
struct ColorGradingSettings {
    float  temperature;      // Kelvin
    float  tint;
    float  saturation;
    float  contrast;
    float  gamma;
    Vector4 colorWheels[3];  // shadows, midtones, highlights
};
```

A 3D LUT texture can be applied for full creative grading. `ColorGradingSettings`
and look LUTs are ADR-037 pipeline step 3: scene-referred, before the ACES
output transform. They are not the colorblind path.

### Accessibility Color Transforms

Colorblind filters use backend-neutral desired settings captured from an immutable
`ConfigurationSnapshotRef` at render-frame setup. The renderer owns applying the
3×3 transform as ADR-037 `ColorPipelinePlan` step 6: after the ACES output
transform and display-referred UI compose, on the complete composed image
including HUD. It does not own gameplay's accessibility preferences, share
storage with `ColorGradingSettings`, or expose live renderer state to gameplay.

`IColorAccessibilityQuery` is defined by the backend-neutral visual-settings
contract in [Accessibility Architecture](./accessibility-architecture.md), not by
PostProcessing. Each query instance retains one snapshot revision. UI, gameplay
and worker consumers read their own captured revision synchronously without a
render-thread call, wait or dependency on the renderer implementation. The query
reports desired mode/severity/contrast, not GPU application status. Non-color cues
(icons, patterns, text) are produced independently by gameplay/HUD.

Colorblind keys belong to the visual-settings domain under
`accessibility.colorblind.*`; UI contrast belongs to `accessibility.visual.ui.*`.
Motion/flash policy belongs to `accessibility.visual.safety.*`. Render consumes
these keys without re-registering or maintaining a competing mutable preference.

All quality configurations preserve accessibility semantics, including compact
ALU implementations on lower-cost backends. Product quality settings do not gate
accessibility or define navigation compute tiers. Accessibility passes must not
introduce synchronous CPU/GPU readback.

## Performance And Product Profiles

ADR-028 product profiles select ordered preferences, not API families or fixed
sample counts. Each effect recipe declares required capabilities, formats,
semantic inputs, resolution/quality parameters, finite memory/work budgets,
cooked variants and an explicit fallback. A setting is admitted only after the
active raster and color plans prove the complete dependency set.

Baseline may omit optional effects but must preserve the same ACEScg, exposure,
output and accessibility semantics. Standard, High and Ultra can request higher
quality or additional effects only through typed product settings. Missing
compute, history, GBuffer semantics or a cooked variant selects a declared lower
effect recipe or reports an unavailable required feature; it never switches
backend, working color space, tone curve or HDR mode silently.

## Editor Integration

Post-process volumes are placed as scene objects:

- Volume gizmo in the viewport for placement and resizing
- Volume priority and blend radius editing in the inspector
- Post-process settings panel with live preview

The editor viewport renders with post-processing when "Lit" or "Shaded" view
mode is active. Individual effects can be toggled in the viewport toolbar for
debugging.

## Related Documents

- [Post-Processing Stack UI Reference](../../../mock-studio/designs.md#architecture-runtime-post-processing-stack)

- [Rendering Architecture](./rendering-architecture.md): render graph and pass definitions
- [Advanced Rendering Architecture](./advanced-rendering-architecture.md): ray-traced effects
- [Material And Shader Model](./material-and-shader-model.md): shader permutations for effects
- [Observability Performance](../observability/observability-performance.md): GPU timing for post-process passes
- [Editor Panel Host](../editor/editor-panel-host.md): post-process volume inspector
