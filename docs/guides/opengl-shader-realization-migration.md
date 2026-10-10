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
