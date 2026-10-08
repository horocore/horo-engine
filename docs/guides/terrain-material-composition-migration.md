# Terrain material composition migration

TRF-003.2 adds `HoroEngine::TerrainRender` as a narrow, non-native material integration
target. Its public header is `Horo/TerrainRender/TerrainMaterialBinding.h`; semantic
layer/weight consumers include `Horo/Terrain/TerrainMaterial.h` from TerrainApi.
The public-header ownership registry and dedicated consumer test enforce these
boundaries. Existing TerrainApi, TerrainRuntime and RenderApi callers need no source
migration, and the canonical terrain source/cook wire format is unchanged.

Source import now delegates weight normalization to the shared TerrainApi operation.
It preserves the previous largest-remainder UNORM16 values and source-domain invalid
sample error. Cooked source consumers continue reading the same ordered uint16 samples.

An adopting application first validates the exact layer set against its captured
configuration. It resolves each semantic material-asset ID through its existing
material authority, retaining the selected resident pipeline/texture generation and
normalized final-target reflection. It supplies the terrain shader's explicit finite
model, exact permutation request, cooked artifact key/target and ready renderer pipeline.
No runtime source compilation, backend discovery or service registration occurs.

At the render safe point, publish against the current context and expected publication
number. Keep the returned immutable snapshot and the corresponding renderer/artifact
leases through every queued frame that references them. Prepare replacements against
new evidence; a failure leaves the prior publication untouched. Close the material
owner before host teardown and retire GPU leases through the renderer's ordinary
frames-in-flight lifetime protocol. Calling shutdown only releases this owner's CPU
reference and cannot prove GPU completion.

Blend samples use linear colors/radiance and unit normals in one tangent frame, after
sampling each layer with its authored UV scale. Validate the canonical weights and
legal tile mask before evaluation. Do not turn invalid weights, missing permutations
or an excess layer count into an unrelated material or implicit alternate.

This target supplies material preparation/publication for host composition. Actual
Terrain tile extraction, GPU upload and draw submission remain the TRF-003.3 consumer.
