# CPU over-life and gameplay payload contract (HORO-1730)

This is the CPU descriptor implementation for ColorOverLife, SizeOverLife, and
custom scalar gameplay payload writers. It does not add a GPU kernel, mutable
gameplay particle access, or gameplay output publication through VfxWorld.

Over-life curves are captured and bounded at simulator preparation. Keys have
strictly increasing normalized ages in [0, 1]. Size multipliers are nonnegative;
opacity and color channels are in [0, 1]. After Integrate advances age, the
simulator clamps `age / maximumAge` to [0, 1] and linearly interpolates curves in
key order. It multiplies sampled size by initialized size, and sampled color alpha
by initialized opacity and the separate opacity curve. Values before/after the
first/last key hold the endpoint. These kernels use prepared fixed arrays and do
not allocate during a step.

Migration for existing descriptor callers: duplicate-age curve keys, negative
size multipliers, and color/opacity values outside [0, 1] are now rejected at
preparation. Re-author those curves with strict ages and bounded values; the
current in-repository callers use no such keys.

Each `CpuParticlePayloadModule` records an Integrate-stage operation, read channel,
and write channel. Gameplay descriptors can select an affine transform or a
threshold selector and provide their parameters; two modules may read one input
and write separate outputs in descriptor order. Preparation resolves semantic IDs
to distinct preallocated streams, requires `GameplayInput` → `GameplayOutput`,
checks that no second module writes the same channel, and proves affine endpoint
or threshold output values fit the declared output range. A module writes only its
output stream after age integration;
it cannot receive a mutable particle span or callback into gameplay. A malformed
stage returns `ParticleStageContractViolation`; wrong access class returns
`ParticleGameplayAccessDenied`; missing channel, aliased stream, or invalid range
or operation returns `ParticlePayloadSchemaMismatch`. Module failures include a structured
`payloadModules[index].field` diagnostic. GameplayInput writes during an active
step fail with a stage diagnostic and cannot change that step's frozen input.

Compatibility: when no module owns a GameplayOutput channel, its pre-existing
normalized-age output formula remains active. An explicit module is the sole
writer for its target channel. This preserves existing descriptor users while
making newly authored gameplay output edges explicit; a later schema migration
can require explicit writers for every output.

`Extract` reveals only streams declared `RenderOnly`; private, GameplayInput, and
GameplayOutput spans are empty at their indices. Existing callers that consumed
undeclared or gameplay-classified `Extract.customFloats` spans must declare a
`RenderOnly` channel for render data or use `ReadGameplayOutput` for a declared
gameplay output instead. The typed committed-generation
read remains the current output seam. It is not a general per-particle gameplay
query or a substitute for the bounded post-commit occurrence/aggregate interface
required by ADR-123. Stage and payload failure paths preserve the previous
committed generation.

The focused suite covers analytic curve samples and packed color, same-build
determinism, zero steady-state allocations, both authored operations, all four
access classes, invalid stage/class/channel/writer/operation/range contracts, reentrant write rejection, and
stale-generation output reads. Linux/macOS/Windows hosted test results must be
read from the PR checks; no cross-platform float bit-exactness is claimed.
