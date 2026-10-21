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
set layouts and a pipeline layout, after exact selected-device aggregate/per-stage
descriptor and bound-set admission. The established MeshVertex triangle ABI,
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
contract; unsupported storage/inline/multisample requests fail with typed actionable
errors. Matrix and parameter arrays use the shared final-artifact material ABI. Descriptor arrays of supported resources
are populated element by element. Valid logical texture/sampler pairing is preserved,
but combined native coordinates require a separate combined-descriptor ABI and
fail with an actionable recook result before Vulkan layout calls. The offscreen consumer records through the HORO-326 command owner, preserving its
exact queue/timeline and retirement authority. Presentation remains outside this capability.

Qualification separates injected native-call failure/lifecycle coverage on Linux
and Windows from the opt-in Linux real-device preparation smoke. The latter cooks
GLSL fixtures to SPIR-V 1.6, initializes the production native runtime, and creates
shader modules, pipeline/layout/cache, host-visible uniform backing, sampled texture/view and sampler objects, and
complete descriptor sets. Logical paired-sampler metadata is preserved while
native Vulkan descriptors stay separate. A second fixture submits an indexed offscreen draw with a view-indexed array of
nonidentity matrices, using the shared typed material packer, then observes the exact
completion and native retirement. It requires no display. No pixel readback is performed;
submission success is not proof of rendered transform pixel values. The hosted workflow
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

## Canonical parameter packing and material submission migration

`MaterialParameterValue` borrows typed scalar spans using existing `ShaderValueType`
and `ShaderParameterId` identities. Values are array-major, then row-major; callers
flatten Horo matrices explicitly according to that documented component order.
`PackMaterialParameterBinding` uses only `MaterialBindingLayout::parameters` from
normalized final-artifact reflection for byte offsets, matrix order, matrix stride
and array stride. It rejects incomplete/duplicate/type-mismatched parameters,
overlapping occupied bytes and overflow, and zeroes padding. The returned block
uses the minimum reflected extent. Frontend-created packed uniform backing explicitly
uses HostVisible access for synchronous CPU initialization; no hidden transfer queue
is created for device-local uploads. `ComputeMaterialParameterBufferByteSize` exposes
that same calculation to adapters; Vulkan applies its 16-byte uniform block tail
alignment and device range limit. Producers must not memcpy native C++ matrix
objects or invent an XR-specific byte/reflection schema.

The existing RenderApi header owner is unchanged. Generic packing regressions,
Vulkan matrix-array admission tests and the material public header consumer cover
this additive API. All consumers rebuild; no stable binary ABI is promised.

`RenderMaterialPass::submission` and the matching resolved request field optionally
carry the existing `RenderQueueSubmission`. Vulkan requires a record, the active
frame identity and the frontend completion lease/transfer receipt. The frontend
rejects explicit records for peers that do not advertise
`supportsExplicitMaterialSubmissions`; legacy peer passes with no record retain
their current contract. Native admission copies all borrowed waits. It never
issues orders, timeline values or implicit queue remaps on the caller's behalf.

Vulkan uses `VulkanGraphRuntime`'s existing `VulkanCommandRuntime` tickets,
`SubmitFrame`, abort, timeline observation and terminal drain. It adds no queue,
semaphore, submission or retirement owner. Native tables retain pipeline/sampler
and buffer/view backing; views retain images and captured targets retain their
views. Submitted tickets retain exact mesh/attachment backing and the one frontend
lease. Registry removal cannot destroy backing still referenced by accepted work.
State changes commit only after successful queue submission. Preparation is
owner-thread load-time work and is rejected during an active frame.

`RenderGraphSynchronizationOperation::GraphicsInputBuffer` records conservative
read-only whole-buffer uniform/vertex/index access, allowing subsequent graph
transitions to preserve write-after-read ordering. Texture or writable uses of
this operation fail generic admission. Material execution accepts one graphics
queue and rejects cross-queue ownership without the graph's matched release/acquire
path; it does not synthesize external release work. The pass currently requires
single-sample whole-image attachments and preserved Store, with defined contents
for Load and sampled reads.

Remaining scope gaps are inline-constant value transport, graphics storage
bindings, MSAA, combined image/sampler native coordinates, WSI presentation and
image upload. These are explicit unsupported outcomes, not completed capabilities.
Hardware pixel-readback qualification and Windows GPU execution remain unproven.
