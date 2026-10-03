# Decal System Architecture

## Purpose

This document defines the decal system for Horo Engine. It covers deferred
decals, mesh decals, decal projection, decal material model, decal lifetime
management, pooling, and editor authoring tools.

## Decal Types

### Deferred Decals

Deferred decals are projected onto GBuffer surfaces:

- A projection box defines the decal volume
- The decal shader reads GBuffer data (position, normal, material properties)
  within the box
- Decal material is blended onto the underlying surface
- Decals are rendered as a separate render-graph pass after the GBuffer

```cpp
struct DeferredDecal {
    WorldTransform   transform;       // projection box transform
    Vector3          extents;         // box half-extents
    AssetId          decalMaterial;
    float            fadeAlpha;
    uint32_t         sortOrder;
    DecalBlendMode   blendMode;
};
```

Blend modes:

- **Translucent**: Alpha-blended overlay (bullet holes, dirt)
- **Stain**: Multiplicative color (blood, scorch marks)
- **Normal**: Replace or blend normal map (surface detail)
- **Emissive**: Additive glow (energy marks, holograms)

### Mesh Decals

Mesh decals wrap a decal mesh onto target geometry:

- A planar projection is applied to a decal mesh
- Vertices are snapped to the target surface using depth sampling
- Useful for complex shapes (logos, painted lines on terrain)
- Mesh decals are static mesh instances with a special decal material domain

```cpp
struct MeshDecal {
    AssetId       decalMeshId;
    AssetId       decalMaterial;
    WorldTransform baseTransform;
    float          projectionDepth;   // max distance to project onto surface
};
```

## Decal Material Model

Decal materials reference a subset of the full material model:

```cpp
struct DecalMaterial {
    AssetId    baseColorTexture;
    AssetId    normalTexture;
    AssetId    roughnessTexture;
    AssetId    metallicTexture;
    AssetId    emissiveTexture;
    float      normalBlendIntensity;
    float      roughnessContribution;
    Color      baseColorTint;
    Color      emissiveTint;
    DecalBlendMode blendMode;
    float      alphaCutoff;
};
```

Decal materials are authored in the material editor with a decal-specific
shader domain. They compile to a decal shader permutation.

## Decal Lifetime

Decals can be:

- **Permanent**: Placed at authoring time, baked into scene data
- **Runtime**: Spawned during gameplay (bullet impacts, explosion scorch)
- **Pooled**: Frequently spawned/despawned decals use a ring-buffer pool

```cpp
struct DecalSpawnRequest {
    DestructionEventOccurrenceId sourceOccurrence;
    DestructionEventBindingTableGeneration bindingGeneration;
    DestructionEventDestinationId destination;
    uint32_t layerOrdinal;
    AssetId          decalMaterial;
    WorldTransform   transform;
    Vector3          extents;
    float            lifetime;           // 0 = permanent
    float            fadeOutDuration;
    DecalSpawnFlags  flags;
};
```

Pooled decals use a fixed-size ring buffer per decal type. When the buffer is
full, the oldest decal is recycled.

## Destruction Event Integration

[ADR-147](../../adr/147-destruction-event-and-cosmetic-consumer-ownership.md)
keeps destruction semantics outside the Decal system. DFR publishes a bounded committed
fact without decal assets or handles. The application-owned dispatcher resolves a
cooked mapping and submits a typed `DecalSpawnRequest` at the Decal/VFX owner boundary.
The request retains the DFR occurrence, binding generation, destination and layer ordinal
as its dedup identity.

The Decal/VFX owner validates exact surface evidence, projection/attachment policy,
material readiness, lifetime and pool capacity. It owns render descriptors and resource
retirement; DFR never polls a decal or keeps a canonical chunk alive through a cosmetic
pointer. Missing surface/material, stale attachment and capacity pressure return typed
consumer outcomes and cannot roll back the destruction revision. Any oldest-entry reuse
is limited to a declared cosmetic policy with stable eligibility/ties; permanent,
event-driven or required work is never implicitly recycled.

## Decal Atlas

For performance, many small decals can be batched into a decal atlas:

- Multiple decal textures are packed into a single atlas
- Atlas is generated at asset cook time
- Decal shader uses atlas UV transform to sample the correct sub-region
- Reduces draw calls and shader switches for decal-heavy scenes

## Performance

Deferred decals are volume-bounded projected draws that read depth/GBuffer data only for affected pixels. Optional full-screen resolve/composite passes are separate budgeted render-graph passes. Optimizations:

- Decal count budget per view (default 256)
- Distance-based culling (decal fade distance)
- Frustum culling against the projection box
- Decal clustering (merge overlapping decals of the same material)
- Atlas batching for small decals

## Editor Authoring

Decal placement tools in the editor:

- Decal brush (paint decals onto surfaces at cursor position)
- Decal projection gizmo (position, rotate, scale the projection box)
- Decal material picker with preview
- Decal list panel showing all decals in the scene
- Decal count and memory budget visualization

## Feature Tiers

| Feature              | `es3`      | `dx11` / `dx12_vulkan` | `high_end` |
| -------------------- | ----------- | ------------- | ------------ |
| Deferred decals      | No          | 256           | 1K           |
| Forward/mesh fallback| 64          | Yes           | Yes          |
| Decal atlas          | No          | Yes           | Yes          |
| Normal blending      | No          | Yes           | Yes          |
| Decal pooling        | Yes         | Yes           | Yes          |

## Related Documents

- [Decal Placement UI Reference](../../../mock-studio/designs.md#architecture-runtime-decal-placement)

- [Rendering Architecture](./rendering-architecture.md): GBuffer and decal render pass
- [Material And Shader Model](./material-and-shader-model.md): decal material domain
- [VFX And Particles Architecture](./vfx-and-particles-architecture.md): VFX-driven decal spawning
- [Scene Runtime](./scene-runtime.md): decal component and placement
- [ADR-127: VFX Decal Projection, Lifetime and Rendering Path](../../adr/127-vfx-decal-projection-lifetime-and-rendering-path-policy.md)
- [ADR-147: Destruction Event and Cosmetic Consumer Ownership](../../adr/147-destruction-event-and-cosmetic-consumer-ownership.md)
