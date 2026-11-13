# Editor clustered lighting

HORO-382 / #382 [RND-012.3] adds a bounded three-dimensional light grid,
explicit lighting selection and a Metal graph implementation. Admitted Metal
devices advertise `supportsMaterialRasterGraph`; selecting native lighting also
requires the complete, valid cooked shader family and kernel.

## Source table and selection

`RenderSceneView` accepts 4096 lights. Sixteen is the per-draw forward limit,
not a scene-wide truncation threshold. Scene extraction supplies stable entity
identities, an actual scene domain and a content revision. Preview light and
transform edits compose in reusable staging banks and publish atomically.

OpenGL and Vulkan consume frontend-selected CPU forward lighting. Each draw
selects from the full source table using its world bounds. Complete coverage
rejects insufficient capacity; explicitly allowed omissions are counted. The
shadow light is remapped by identity into each selected draw table.

The editor accepts these independent startup choices:

```text
--lighting forward|clustered
--lighting-fallback forbid|forward
--lighting-coverage complete|allow-omissions
```

Defaults request clustered lighting with explicit forward fallback and complete
coverage. Unsupported native capabilities cannot silently select another backend.
Strict native selection fails when its actual prerequisites are absent. CPU work
has a finite per-view budget; readiness errors defer admission before frame start.
Mesh admission checks both the actual ready state and source generation.

## Native ownership and graph

The default grid is 8 by 4 by 8, with 4096 reference slots per cluster. The
canonical grid maps top-left pixels and device depth to true view-space clusters.
Its view, source table, projection and grid identities must match the consuming
camera and shader parameters. Fragment slice selection uses actual interpolated
geometry depth, avoiding depth-buffer quantization at large near/far ratios.
`MapClusteredLightViewDepth` exposes the matching geometric-depth mapping; the
existing device-depth mapper retains the semantics of its quantized input.

The Metal path owns cooked programs, a cooked culling kernel, finite frame slots
and their resource leases. Its same-command graph orders camera depth, light
culling, shadow atlas, lit geometry, overlays and primary output. Read-only
storage bindings and material geometry are retained through actual command
completion. Stable topology reuses prepared slots; no runtime shader compilation
is substituted for missing artifacts.

Diagnostics distinguish pending GPU evidence from a completed uploaded table
revision. Encoded work alone cannot establish membership or overflow totals.

Scene `Mat4` storage is column-major. The editor converts it to the shared
material writer's row-major component convention before packing reflected
matrix and array strides. Metal validates the resulting uniform layouts against
actual driver reflection, including the required float4x4 and float4 arrays.

## Cold assets and hot reload

The host composition root owns `HoroEditorClusteredLightingAssetsInternal` and
`HoroEditorViewportLightingInternal`. This adds only explicit application-to-editor
module edges; scene extraction continues to depend on renderer-neutral APIs.

The loader expects these four shader package/reflection pairs relative to the asset root:

- `shaders/editor/viewport-clustered-metal`
- `shaders/editor/viewport-grid-metal`
- `shaders/editor/viewport-shadow-metal`
- `shaders/runtime/light-culling-metal`

Each pair uses `.horoshdr` and `.reflection.json`. All absent means no packaged
native recipe. A partial bundle or invalid file is an error. Presence alone does
not validate a package; decoding, native reflection and kernel admission still
run. Canonical Metal hot-reload families follow the actually admitted lighting
mode while preserving the caller's tool identities, limits and lifecycle fences.
Custom recipes remain caller-owned.

## Integration and qualification

Rebuild C++ consumers of the changed render scene, backend capabilities and graph
workload headers. Supply real light provenance and view identity at host
composition. Call lighting preparation before `BeginFrame`; do not synthesize
ready mesh state from allocated handles or use a frame ordinal as content revision.

A trusted native Apple Metal cooking configuration is not provisioned by this
change. Finding an installed compiler does not approve its digest. Parser fixtures
and local library builds do not prove native shader production or GPU execution.
The configured tests require genuine cooked inputs and explicitly skip when the
asset root is absent. With a complete approved-tool bundle, run both the private
native qualification and the actual frontend path:

```sh
export HORO_CLUSTERED_LIGHTING_ASSET_ROOT=/absolute/cooked-package-root
export MTL_DEBUG_LAYER=1
build/tests/HoroMetalGraphExecutionSmoke \
  'Configured Metal private runtime qualifies actual clustered compute to material raster before advertisement'
build/tests/HoroEditorViewportMetalSmoke \
  'Configured cooked Metal viewport consumes real clustered storage through one frontend graph'
```

Use the test executable paths from your configured build directory. Qualification
requires executed cases with no failures or skips: native completion and
membership readback, spatial cluster selection, more-than-sixteen-light pixel
evidence, pending/abort ownership, frontend frame submission, family replacement,
and bounded drain. These checks have separate roles; parser fixtures and an
encoded command buffer cannot substitute for either runtime path.

Vulkan native compilation/runtime, full editor visual validation and performance
acceptance must be reported separately. Existing unlit XR rendering is not
clustered-lighting parity. Exposure/ACES and broader material work remain outside
this ticket. Do not close HORO-382 based solely on CPU tests or a skipped GPU smoke.
