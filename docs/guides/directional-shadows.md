# Directional and cascaded shadows

The editor's OpenGL, Metal and Vulkan viewport paths consume the same
`Horo::Render::DirectionalShadowProfile` and fixed-capacity
`DirectionalShadowFrame`. The canonical implementation fits up to four camera
frustum slices for the first positive-intensity directional light in the
submitted scene. Native atlas storage, pass execution and retirement remain
owned by the selected renderer.

## Select a quality profile

Pass one quality selection when starting HoroEditor. Both argument forms are
supported for each installed renderer:

```sh
HoroEditor --renderer opengl --shadow-quality high
HoroEditor --renderer metal --shadow-quality=ultra
HoroEditor --renderer vulkan --shadow-quality medium
```

The default is `high`. Values are case-sensitive: `off`, `low`, `medium`, `high`
and `ultra`. Missing, unknown or duplicate selections fail before native startup.
Selecting a renderer still requires its normal installation and availability
checks; selecting a shadow profile does not establish device support.

| Profile | Cascades | Allocated tile edge | Complete atlas | Depth comparison kernel |
| --- | ---: | ---: | --- | --- |
| `off` | None rendered | Retained settings | No active atlas extent | Disabled |
| `low` | 1 | 1024 px | 1024 × 1024 px | 1 × 1 |
| `medium` | 2 | 1024 px | 2048 × 1024 px | 3 × 3 PCF |
| `high` | 4 | 1024 px | 2048 × 2048 px | 3 × 3 PCF |
| `ultra` | 4 | 2048 px | 4096 × 4096 px | 5 × 5 PCF |

Each tile reserves a two-pixel guard on every edge. Its inner raster viewport
therefore has an edge of `tileResolution - 4`: 1020 pixels for a 1024-pixel tile,
or 2044 pixels for a 2048-pixel tile. One cascade uses one tile; two use a
horizontal pair; three or four use a two-by-two atlas. Receiver sampling uses
the prepared inner atlas rectangle and a filter radius no larger than two
atlas texels.

`off` keeps positive, valid count/resolution settings while disabling the pass
and receiver sampling. A native owner may retain its existing allocation while
disabled. A replacement profile becomes active with its complete admitted
native resources. Allocation or admission failure preserves the previous ready
profile and atlas; it does not publish a partially replaced family.

## Fitting, blending and units

`BuildDirectionalShadowFrame` accepts the actual render camera, viewport aspect,
clip-depth convention, scene casters and selected profile. It returns an owned
frame with no retained scene pointers and no frame-storage allocation on the
successful path. Disabled shadows, no positive directional light, or no caster
produce a successful empty optional.

The presets cover at most 100 meters, clamped to the camera far plane. Splits
use mixed linear/logarithmic partitioning with a logarithmic weight of 0.65.
The public API also accepts pure logarithmic splits, one through four cascades,
and a validated coverage distance. Split near/far values and blend starts are
positive camera-forward distances in scene meters, not Euclidean camera
distances or device depth values.

The default transition occupies the final 10% of a logical slice. The next
cascade's fit includes that preceding transition interval so both maps cover
the blend. Frustum receiver spheres use outward radius quantization; enabled
stabilization snaps their light-space centers to a texel grid. Conservative
transformed caster bounds extend the depth fit, and receiver normal bias
expands both receiver and depth coverage.

Fitting operates in camera-relative space. A frame carries `cameraOrigin` and
`cameraRelativeViewProjection` for localized model transforms, as well as a
world-input compatibility matrix in `viewProjection`. Consumers must use the
matching model coordinate space. The explicit depth convention is
`NegativeOneToOne` for OpenGL and `ZeroToOne` for Metal/Vulkan; unknown enum
values fail validation.

The preset bias settings are:

| Setting | Default | Units and interpretation |
| --- | ---: | --- |
| `depthBias` | 0.001 | Normalized shadow-depth comparison offset |
| `slopeBias` | 0.002 | Normalized depth comparison offset scaled by receiver slope |
| `normalBias` | 0.02 | Receiver normal offset in world meters, independent of resolution |
| `filterRadius` | 0, 1 or 2 | Atlas texels; kernel edge is `2 * radius + 1` |

The public profile validates depth/slope bias in `[0, 0.1]`, normal bias in
`[0, 10]` meters, blend fraction in `[0, 0.5]`, and mixed split weight in
`[0, 1]`. Tile edges must be powers of two from 64 through 2048. Coverage must
be finite and within `[0.01, 100000]` meters, and active coverage must extend
beyond the camera near plane.

## Cache and native pass ownership

The frame fingerprint includes actual camera/projection/aspect/depth settings,
the selected light, profile settings, caster identities, transforms and bounds.
It also hashes actual supplied mesh vertex values and indices within a four-MiB
geometry work cap. A mesh ID or generation alone is not accepted as evidence
that geometry is unchanged. Missing CPU geometry or geometry above that cap
sets `cacheable = false`, which conservatively requires rendering again.

OpenGL may reuse its retained atlas only after a complete actual native shadow
pass succeeded on the graphics owner. Its cache compares both the input
fingerprint and the exact atlas resource incarnation. Resource loss, replacement
or failed rendering invalidates reuse. `CommitDirectionalShadowRender` is a
post-pass acceptance operation; successful fitting or encoded commands alone
do not justify calling it. This owner-side completion policy does not require
waiting for GPU idle.

Metal and Vulkan currently redraw their active cascade family every frame.
Their encoded passes are not reported as reusable completed shadow-cache
generations. All paths retain the complete prior ready resource generation
through their existing lifetime/retirement authority when a profile replacement
fails or older submitted work still references it.

## Package the Vulkan lit and shadow artifacts together

Vulkan startup loads both offline-produced artifact pairs under the packaged
asset root:

```text
shaders/editor/viewport-vulkan.horoshdr
shaders/editor/viewport-vulkan.reflection.json
shaders/editor/viewport-shadow-vulkan.horoshdr
shaders/editor/viewport-shadow-vulkan.reflection.json
```

The lit source remains `assets/shaders/editor/viewport.hlsl`. The depth pass
reuses the canonical shared source
`assets/shaders/editor/viewport_shadow.hlsl`; it has no separately authored
Vulkan shadow source. On the admitted reviewed Ubuntu 26.04 x86-64 cook host,
invoke the existing cooker for both roles:

```sh
HoroEditorVulkanShaderCook assets/shaders/editor/viewport.hlsl \
  "$asset_root/shaders/editor/viewport-vulkan" "$scratch_root/lit" \
  "$dxc" "$spirv_val" "$spirv_cross"
HoroEditorVulkanShaderCook assets/shaders/editor/viewport_shadow.hlsl \
  "$asset_root/shaders/editor/viewport-shadow-vulkan" "$scratch_root/shadow" \
  "$dxc" "$spirv_val" "$spirv_cross" --shadow
```

The cooker validates the reviewed archive/executable identities and actual host
userspace. Its default viewport mode and explicit `--multiview` mode remain
available. Editor startup never invokes this compiler as a missing-asset
fallback. The workflow's bundle schema 2 verifies both payload/reflection pairs,
the exact tool catalog, and current source/run provenance before selecting the
bundle.

Development hot reload uses one complete Vulkan family with canonical role IDs
`{1: Lit, 3: Shadow}`. Vulkan has no Grid role in that family. Both roles retain
the finite 8-MiB payload/debug limits and pass through the existing asynchronous
compilation and owner-thread publication contract. OpenGL and Metal continue to
use their complete Lit/Grid/Shadow family.

## Qualification and rebuild boundary

The recorded macOS suite validation is 244 passing cases with 9161 assertions,
including the native CGL case below and startup-argument admission cases.
Actual Metal qualification has passed two cases and 115 assertions without
skips, exercising production MSL, reflected layout and heap/resource lifetime.
The actual OpenGL CGL shader case also passed 90 assertions without skips,
compiling the production shaders and checking driver uniforms. Native Vulkan execution, the
reviewed Linux compiler route, image comparisons and performance qualification
remain unrun locally. These results do not establish a full HoroEditor link or
an observed editor/viewport visual result.

This change grows the public C++ `RenderBackendCapabilities`, `MaterialPass`
and pipeline-state contracts. Rebuild all renderer consumers and modules from
the same matching source snapshot. Compatibility with older C++ renderer
binaries is not established. No gameplay SDK 25 or C extension ABI value change
was found; that does not remove the C++ renderer rebuild requirement.

See [advanced rendering](../architecture/runtime/advanced-rendering-architecture.md),
[scene math](../architecture/foundation/scene-math.md), and
[coordinate precision](../architecture/runtime/coordinate-precision-and-origin-rebasing.md)
for the owning architecture contracts.
