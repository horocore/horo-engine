# Material And Shader Model

## Purpose

This document defines Horo Engine's material and shading architecture. It
specifies the standard PBR material model, shader variant system, material asset
format, material instances, render feature tiers, and the boundary between
authoring content and runtime shader compilation.

The goal is to give artists and technical artists a stable material contract
while giving the renderer enough information to compile efficient shader
variants, batch render instances, and fall back gracefully on lower-end
hardware.

## Scope

Covered:

- standard PBR material model and parameter set
- material asset format and serialization
- material instances and overrides
- shader variants, permutation keys, and compilation policy
- feature tiers and capability-driven fallback
- pipeline cache and offline shader compilation boundary
- integration with the asset pipeline, render graph, and scene runtime

Not covered:

- specific renderer backend implementations (see
  [Rendering Architecture](./rendering-architecture.md))
- texture import settings (see [Asset Pipeline](./asset-pipeline.md))
- lighting and shadow algorithms (see
  [Advanced Rendering Architecture](./advanced-rendering-architecture.md))

## Core Decisions

- Materials are authored assets. They reference a shader graph or shader source
  and declare parameters and feature flags.
- Material instances override parameters without duplicating shader bindings or
  variant selection logic.
- The standard PBR material model is the default and must be supported by every
  renderer backend.
- Shader variants are produced from explicit permutation keys, not from runtime
  string substitution.
- Product profiles express quality preferences, not hardware capability sets.
  Materials and shader variants declare explicit feature, format and limit
  requirements. The frontend selects an admitted recipe and records every fallback
  under [ADR-028](../../adr/028-renderer-capability-limits-and-product-profiles.md).
- Pipeline state objects are cached and reused. Shader compilation may happen
  during cook or explicit editor/development preparation under
  [ADR-035](../../adr/035-shader-source-and-intermediate-representation.md).
  ADR-035 is the sole source-language, compiler-route, reflection, and packaged
  compilation authority; this material model only supplies semantic inputs and
  consumes its validated artifacts.
  Portable artifacts, native driver caches and live GPU objects have distinct
  ownership and compatibility keys.
- No gameplay code queries raw shader handles. Gameplay sees only material names,
  parameter overrides, and feature flags.
- Virtual-texture integration follows
  [ADR-164](../../adr/164-virtual-texturing-ownership-product-scope-and-capability-tier.md):
  Materials owns semantic sampling intent and required/fallback variants, while VTX
  owns logical page state and Renderer owns physical resources and bindings. Variant
  selection is admitted before activation and never generated on a runtime page miss.
- [ADR-168](../../adr/168-vtx-gpu-page-table-physical-cache-shader-and-material-ownership.md)
  further requires typed logical VTX slots and offline reflected sampling variants.
  Material assets/instances never store atlas coordinates, sparse tiles, descriptor
  indexes or mapping revisions; render extraction resolves a per-frame binding lease.

## Shader Graph Editor Surface

Shader and material graph authoring uses the shared editor graph surface defined
in [Editor Panel and Tab Architecture](../editor/editor-panel-host.md). The first
production graph surface is built on `imgui-node-editor` behind a private Horo
adapter; `imnodes` is reserved for prototype or simple internal graph tools.

The node editor widget is not the shader compiler and not the material schema.
It renders graph snapshots and emits user-intent commands. The material/shader
subsystem owns:

- graph asset schema and stable node, pin, and link identity;
- type checking between pins;
- cycle and dependency validation;
- texture, sampler, material parameter, and feature-tier validation;
- shader-code generation or graph IR emission;
- diagnostics and source/graph location mapping;
- cook-time variant generation and runtime fallback policy.

Node positions, collapsed state, selection, and zoom are editor presentation
state. Shader graph semantics are serialized through stable graph asset data,
not through third-party widget IDs or layout state.

AI-assisted shader editing may propose nodes, links, generated shader source, or
parameter changes, but it must go through graph edit commands and material
validation before any asset is modified. The assistant cannot inject raw shader
source into a material asset without diagnostics, preview, and undo integration.

## Material Model

### Standard PBR Parameters

Every standard PBR material exposes these parameters:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `albedo` | `Vec3` or texture | `(0.5, 0.5, 0.5)` | Base color. |
| `metallic` | `float` or texture | `0.0` | 0 = dielectric, 1 = metal. |
| `roughness` | `float` or texture | `0.5` | Perceptual roughness. |
| `normal` | texture | none | Tangent-space normal map. |
| `occlusion` | `float` or texture | `1.0` | Ambient occlusion multiplier. |
| `emissive` | `Vec3` or texture | `(0, 0, 0)` | Emissive color. |
| `emissiveIntensity` | `float` | `1.0` | Scalar emissive multiplier. |
| `opacity` | `float` or texture | `1.0` | Alpha value for translucent/masked. |
| `opacityMaskThreshold` | `float` | required for `Masked` | Alpha-test threshold for masked. |

The standard shader uses a metallic-roughness workflow. Specular-glossness is not
a core workflow but may be added through custom shaders.

### Blend And Shading Modes

| Mode | Behavior | Use case |
|---|---|---|
| `Opaque` | No transparency, writes depth. | Most surfaces. |
| `Masked` | Alpha test, writes depth, no sorting. | Foliage, grates, decals. |
| `Translucent` | Blended transparency, sorted back-to-front. | Glass, water, fog. |
| `Additive` | Additive blend, no depth write. | VFX, glows. |

| Shading Model | Notes |
|---|---|
| `Lit` | Default PBR. |
| `Unlit` | Emissive/opacity only. |
| `Hair` | Optional; requires anisotropic scattering. |
| `Cloth` | Optional; sheen and fuzz layers. |
| `Subsurface` | Optional; translucency and scattering. |
| `ClearCoat` | Optional; dual normal/roughness layer. |

New shading models must register their required vertex attributes, permutation
keys, and render passes. Required shading models fail admission when unavailable.
Optional models may select only an authored, cooked and product-declared fallback;
the renderer records that edge and never skips a model from an API name or profile
label. `Hair`, `Cloth`, `Subsurface`, and `ClearCoat` therefore depend on exact
effective capabilities, limits and target variants rather than legacy `es3`/`dx11`
tiers.

### Material Input Types

Material parameters may be:

- scalar `float`
- `Vec2`, `Vec3`, `Vec4`
- `Color` (sRGB or linear depending on semantic)
- texture `AssetId` with optional sampler state override
- boolean feature toggles

Boolean feature toggles participate in shader variant generation. Scalar and
color parameters do not. Texture presence usually participates in variant
generation unless the shader uses bindless default descriptors.

## Material Asset Format

A material asset is a JSON document stored as `.horomat` or produced from a
shader graph `.horoshadergraph`.

```json
{
  "schemaVersion": 1,
  "assetGuid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "assetType": "material",
  "sourceShader": {
    "kind": "standard",
    "variantSet": "core.shaders.standard"
  },
  "parameters": {
    "albedo": {
      "type": "texture",
      "value": "texture_brick_001"
    },
    "roughness": {
      "type": "float",
      "value": 0.8
    },
    "metallic": {
      "type": "float",
      "value": 0.0
    }
  },
  "features": {
    "normalMap": true,
    "emissive": false,
    "clearCoat": false
  },
  "renderState": {
    "blendMode": "Opaque",
    "shadingModel": "Lit",
    "cullMode": "Back",
    "depthTest": "LessEqual",
    "depthWrite": true
  },
  "minProfile": "baseline",
  "preferredProfile": "high",
  "profileOverrides": {
    "baseline": {
      "features": { "normalMap": false },
      "parameters": { "roughness": 1.0 }
    }
  }
}
```

The asset pipeline cooks this into a runtime material descriptor plus a set of
shader variant requests. The profile fields show the target contract; existing
`minTier`/`preferredTier`/`tierOverrides` assets require the staged ProjectVersion
migration in ADR-028 before a parser or writer adopts it. The example does not
announce a schema implementation or bypass the existing project-version authority.

## Material Instances

A material instance inherits shader, feature flags, and render state from a
parent material and overrides a subset of parameters.

```json
{
  "schemaVersion": 1,
  "assetGuid": "b2c3d4e5-f6a7-8901-bcde-f23456789012",
  "assetType": "material_instance",
  "parentMaterial": "a1b2c3d4...",
  "parameterOverrides": {
    "albedo": { "type": "texture", "value": "texture_concrete_002" },
    "roughness": { "type": "float", "value": 0.9 }
  }
}
```

Rules:

- Instances cannot change feature flags or render state that affect the shader
  variant key. Those changes require a new material.
- Scalar, color, and texture **value** overrides do not change the variant key.
  Changing a texture override from one existing texture to another does not alter
  the "texture present" permutation; only the bound descriptor changes.
- Instances share the same pipeline state objects and shader variants as the
  parent whenever possible.
- Overrides are stored in the scene object component, not in the parent material
  asset.
- Runtime material instances live in the scene runtime material table and are
  keyed by parent material plus override hash.

## Shader Variant System

A shader variant is a fully compiled shader program produced from a base shader
plus a permutation key.

### Permutation Key

```cpp
struct ShaderPermutationKey {
    ShaderId shaderId;
    FeatureMask featureMask;
    RenderPassId passId;
    VertexLayoutId vertexLayoutId;
    TargetPlatformId platform;
};
```

`RenderProductProfile` is an admission and recipe-selection axis, not a compile
axis of its own. Profile resolution chooses which declared permutation to bind.
Compile-time differences that a profile would otherwise imply must appear as
declared feature flags or as a distinct shader/pass identity. Profile-only
parameter or uniform overrides stay out of the key and do not duplicate binaries.
Two admitted profiles that resolve to the same `FeatureMask`, pass, vertex layout
and platform share the compiled variant.

Feature flags that participate in the key must be declared explicitly in the
shader manifest. Implicit feature detection from material parameters is not
allowed.

The complete cook/cache identity also includes backend shader format, versioned
profile/recipe policy and shader layout. Runtime plan caches include effective
capability and settings revisions. A profile name alone cannot select a variant
whose requirements have not passed admission.

### Variant Compilation Policy

- **Cook-time**: compile all required declared permutations for the selected target
  set through ADR-035's HLSL/SPIR-V or direct DXIL route. Publish payloads and
  validated Horo reflection/target maps together.
- **Editor/development**: explicit bounded compilation requests may build new
  variants using the same target descriptor and diagnostics. Pending work never
  blocks the frame loop; stale/failed candidates cannot replace the last good one.
- **Packaged game/headless/server**: require declared cooked artifacts; no missing
  variant may trigger HLSL/graph compilation. Controlled native realization of a
  cooked artifact, such as GL compile/link of cooked GLSL, remains necessary.

A missing required variant is a typed error. Only predeclared, cooked and admitted
optional fallback variants may replace it; platform source-compiler availability
is not permission to invent a runtime variant.

### Variant Explosion Guard

- Feature flags must be opt-in in the shader manifest.
- The cook profile may cap the maximum number of variants per shader.
- Materials that request unsupported feature combinations produce errors during
  import, not at runtime.

`ShaderPermutationModel` is the implemented backend-neutral admission contract.
It publishes a canonical feature-bit table and an explicit, finite list of
admitted masks; the engine never expands the feature table into an implicit
Cartesian product. Both the model's own cap and the caller's hard validation
envelope apply before selection. `ResolveShaderPermutation` accepts only a listed
mask and never invents or compiles a fallback.

`ShaderSpecializationValue` is deliberately separate from
`ShaderPermutationKey`. Values must match manifest-declared specialization IDs
and types, are completed from declared defaults, and remain runtime binding data.
Changing one therefore cannot multiply compiled variants. The contract is inert
and synchronous: it owns no worker, callback, native object, cancellation, or
shutdown path. Compilation scheduling and cooked-artifact publication remain at
the host/Asset Pipeline boundary described above.

## Product Profiles And Variant Admission

[ADR-028](../../adr/028-renderer-capability-limits-and-product-profiles.md) is the
single authority for `baseline`, `standard`, `high`, `ultra`, their resolution
order and migration from API-named tiers. There is no material-local GPU tier
classifier. The frontend supplies a resolved policy based on effective features,
typed limits, complete format/usage/sample predicates and allowed cooked variants.

A material's `minProfile` restricts profile eligibility; `preferredProfile` records
author intent without overriding host/user policy. Neither proves hardware support.
`profileOverrides` patch declared feature choices and parameters for an eligible
selected profile, with keys restricted to the canonical profile names. A missing
override retains the base values only when that exact variant passes admission.
Required features are never silently removed. Optional feature fallbacks must be
declared and cooked; missing required variants or incompatible requirements produce
typed errors instead of successful degraded conversion.

Cooking uses versioned target requirement manifests, not the cook host's GPU.
Runtime rechecks every selected recipe against the active effective snapshot.
Profile fallback follows ADR-028: an omitted product fallback list uses the
canonical descending allowed chain; an explicit list is used as written. Fallback
cannot relax material minimums or required content features. Resolution records
selected variants and reasons even when quality degrades within the same profile
name.

## Pipeline Cache

ADR-035 distinguishes Asset Pipeline's cooked shader artifacts, the backend's
optional native driver/pipeline acceleration blobs, and the renderer registry's
live GPU objects. `ShaderPermutationKey` is a logical index; full artifact identity
also includes source/include digests, manifest/target/layout versions, toolchain
builds and compiler options. Native blob compatibility additionally includes the
device/driver identity required by the backend.

Live pipelines/layouts/handles are never serialized. A shipped native cache is
only acceleration: reject incompatible blobs and prepare from the admitted cooked
artifact at a permitted non-frame-hot boundary. Cache invalidation does not permit
an implicit source cook or claim that native pipeline creation can never compile.

Material parameters remain typed semantic values. Renderer packing uses the
selected artifact's validated target offsets/strides and binding map; C++ struct
layout or another backend's reflection cannot substitute for that map.

## Renderer Integration

### Terrain Material Boundary

[ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md)
applies this model to Terrain/Foliage. Terrain authoring/cook owns semantic layer order,
weights, holes, material-function references, legal per-tile layer masks and required
blend behavior. Material/Shader cook validates those inputs and emits explicit finite
target permutations with normalized reflection. RenderFrontend selects and binds only a
cooked permutation compatible with the exact Terrain payload, raster recipe and effective
capabilities; the backend cannot change layer count, disable holes or substitute a shader.

Terrain `Baseline`/`Standard`/`High`/`Ultra` and renderer product profiles are independent
typed axes. Equal labels do not imply conversion or a hidden compile flag. Profile-only
numeric changes remain parameters; shader-byte or interface changes appear as declared
features, pass/layout identity or target-artifact identity. TRF-003.2 owns the exact
Terrain material permutation key and layer/blending limits within this boundary.

Opaque, Masked, TransparentSorted and TransparentAdditive classification is authored and
cooked material meaning, not an extractor guess from foliage type. Missing required
texture, reflection, permutation, layer capacity or pass compatibility fails preparation.
An optional lower variant must be explicitly declared, cooked and recorded by the product
plan.

### Material Table

The scene runtime owns a `MaterialTable` that maps `MaterialId` to:

- parent material asset
- resolved parameter block
- declared feature/classification and permutation requirements

This semantic table contains no resident shader, pipeline state object or native binding.
RenderFrontend owns a generation-scoped realization that maps `MaterialId` plus packed
`MaterialBindingId` to the admitted cooked variant, renderer resources and pipeline.
The semantic table is populated during scene conversion. Changing the parent material or
feature flags at runtime requires an explicit material swap. Editing per-instance
parameter overrides does not require a swap; the scene runtime updates the instance's
entry under a new override hash while the parent material remains shared.

### Render Extraction

Mesh instances extract:

- `MaterialId`
- per-instance parameter overrides (when supported)
- transform, bounds, and visibility flags

The render frontend groups instances by material and shader variant for batching.

### Pass Binding

Materials declare which render passes they participate in:

- `OpaqueForward` / `DeferredGBuffer`
- `ShadowCaster`
- `DepthPrepass`
- `Translucent`
- `MotionVectors`
- `CustomN`

The render graph queries the RenderFrontend realization for the exact admitted variant
per pass and retains its generation through execution.

### GBuffer Contract

When a material participates in the deferred path, the active shader declares
its GBuffer output layout. The standard PBR material model defines a canonical
layout so that lighting, decal, SSR/SSGI, and post-processing passes can consume
GBuffer data without per-material negotiation.

The standard GBuffer includes, at minimum:

| Slot | Content |
|---|---|
| `GBUF_ALBEDO` | Albedo (RGB) and opacity (A) if needed. |
| `GBUF_NORMAL` | Tangent-space normal (RG) and roughness/metallic packed bits. |
| `GBUF_MATERIAL` | Metallic, roughness, occlusion, and material flags. |
| `GBUF_EMISSIVE` | Emissive color (RGB) and emissive intensity scalar. |
| `GBUF_MOTION` | Per-pixel motion vectors for TAA and motion blur. |

Custom shaders may append additional slots but must not reorder or remove the
standard slots. The renderer backend compiles the declared layout into a stable
`GBufferLayoutId` used by downstream passes.

## Shader Graph

A shader graph is an authoring-time visual or textual representation that
generates shader source. It is not interpreted at runtime.

- Graphs compile to shader source during import.
- Generated source must conform to the shader manifest contract.
- Custom nodes must declare required features, inputs, outputs, and target
  passes.
- Graph assets ship only in development builds for hot-reload; they are stripped
  from release builds.

## Diagnostics And Validation

Material validation rules:

- all referenced textures must exist and match expected usage
- parameter types must match the shader manifest
- feature flag combinations must be legal
- `minProfile` and required variant predicates must be satisfiable by every
  declared target requirement manifest
- translucent materials must not request opaque-only passes
- masked materials must explicitly provide `opacityMaskThreshold`

Cook-time diagnostics:

- list of emitted variants per shader
- declared optional variants excluded by the requested profile/target policy
- missing required variants are errors; only declared optional exclusions may be
  reported without failing publication

## Testing Requirements

- Unit tests for permutation key equality and hash stability.
- Tests for material instance override inheritance.
- Cook tests that verify variant emission for each standard feature flag.
- Runtime tests for explicit optional-feature fallback, missing required variants,
  driver restrictions, stale capability revisions and profile-policy rejection.
- Terrain tests for layer/hole/blend permutation admission, material classification and
  Terrain-tier/render-profile independence without implicit backend/profile fallback.
- Visual regression tests for standard material spheres under representative
  lighting.

## Related Documents

- [Shader Graph UI Reference](./shader-graph-editor.html)

- [Material Editor UI Reference](./material-editor.html): preview, shader domain, texture slots, parameters, and tier compatibility panel.

- [Rendering Architecture](./rendering-architecture.md): render graph, backend
  abstraction, pass extraction.
- [Asset Pipeline](./asset-pipeline.md): import, cook, cache, and platform
  profiles.
- [Advanced Rendering Architecture](./advanced-rendering-architecture.md):
  lighting, shadows, global illumination, post-processing.
- [Built-In Scene Primitives](./built-in-scene-primitives.md): default material
  assignment and vertex layout.
- [Terrain And Foliage Architecture](./terrain-and-foliage-architecture.md): Terrain
  layer, candidate and render-plan ownership.
- [ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md):
  Terrain material/permutation, profile and renderer-realization boundary.
