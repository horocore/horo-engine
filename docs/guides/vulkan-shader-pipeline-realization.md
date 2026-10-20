# Vulkan shader and pipeline realization

HORO-327 implements the existing `IRenderResourceBackend` shader/pipeline/sampler
and `IMaterialBindingBackend` preparation calls for the selected Vulkan device.
The additive public descriptor migration below connects existing typed permutation
values to immutable native pipeline creation. Header target ownership is unchanged.
Frontend cooked-artifact admission and exact generation pins remain authoritative.
Native types stay in the Vulkan target. OpenGL, Metal and Null remain independent.

`VulkanResourceRuntime` retains the sole backend identity issuer and delegates
shader ownership to its private `VulkanShaderRuntime`. Native initialization
provides selected adapter properties and exact dispatch; there is no second
adapter selection, queue, submission, cancellation or shutdown authority.
The host serializes synchronous preparation on its render owner thread and
checks cancellation before/after a call. No asynchronous compilation is launched.

The owner checks SPIR-V 1.6 header/instruction bounds and exact entry-point/stage,
then creates shader modules. Final-artifact reflection produces direct descriptor
set layouts and a pipeline layout. The established MeshVertex triangle ABI,
attachment/depth/raster/blend state and dynamic viewport/scissor become a real
Vulkan graphics pipeline using dynamic rendering. Manifest specialization IDs
map directly to SPIR-V SpecId decorations. Canonical typed runtime overrides from
permutation resolution are supplied per visible stage; omitted IDs use manifest
default bits. No cooked artifact is mutated and no second permutation authority
is introduced.

One device-local native pipeline cache accelerates subsequent creation and is
destroyed before the device. Cache admission checks adapter/module/driver identity,
pipeline-cache UUID and device properties together with artifact/interface/state
identity. No untrusted serialized cache payload is passed into the driver.

Material realization validates the exact pipeline layout, resource generations,
usage, uniform offsets/ranges and packed block extent before creating a private
descriptor pool and immutable descriptor sets. The frontend owns resource backing
and pins all referenced generations; this owner allocates only binding metadata.
Consumer/submission leases must outlive queued use and be released before the
canonical queue drain completes and shader/resource shutdown runs. Destructors
never wait for GPU completion. HORO-326 owns queue submission and bounded drain.

The current graphics envelope is single-sample MeshVertex triangles, read-only
uniform buffers, sampled images and separate non-comparison samplers, four sets
and 128 logical descriptors, at most 4096 resident objects per kind, and a 64 MiB
stage artifact. Inline constants have no value-submission field in the material
contract; unsupported storage/inline/multisample/packed-matrix or packed-array
requests fail with typed actionable errors. Descriptor arrays of supported resources
are populated element by element. This preparation implementation does not claim
Vulkan frame rendering, image upload/transitions or presentation ownership.

Qualification separates injected native-call failure/lifecycle coverage on Linux
and Windows from the opt-in Linux real-device preparation smoke. The latter cooks
GLSL fixtures to SPIR-V 1.6, initializes the production native runtime, and creates
shader modules, pipeline/layout/cache, host-visible uniform backing and descriptor
sets. It does not submit frame work and requires no display. The hosted workflow
fails if its explicitly requested Vulkan device is unavailable. The ordinary suite
skips this smoke unless both fixture paths are supplied. macOS is not a supported
Vulkan product platform and supplies no native Vulkan qualification evidence.

## Public descriptor migration

Reason: `ResolvedShaderPermutation` already carries typed runtime specialization
values, but the pipeline descriptor did not transport them. A backend could only
repeat compiler defaults. `RenderGraphicsPipelineDescriptor::specializationValues`
adds a bounded, increasing-ID override list using the existing
`ShaderSpecializationValue` type. Values must name declared manifest inputs with
matching scalar types; duplicate/unknown IDs, nonfinite floats and invalid booleans
fail admission before native work. The descriptor and resource generation own the
immutable values.

Affected callers are pipeline producers using `RenderFrontend::CreateGraphicsPipeline`
and concrete `IRenderResourceBackend` preparation adapters. Producers with non-empty
overrides compute `PipelineCacheCompatibility::pipelineDescriptorDigest` using
`ComputeRenderGraphicsPipelineDescriptorDigest(state, values)`. Empty overrides
produce the identical legacy state-only digest, so existing source callers and
cache identities are preserved. Existing Metal/OpenGL implementations already
reject any declared specialization inputs; they continue returning typed
unsupported errors rather than dropping valid overrides. Null/D3D12 preparation
retains the existing typed unsupported boundary.

All consumers must rebuild against the additive public struct layout; no stable
binary ABI compatibility is claimed during active development. No new header or
CMake owner is introduced. The RenderApi descriptor/cache tests, existing public
header consumer, Vulkan override/cache tests and Linux native smoke cover the
migration. Both hosted Vulkan matrix jobs build the affected public consumer and
cache/permutation test targets; general renderer CI qualifies peer compositions.
