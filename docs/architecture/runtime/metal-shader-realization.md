# Metal Shader, Pipeline and Material Realization

The Metal module consumes the established `IRenderResourceBackend` and
`IMaterialBindingBackend` preparation contracts. The frontend remains the sole
resource generation, admission, dependency pin and retirement authority.

Native preparation runs synchronously on the initialized render owner, outside
an active frame. The host serializes cancellation and shutdown before or after
these bounded calls. No worker, source compiler, device selection or fallback is
started by preparation. Each collection admits at most 65,535 native instances.
Driver allocation costs are conservative admission estimates rather than measured
Metal allocation sizes. The selected device registry identity and OS driver
version participate in exact pipeline cache compatibility; portable shader
artifacts rebuild native pipelines and native binary cache blobs are not required.

Shader modules consume offline `MTLB` stage payloads, copy their bytes into
system-owned library data, and require the exact cooked entry and native stage.
Graphics pipelines validate both resident stage identities, the full cooked
reflection and selected-device cache identity before publication. Native argument
reflection independently checks active bindings, resource types, access and
uniform packing. Direct scalar uniform-buffer, noncomparison float 2D sampled-texture and sampler
bindings are supported; arrays, matrices, generated helpers, inline constants,
storage resources, depth-comparison sampling and specialization inputs return typed unsupported results.
The fixed MeshVertex layout reserves vertex-buffer slot 30. The current graphics
preparation envelope supports single-sample pipelines.

Materials use exact pipeline layout equality and frontend-resolved generations.
Uniform buffer ranges require 256-byte alignment and the actual native block size.
The immutable table retains native objects, while the frontend retains placed
resource backing through its consumer and command-completion leases. Releasing a
preparation identity does not invalidate an existing material table. Hosts drain
consumer leases before backend shutdown. This preparation work does not implement
frame-pass material draw encoding or claim complete Metal material rendering.

`HoroMetalShaderPreparationSmoke` is opt-in through `HORO_ENABLE_GPU_SMOKE_TESTS`.
Its build cooks a small MSL fixture with the host Apple toolchain; the runtime
under test still accepts offline libraries only. The test requires a native Metal
device and covers exact shader entries, malformed bytes, pipeline/cache identity,
reflected uniform size, material range admission, stale identities and shutdown.

The existing macOS CI smoke step cooks this fixture in its dedicated
`build/ci-metal-shader` configuration and uploads the library, exact source and
checkout identities, tool hashes, build commands and device-test outcome with the
test reports. Missing offline compiler tools fail fixture preparation. A runner
without a Metal device records `skipped_no_device`; successful hosted compilation
alone does not qualify native behavior. The main CI test registration remains in
its existing configuration.
