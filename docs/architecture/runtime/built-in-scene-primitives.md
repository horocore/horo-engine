# Built-In Scene Primitives

## Purpose

This document defines the built-in scene primitives that Horo Engine guarantees
without external packages. A *scene primitive* is a first-class authoring and
runtime object category that can be created directly in the editor, CLI, and
MCP, serialized in scene documents, and resolved to runtime data without an
imported source asset.

Primitives are distinct from imported assets. Imported assets flow through the
[Asset Pipeline](./asset-pipeline.md) and are referenced by `AssetId`. Built-in
primitives are described by a typed descriptor and resolved procedurally by the
engine.

## Scope

Built-in scene primitives cover three concerns:

1. **Procedural mesh primitives** — engine-generated geometry for rapid
   prototyping and editor gizmos.
2. **Collider shape primitives** — physics shapes that can be assigned to
   objects by default or explicitly.
3. **Scene object primitives** — logical objects such as cameras, lights, and
   volumes that are not primarily defined by a mesh asset.

This document does **not** define:

- UI primitive components (see [UI Design System](../editor/ui-design-system.md)).
- Concurrency primitives (see [Concurrency And Job System](../foundation/concurrency-and-jobs.md)).
- Package primitives (see [Package System](../packages/package-system.md)).

## Primitive Catalog

The `PrimitiveCatalog` is the single authoritative registry of built-in
primitives. All editor menus, inspector enum options, scene serialization
validation, CLI commands, MCP tool schemas, and tests consume the same catalog.

```cpp
struct PrimitiveDescriptor {
    PrimitiveId id;              // stable identifier, e.g. "primitive.mesh.capsule"
    PrimitiveCategory category;  // Mesh, Collider, SceneObject
    std::string displayName;     // user-facing label
    std::string iconToken;       // optional design-system icon name
    bool isRenderable;
    bool isPhysicsSolidByDefault;
    std::optional<PrimitiveMeshType> meshType;
    std::optional<ColliderShapeType> defaultCollider;
};
```

Primitive IDs are stable across releases. They are **not** asset GUIDs and do
not depend on project-local files. Migration is required if a primitive ID is
renamed or removed.

## Core Mesh Primitives

The engine provides the following procedural mesh primitives:

| ID | Display name | Notes |
|---|---|---|
| `primitive.mesh.box` | Cube / Box | Axis-aligned unit box, centered at origin. |
| `primitive.mesh.sphere` | Sphere | UV sphere with configurable stacks/slices. |
| `primitive.mesh.capsule` | Capsule | True capsule: cylindrical body with hemispherical caps. |
| `primitive.mesh.cylinder` | Cylinder | Right circular cylinder. |
| `primitive.mesh.cone` | Cone | Right circular cone. |
| `primitive.mesh.plane` | Plane | Large single quad, typically used as a floor. |
| `primitive.mesh.quad` | Quad | Two-triangle unit quad, useful for billboards and UI-in-world. |

Mesh primitives carry:

- position, normal, and UV vertex data
- index buffer
- local-space AABB
- optional tangent/bitangent data when the renderer requires it
- optional configurable tessellation parameters

Procedural mesh data is **not** stored as a project asset. The scene document
stores a `PrimitiveMeshDescriptor`:

```cpp
struct PrimitiveMeshDescriptor {
    PrimitiveMeshType type;
    PrimitiveMeshVersion version;        // generation schema version for migration
    PrimitiveMeshParameters parameters;  // typed variant matching type
};
```

Version one uses these exact parameter payloads and defaults:

| Type | Parameters | Defaults |
|---|---|---|
| Box | `Vec3 size` | `(1, 1, 1)` |
| Sphere | `radius`, `slices`, `stacks` | `0.5`, `32`, `16` |
| Capsule | `radius`, `totalHeight`, `radialSegments`, `hemisphereRings` | `0.5`, `2`, `32`, `8` |
| Cylinder | `radius`, `height`, `radialSegments` | `0.5`, `1`, `32` |
| Cone | `radius`, `height`, `radialSegments` | `0.5`, `1`, `32` |
| Plane | `Vec2 size` | `(10, 10)` |
| Quad | `Vec2 size` | `(1, 1)` |

The generator uses right-handed Y-up scene space and outward CCW triangle
winding. Plane lies on XZ and faces `+Y`; Quad lies on XY and faces `+Z`.
Sphere, capsule, cylinder, box, plane, and quad use a centered pivot. Cone uses
the documented base-center pivot and spans `y=0..height`.

`PrimitiveMeshGenerator::Generate` validates the complete type/version/parameter
combination and returns `Result<MeshData>`. Its stable error domain is
`horo.runtime.scene.primitives`; invalid dimensions, invalid tessellation,
type/parameter mismatch, unsupported version, and capacity overflow remain
distinct machine-readable failures. It never substitutes a Box or silently
skips a descriptor.

At cook time, the descriptor may be baked into a runtime mesh asset if the
release pipeline chooses to pre-generate geometry. At edit time, the mesh is
generated on demand and cached by `PrimitiveMeshCache`.

`PrimitiveMeshCache` keys the complete descriptor, including version and every
parameter. It is bounded by item and CPU-byte budgets. A cache lease pins its
immutable `MeshData` against eviction; unleased least-recently-used entries may
be evicted. Concurrent requests are serialized through the cache boundary, so
one key cannot publish duplicate resources. A hit performs no mesh generation
or new geometry allocation.

## Core Collider Shape Primitives

The physics module supports at least the following collider shapes as core
primitives:

| ID | Display name | Notes |
|---|---|---|
| `primitive.collider.box` | BoxCollider | Axis-aligned box in the collider's local space. |
| `primitive.collider.sphere` | SphereCollider | Uniform sphere. |
| `primitive.collider.capsule` | CapsuleCollider | Capsule aligned to the local Y axis by default. |
| `primitive.collider.static_plane` | StaticPlaneCollider | Infinite plane; immovable and one-sided. |

Collider primitives are used for:

- default collision geometry assigned when a mesh primitive is created
- explicit collider components attached to objects with custom or imported meshes
- trigger volumes
- editor-only raycast and placement helpers

Collider primitives are **not** renderable unless the editor chooses to draw a
wireframe debug visualization.

### Default Collider Policy

When a primitive object is created, the editor applies a conservative default
collider policy:

| Mesh primitive | Default collider | Solid by default |
|---|---|---|
| Box | `primitive.collider.box` | yes |
| Sphere | `primitive.collider.sphere` | yes |
| Capsule | `primitive.collider.capsule` | yes |
| Cylinder | none | no |
| Cone | none | no |
| Plane | `primitive.collider.static_plane` | no |
| Quad | none | no |

Cylinder, cone, and quad do not receive a default collider because no core
collider shape exactly matches their geometry. A project may later attach a
convex-hull, mesh, or custom collider when the physics backend supports it.
Plane receives a `StaticPlaneCollider` only when the creation context indicates
it is intended as a floor or immovable surface; otherwise it receives no
collider.

## Core Scene Object Primitives

The following logical objects are core scene primitives:

| ID | Display name | Notes |
|---|---|---|
| `primitive.object.empty` | Empty Object | Invisible node used for grouping and transforms. |
| `primitive.object.camera` | Camera | Perspective or orthographic view frustum. |
| `primitive.object.light_directional` | Directional Light | Infinite directional light source. |
| `primitive.object.light_point` | Point Light | Omni-directional point light. |
| `primitive.object.light_spot` | Spot Light | Cone-shaped spot light. |
| `primitive.object.trigger_volume` | Trigger Volume | Collision volume that emits overlap events. |
| `primitive.object.audio_source` | Audio Source | Point/ambient audio emitter placeholder. |

These objects are not backed by procedural mesh descriptors. They are backed by
typed scene components such as `CameraComponent`, `LightComponent`,
`TriggerVolumeComponent`, and `AudioSourceComponent`.

Later engine releases or official packages may add:

- `primitive.object.light_rect` — Rect Light
- `primitive.object.light_sky` — Sky Light
- `primitive.object.terrain` — Terrain (requires terrain system)
- `primitive.object.foliage` — Foliage patch (requires foliage system)
- `primitive.object.text_3d` — 3D text (requires text renderer)

These are **not** guaranteed core primitives; they are candidate features that
must each go through the package or feature approval process.

## Default Material And Shader Policy

Every renderable mesh primitive is extracted with the typed binding
`core.materials.default`. The primitive catalog does not store shader or
material state; it only identifies the mesh. Until the general material system
is connected, editor viewport adapters resolve this binding through their
narrow neutral preview shader. This seam must not grow into a second material
system.

```text
Default material:
  - shader: core.shaders.standard
  - albedo: medium gray or a neutral tone
  - roughness: 0.5
  - metallic: 0.0
  - normal map: none
```

A primitive mesh instance is rendered as `MeshResourceId` + material binding +
transform. Extraction emits a deduplicated immutable resource table, and draw
instances reference that table only by mesh ID. OpenGL and Metal adapters upload
generic position/normal/UV plus `uint32_t` index data and do not branch on
`PrimitiveMeshType`. Imported meshes will become a second producer of this same
contract when the asset pipeline is implemented.

Primitive meshes provide the current required layout: position, normal, and UV.
Tangents and bitangents are deliberately deferred until the material/normal-map
pipeline exists. At that boundary they must be generated or the material system
must report a typed layout validation error.

## Default Transform Conventions

Primitive meshes are generated in local space with deterministic dimensions so
that "Create > Box" and "Create > Sphere" produce comparably sized objects.

| Primitive | Default dimensions | Pivot |
|---|---|---|
| Box | 1 x 1 x 1 | center |
| Sphere | radius 0.5 (diameter 1) | center |
| Capsule | radius 0.5, total height 2 | center |
| Cylinder | radius 0.5, height 1 | center |
| Cone | radius 0.5, height 1 | base center |
| Plane | 10 x 10 | center |
| Quad | 1 x 1 | center |

These conventions are part of the primitive contract. The authoring transform
component still allows arbitrary scaling.

## Editor Integration

### Hierarchy Menu

The editor hierarchy context menu and the application `Game Object -> Create`
menu are generated from the same `PrimitiveCatalog` metadata and dispatch the
same `CreateSceneObjectUseCase`. They must not hardcode primitive names or
membership independently of the catalog.

`PrimitiveCategory` retains runtime meaning (`Mesh`, `Collider`, or
`SceneObject`). The separate typed `PrimitiveCreationGroup` controls creation
presentation without weakening that runtime classification:

```text
Create
  Empty Object
  3D Objects
    Box, Sphere, Capsule, Cylinder, Cone, Plane, Quad
  Cameras
    Camera
  Lights
    Directional Light, Point Light, Spot Light
  Volumes
    Trigger Volume
  Audio
    Audio Source
```

Right-clicking a hierarchy row creates below that row. The empty hierarchy area
creates at the document root. The application menu creates below the current
selection, or at the root when there is no selection. A successful operation is
one undoable document command and selects the created object. Collider-only
descriptors use `NotCreatable` and do not appear in either menu.

Environment, text, terrain, ragdoll, and similar higher-level features must not
appear in the primitive menu unless they are registered as core primitive
descriptors.

### Inspector

The inspector's mesh and collider enum options are populated from the catalog.
When a user changes a prop's mesh from "box" to "capsule", the runtime
conversion, default collider suggestion, and placement bounds all update from
the same descriptor.

### Asset Identity

Primitive meshes are **not** assigned a project `AssetId`. They are referenced
by `PrimitiveMeshDescriptor`. Imported meshes continue to use `AssetId` and the
asset pipeline.

This distinction is visible in serialization:

```json
{
  "kind": "primitive_mesh",
  "type": "capsule",
  "parameters": { "radius": 0.5, "halfHeight": 1.0 }
}
```

versus

```json
{
  "kind": "imported_mesh",
  "assetId": "a1b2c3d4..."
}
```

## Runtime Conversion

The implemented editor extraction path resolves a primitive mesh descriptor to
a cached generic `MeshData` lease. A future `BuildRuntimeSceneDefinition` will
use the same cache contract for gameplay runtime conversion. Physics shapes and
imported meshes are not implemented by this slice. Process-local mesh resource
IDs never enter the scene document; versioned descriptors remain the authored
source of truth.

[ADR-085](../../adr/085-physics-shape-authoring-cook-and-runtime-boundary.md)
maps the built-in cube, sphere, capsule and plane collider descriptors to Physics'
analytic box, sphere, capsule and static-plane source kinds. These descriptors use
the same validation, scale-bake, cook-envelope and immutable runtime-lease boundary
as imported collider assets; the catalog does not bypass Physics shape policy.

When an authoring command adds one of these colliders, it records the project's
explicit default `CollisionProfileId` (or the command's explicit profile) under
[ADR-086](../../adr/086-collision-layer-profile-and-query-channel-policy.md).
Runtime conversion never resolves a missing profile by primitive kind, display
name, array index or fallback mask.

Under [ADR-087](../../adr/087-scene-to-physics-ownership-and-conversion.md), a
primitive mesh descriptor alone remains visual data and creates no body. An
authoring action that offers a collider convenience persists an explicit rigid-
body + collider component bundle with stable slots in one document transaction.
Runtime conversion never repeats that inference or publishes scene entities before
the required Physics candidate is ready.

## CLI And MCP Integration

CLI and MCP creation commands do not hardcode object types. They accept a
`primitiveId` and validate it against the `PrimitiveCatalog`:

```json
{
  "tool": "create_object",
  "arguments": {
    "name": "PlayerSpawn",
    "primitiveId": "primitive.object.empty"
  }
}
```

Unknown primitive IDs fail with a typed error. The MCP tool schema is generated
from the catalog so that clients receive an accurate enum without manual
maintenance.

Scene creation commands use the shared `CreateSceneObjectUseCase`. GUI, CLI, and
MCP adapters invoke this use case; they do not implement separate creation logic.
The use case accepts a `PrimitiveCreationRequest`, validates it against the
catalog, applies the default material and collider policy, and commits a typed
editor command or runtime spawn operation.

## Headless And Test Support

The catalog and procedural mesh generators are available in headless builds and
tests. A test may create a scene containing only primitive descriptors and
assert on deterministic runtime output without importing external assets.

```cpp
PrimitiveMeshDescriptor desc{
    .type = PrimitiveMeshType::Capsule,
    .parameters = {.radius = 0.5f, .halfHeight = 1.0f}
};
Mesh mesh = PrimitiveMeshCache::Generate(desc);
```

## Core Vs Package Boundary

The following primitives are **engine core** and must be available in every
Horo project without packages:

- All core mesh primitives listed above.
- All core collider shape primitives listed above.
- Empty, Camera, Directional/Point/Spot Light, Trigger Volume, Audio Source.

The following may be provided by **official packages** or future engine
features, but are not required for a minimal engine:

- Rect Light, Sky Light
- Terrain, Tree/Foliage, Wind Zone
- 3D Text
- Ragdoll
- Torus, Icosphere, Disc, Pyramid
- Convex Hull, Triangle Mesh, Heightfield colliders
- Advanced procedural shapes

Packages that add new primitive types must register descriptors through the
same `PrimitiveCatalog` extension point and must not reuse reserved core
primitive IDs.

## Versioning And Migration

A `PrimitiveMeshVersion` field in descriptors allows the engine to migrate old
parameter layouts. If a primitive is removed, the loader must preserve the
unknown descriptor and emit a diagnostic rather than silently replace it with a
different shape.

## Related Documents

- [Scene Primitives UI Reference](../../../mock-studio/designs.md#architecture-runtime-primitives-panel): primitive placement, snap settings, collider generation, and default materials panel.

- [Asset Pipeline](./asset-pipeline.md): imported asset flow, `AssetId`, and
  cook pipeline.
- [Rendering Architecture](./rendering-architecture.md): render extraction and
  backend-neutral rendering of generated and imported meshes.
- [Physics Architecture](./physics-architecture.md): collider shapes, physics
  world, and determinism.
- [Scene Runtime](./scene-runtime.md): runtime scene definitions and ECS
  conversion.
- [Editor Document Model](../editor/editor-document-model.md): scene document
  serialization and transactions.
- [MCP Architecture](../interfaces/mcp-architecture.md): tool schema generation
  and validation.
- [Desired Project Trees](../desired-project-tree.md): repository layout,
  primitive ownership, and dependency direction.
