# Vulkan shader and pipeline realization

HORO-327 implements the existing `IRenderResourceBackend` shader/pipeline/sampler
and `IMaterialBindingBackend` preparation calls for the selected Vulkan device.
No public header, target ownership or consumer migration changes are required.
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
map directly to SPIR-V SpecId decorations; canonical manifest default bits are
supplied per visible stage. The established public pipeline descriptor has no
runtime-specialization override field; it consumes manifest defaults. Callers
select a cooked artifact/manifest through the existing permutation authority.

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
