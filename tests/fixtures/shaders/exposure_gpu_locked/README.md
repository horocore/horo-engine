# Exposure GPU reflection capture

These are the unmodified outputs of the pinned strict DXC, SPIRV-Val and SPIRV-Cross route for the three canonical entries in `assets/shaders/runtime/exposure.hlsl`. The portable provenance records the source hash, tool releases and digests, original argument policy and hashes of all nine files. Machine-specific commands remain in the original diagnostic receipt outside this fixture.

The Build image has SPIR-V Depth=2 (unspecified), while actual Cross JSON and MSL independently declare a readonly sampled float2D image. The Adapt MSL is the original unqualified Cross output; tests exercise the production readonly argument qualification against the actual validated binary before normalization.

This capture proves parser inputs and their provenance. It contains no metallib, native Metal compilation result, GPU execution or cooked-package qualification. Negative tests mutate copies of these actual outputs; they do not construct substitute valid shader interfaces.
