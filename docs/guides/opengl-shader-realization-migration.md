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

## Native preparation contract draft

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
material binding layouts and tables remain the material authority; native
realization and completion integration are still being implemented in this draft.
No native qualification or full ticket completion is claimed by these interfaces.

`ShaderReflectedInterfaceVariable::nativeName` carries the final target name when
native lookup requires it. Logical reflection may omit it; names are bounded by
`ShaderReflectionLimits::maximumIdentityBytes` and reject embedded NUL or invalid
characters. Pipeline admission checks nested name sizes before copying reflection.
OpenGL 4.1 fragment-output realization must require the actual cooked output name
for `glGetFragDataLocation`; it must not invent names or require OpenGL 4.3 program
interface enumeration. Cook adapters providing native realization must populate
this metadata and preserve logical interface identity separately from target names.
