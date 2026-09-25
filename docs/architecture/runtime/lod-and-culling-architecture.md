# LOD And Culling Architecture

## Purpose

This document defines the level-of-detail (LOD) and visibility culling
subsystems for Horo Engine. It covers mesh LOD, HLOD, impostors, frustum
culling, occlusion culling, distance culling, and GPU-driven culling
pipelines.

## Mesh LOD

### LOD Chain

Each renderable mesh defines a chain of LOD levels:

```cpp
struct MeshLODChain {
    uint32_t              lodCount;
    MeshLODLevel          levels[MAX_LOD_COUNT];
    LODSelectionMode      selectionMode;
    float                 lodBias;          // global quality bias
};

struct MeshLODLevel {
    AssetId               meshId;           // simplified mesh asset
    float                 screenSize;       // fraction of viewport height
    float                 transitionDuration; // cross-fade time
};
```

LOD selection uses screen-space size relative to viewport height:

- `screenSize = (boundingSphere.radius / distance) * (viewportHeight / tan(fov/2))`
- Select the first LOD where `screenSize >= level.screenSize`
- Apply `lodBias` to shift selection (negative = more detail, positive = less)

### Smooth LOD Transition

Transitions between LOD levels use dithered cross-fade:

- During the transition window, both LOD levels are rendered
- A screen-door dither pattern determines which level contributes per pixel
- The dither pattern is temporal (varies each frame) to avoid static noise
- Cross-fade is GPU-driven via a global dither pattern texture

### LOD Generation

LOD levels are generated at asset import time:

- Mesh simplification uses quadric error metrics (QEM)
- Target triangle count is configurable per LOD level
- Generated LOD meshes are stored as derived assets alongside the source mesh
- Skeleton and skinning data are preserved for skinned mesh LOD

```cpp
struct MeshLODGenerationSettings {
    float  targetTriangleRatio[MAX_LOD_COUNT];  // e.g., 0.5, 0.25, 0.1
    bool   preserveUVBoundaries;
    bool   preserveHardEdges;
    bool   generateImpostors;
};
```

## Hierarchical LOD (HLOD)

HLOD groups nearby static meshes into cluster proxies:

- Static meshes within the same spatial cluster are merged
- Cluster proxy is a simplified combined mesh with merged materials
- HLOD clusters are generated offline (baked into level data)
- HLOD transitions are distance-based, not screen-size

```cpp
struct HLODCluster {
    BoundingBox          bounds;
    AssetId              proxyMeshId;
    float                transitionDistance;
    std::vector<EntityId> sourceEntities;  // entities replaced by this proxy
};
```

HLOD clusters are organized in a spatial hierarchy (octree) for efficient
activation. Editor tools allow manual adjustment of cluster boundaries and
proxy quality.

## Impostors

Billboard impostors capture a mesh from multiple viewing angles:

- Impostors are pre-rendered at baking time from 8-32 viewpoints
- Stored as a texture atlas with depth for parallax correction
- Used as the farthest LOD for foliage and distant static meshes
- Impostor rendering is a single quad draw with atlas lookup

```cpp
struct ImpostorAsset {
    AssetId    atlasTexture;
    AssetId    depthTexture;
    uint32_t   viewCount;         // horizontal × vertical viewpoints
    float      objectRadius;      // for screen-size calculation
};
```

## Frustum Culling

Frustum culling eliminates objects outside the camera frustum:

- CPU culling: bounding sphere/frustum test on the job system
- GPU culling: compute shader tests bounding spheres in parallel
- Results feed into indirect draw buffers

```cpp
struct CullingFrustum {
    Plane    planes[6];
    float    nearZ;
    float    farZ;
};
```

GPU frustum culling is admitted only when the selected recipe, effective
compute/storage/indirect capabilities, bounded GPU Scene contract and cooked
variants all pass. CPU culling is the declared baseline fallback; API name alone
does not select either path.

## Occlusion Culling

### Software Occlusion

Software occlusion uses a hierarchical Z-buffer (Hi-Z):

- Previous frame's depth buffer is downsampled to a mip chain
- Object bounding boxes are tested against the Hi-Z pyramid
- Objects fully occluded are skipped in the draw list
- Hardware occlusion queries are used as a fallback on older APIs

### Portal / Precomputed Visibility

For indoor scenes, portal-based occlusion is available:

- Precomputed visibility sets per spatial cell
- Portals (doorways, windows) connect cells
- Only cells visible through portals from the camera cell are rendered
- Visibility is computed offline and stored as a bit array per cell

```cpp
struct VisibilityCell {
    BoundingBox      bounds;
    std::vector<VisibilityPortal> portals;
    std::bitset<MAX_CELLS> visibleCells;   // precomputed at bake time
};
```

## Distance Culling

Objects beyond a maximum draw distance are culled regardless of visibility:

```cpp
struct DistanceCullingSettings {
    float  maxDrawDistance;        // absolute max distance
    float  perObjectMultiplier;    // scale per object (e.g., large objects visible farther)
    bool   cullShadowsBeyond;      // also cull shadow casters beyond distance
};
```

Per-object culling distances are configured on the renderable component.
Distance culling is integrated with the GPU culling compute pass.

## GPU-Driven Culling Pipeline

GPU-driven culling consumes an immutable published GPU Scene generation from
[ADR-038](../../adr/038-gpu-scene-and-instance-data-model.md), plus a generation-
checked view descriptor, Hi-Z/history inputs when admitted and finite output
capacities:

```text
1. Compute shader: frustum + occlusion + distance culling
2. Compact visible instance list
3. Write indirect draw commands to GPU buffer
4. Execute indirect draws (no CPU readback)
```

Per-view visibility, selected LOD, compaction and generated commands live in
view-owned work buffers; they never mutate persistent base instance records.
Outputs carry their source GPU Scene/view/configuration generations and are
rejected on mismatch before native execution. Counter, visible-index and argument
overflow follow one admitted bounded policy and never write beyond capacity.

The render graph submits backend-neutral generated-draw batches. Backends translate
validated batches to private native indirect execution. Culling parameters
(frustum, origin-relative camera, Hi-Z and policy) are uploaded per view. No CPU
readback is required for normal drawing, and optional diagnostics are asynchronous,
bounded and generation tagged. GPU visibility is presentation data, not gameplay
or streaming truth.

## Terrain And Foliage Boundary

[ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md)
separates Terrain representation authority from renderer selection. Terrain Cook/Runtime
owns the finite legal tile/foliage LOD set, geometric-error inputs, seam/skirt/morph and
neighbor compatibility, material/hole preservation, resident availability and exact
content generation. The Terrain extractor publishes those facts in a bounded immutable,
view-independent candidate snapshot.

RenderFrontend owns selection per view because projection, output extent, raster recipe,
history/hysteresis and render budgets are presentation inputs. It may select only a
resident compatible representation and must satisfy adjacent-tile seam constraints. No
compatible required selection yields explicit preparing/unavailable/failure state rather
than a crack, flat substitute, silent omission or independent neighbor clamp.

Core 1.0 performs deterministic bounded CPU frustum/distance/LOD/seam planning and emits
neutral direct or instanced batches. The GPU-driven pipeline above is an optional post-1.0
Terrain/Foliage recipe requiring separately cooked shaders, complete effective compute/
storage/indirect capability, finite work buffers and an admitted fallback. GPU-selected
visibility/LOD remains per-view presentation data and cannot synchronously mutate World
Streaming residency or become gameplay truth.

## Debug And Visualization

- LOD visualization overlay (color by LOD level)
- Frustum culling visualization (show culling frustum)
- Occlusion culling visualization (show Hi-Z pyramid, occluded objects in
  wireframe)
- Draw call statistics per culling category
- GPU timing for culling compute passes

## Product Profile Policy

ADR-028 product profiles express preferences, not API tiers or fixed LOD counts.
Mesh/HLOD levels, transition policy, maximum candidate/visible instances, Hi-Z
resolution/history, compaction/argument capacity, CPU/GPU work and memory are
finite typed product/cook settings bounded by effective limits. Each GPU recipe
declares compute, storage, indirect, format, shader and GPU Scene requirements.

Baseline uses bounded CPU culling and direct/instanced draws. Higher profiles may
prefer GPU frustum/distance/occlusion and generated draws when the complete recipe
passes. Missing optional requirements selects only a declared CPU/lower-quality
fallback and records it; required GPU-driven content fails admission. No path
switches backend, silently drops candidates or changes gameplay visibility.

## Related Documents

- [LOD Debugger UI Reference](../../../mock-studio/designs.md#architecture-runtime-lod-debugger)

- [Rendering Architecture](./rendering-architecture.md): draw call submission and indirect draws
- [Terrain And Foliage Architecture](./terrain-and-foliage-architecture.md): foliage LOD and impostors
- [ADR-139](../../adr/139-terrain-render-extraction-material-lod-and-tier-boundary.md): Terrain/Foliage representation, per-view selection and CPU/GPU recipe ownership
- [World Streaming Architecture](./world-streaming-architecture.md): visibility cell streaming
- [Advanced Rendering Architecture](./advanced-rendering-architecture.md): ray-traced occlusion
- [Scene Runtime](./scene-runtime.md): renderable component and bounding data
