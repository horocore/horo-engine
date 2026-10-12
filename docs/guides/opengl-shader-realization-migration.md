# OpenGL cooked shader realization

## Shared package consumption

`HoroEngine::RenderApi` owns `ShaderArtifact.h`. Offline adapters package stage
records with `PackageShaderArtifactStages`; renderer consumers decode the same
HOROSHDR version-one bytes with `DecodeShaderArtifactStages`. The toolchain's
private adapter record is an alias of this shared record and its packaging
function delegates to the sole codec. Native backends do not include toolchain
private headers or link the offline compiler target.

The wire representation is unchanged. Its finite admission now rejects empty
packages, duplicate stage/entry records, invalid entry names, malformed lengths,
foreign target/encoding pairs and trailing bytes. Rebuild malformed cached
artifacts through the owning cook route; runtime decoding never compiles source
or substitutes another target. The maximum package is 64 MiB with eight stages
and 128 bytes per entry name.

Decode results borrow their original package storage. They are synchronous views,
not reconstruction ownership. A renderer creation queue must copy selected code
and validated metadata before returning, and validate native support and final
reflection independently before publishing a resident generation. Shader and
pipeline resident identity remains in the frontend resource registry.

The ownership registry generates a separate consumer translation unit for this
header; an additional consumer checks the finite borrowed-view contract using
only RenderApi usage requirements.

## Native preparation and material consumption

`RenderShaderResources.h` and `RenderShaderResourceErrors.h` also belong to
RenderApi. Shader modules carry the original cooked artifact, exact target,
stage/entry and logical interface identity. Graphics pipelines carry the existing
manifest/reflection/cache compatibility contracts and an immutable state digest
for the established MeshVertex triangle layout. Resident handles remain owned by
the frontend registry; fixed state identity excludes process-local handle values.

The additive `IRenderResourceBackend` shader, graphics-pipeline and sampler
operations default to typed Unsupported. A backend implementing a creation
operation must implement its corresponding destruction operation and cost query.
These defaults create no native object and permit existing peer/custom backends
to compile without pretending to implement realization. Expected failures use
Result; native-free cost queries permit only owned metadata allocation/length
exceptions. New public frontend creation methods are synchronous render-owner
loading operations that return an already-ready generation and completed operation.
They reject active frames and wrong-thread access before changing residency.

Budget admission precedes native allocation. Publication guards own reservation,
unpublished native object and committed backing independently, so an allocation
failure while constructing diagnostics rolls all three back. Shader dependency
pins survive logical module release until dependent pipeline retirement. Existing
material binding layouts and tables remain the material authority. Pass the owning
frontend as `IMaterialBindingBackend` when creating a material table: it resolves
and pins exact pipeline, buffer, texture-view and sampler generations. Packed
parameters become budget-admitted uniform buffers with actual linked-block size
and zero-filled ABI tail padding; the concrete backend never allocates hidden
material backing outside that admission path.

Query `RenderFrontend::QueryPipelineDeviceIdentity()` during owner preparation to
populate the pipeline cache identity from the selected context's driver facts.
This identity describes context compatibility, not physical-adapter enumeration.
An explicit adapter retarget request is unsupported rather than silently ignored.
OpenGL validates the actual linked attributes, fragment output and complete active
uniform-block/sampler reflection before publishing the program. Unsupported
storage resources, arrays, inline constants, specialization and anisotropy above
one return typed failures instead of policy substitutions.

`RenderFrameScope::ExecuteMaterialPass` consumes bounded material draw records
using the established mesh layout and target formats. Its completion lease retains
material consumers, meshes and targets before the first native draw. Sampler and
pipeline release are owner-thread preparation operations and are rejected during
an active frame. After presentation or cancellation, CPU ownership may be released;
exact native completion still controls destruction. `BeginFrame` admission polls
native fences without waiting. `ProcessResourceRequests` drains upload/registry
retirements and does not itself advance GPU completion. The host drains all material
consumers before destroying the frontend and backend.

Material execution has two synchronous phases. Its private no-escape boundary
owns admission, lease transfer and rollback, returning either the original typed
result or a stack-only exception tag after aborting the frame. The public method
then constructs the existing `FrameException` error. It is deliberately not
`noexcept`: owned error identities and messages can allocate, so allocation or
length failure during this second phase may escape, but cleanup is already
complete. Expected backend failures retain their original identity and detail.
There is no Foundation identity migration, virtual-interface change, additional
frame-hot allocation or new backend exception contract. Existing callers and
member-function pointers retain the same signature; public-header consumers and
standard, nonstandard, pre/post-transfer and double-allocation-fault regressions
cover this boundary.

`HoroRenderFrontend` owns this frame-consumption boundary in its private
`RenderFrameMaterialExecution.cpp` translation unit, alongside its existing graph
and parallel frame implementations. Shader resource loading/publication stays in
`RenderFrontendShaderResources.cpp`. Draw identity resolution is separate from
completion-lease acquisition/transfer; neither introduces a new public owner,
cross-target header, dependency or native ABI.

The GL encoder restores caller program, mesh, buffer ranges, texture/sampler units,
target draw buffers and affected raster state. Generic primitive restart, supported
fixed-index restart and all admitted clip distances are neutralized during the
draw and restored afterward. Real device qualification is opt-in through
`HORO_ENABLE_GPU_SMOKE_TESTS`; the standalone `HoroOpenGLMaterialSmoke` uses SDL
only as test-owned presentation composition. Its readback/teardown synchronization
does not occur in the production material consumer.

`ShaderReflectedInterfaceVariable::nativeName` carries the final target name when
native lookup requires it. Logical reflection may omit it; names are bounded by
`ShaderReflectionLimits::maximumIdentityBytes` and reject embedded NUL or invalid
characters. Pipeline admission checks nested name sizes before copying reflection.
OpenGL 4.1 fragment-output realization must require the actual cooked output name
for `glGetFragDataLocation`; it must not invent names or require OpenGL 4.3 program
interface enumeration. Cook adapters providing native realization must populate
this metadata and preserve logical interface identity separately from target names.
