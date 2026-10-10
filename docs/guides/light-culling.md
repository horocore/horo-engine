# Bounded light extraction and culling

The scene adapter belongs to `HoroEngine::SceneRenderExtraction`, which depends
on `RuntimeScene` and `RenderApi`. Scene code does not depend on a renderer.
Consumers previously extracting authored lights themselves can migrate to
`ExtractSceneLights`, retain the returned scene/revision provenance, and use only
the returned prefix of their caller-owned scratch table. Extraction is synchronous
on the scene owner thread. It rejects oversized tables and malformed lights,
preserves typed scene/math errors, and never silently truncates.

`RenderApi` owns the light membership, packed-frame, upload and cooked-kernel
contracts. `RenderFrontend` owns the finite buffer pool. The public-header
ownership registry assigns every added header to exactly one target; consumers
must declare the owning target instead of adding repository-wide include roots.
The existing `RenderSceneView` forward-light limit remains sixteen.
`PrepareForwardLights` produces that bounded span using the same conservative
membership rule. Required complete coverage fails when capacity is insufficient;
explicitly optional coverage reports omitted lights in stable identity order.

Point and spot lights use their world-space range spheres; spot cones may retain
false positives. Directional lights affect every cluster. Zero intensity or zero
punctual range contributes nothing. The caller supplies normalized inward planes
for its admitted three-dimensional clustered grid. This change does not invent a
two-dimensional tiled renderer or select a rendering profile.

For an explicitly admitted native path:

1. Cook `assets/shaders/runtime/light_culling.hlsl` through the existing locked
   HLSL 2021 pipeline. Preserve its HOROSHDR v1 package and final-target reflected
   binding map. Metal receives an offline Metal library, never runtime MSL source.
2. At preparation, realize the cooked kernel and allocate a `LightFrameBufferPool`
   with a finite product budget and frame-slot count. The narrow `LightCulling`
   capability grants this operation; it does not grant general compute support.
3. Extract, pack and update a caller-selected slot before opening a frame. Ready
   resource generations and actual native command completion are required.
   Pending native use returns `LightCullingErrors::Pending` without changing bytes
   or consuming the next revision. The host chooses whether to defer a frame; no
   backend fallback or frame wait occurs.
4. Author exactly four distinct storage uses (two read inputs, two write outputs)
   and a `RenderGraphLightCulling` workload carrying the returned revision and
   dispatch. Native admission rechecks buffer namespace, revision, range and
   kernel incarnation before encoding. Sequential Metal graph execution supports
   this workload. Parallel recording, Null and OpenGL report typed unsupported
   results. Vulkan has no admitted native implementation here.
5. Quiesce graph users, end active frames, and close the pool on its creating
   render thread before destroying the frontend. Failed release retains remaining
   handles for retry. Native command pins retire independently of CPU pool life.
   Release preparation kernel leases on the render owner thread.

`HoroLightCullingTests` covers deterministic CPU membership, extraction, packed
ABI, cooked-envelope admission and graph contracts. `HoroRenderFrontendTests`
covers reusable-slot readiness, revision/backpressure and teardown. These tests
do not prove GPU execution. The opt-in `HoroMetalGraphExecutionSmoke` includes
actual native-completion upload checks and a cooked-kernel parity check.

The parity check requires `HORO_LIGHT_CULLING_PACKAGE` pointing to the locked
toolchain's HOROSHDR package and `HORO_LIGHT_CULLING_METAL_BINDINGS` containing
the reflected native buffer indices for lights, clusters, membership, references
and dispatch, in that order, separated by commas. Without both it explicitly
skips. It loads the supplied package, verifies native pipeline reflection, executes
on the selected device, waits only within the bounded test, and compares stable
membership, references and overflow with the CPU recipe. A skipped check is
missing qualification evidence, not a passing GPU result.
