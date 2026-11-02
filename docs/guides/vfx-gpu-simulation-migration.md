# GPU particle simulation contracts (HORO-1731)

GPU particle simulation uses the existing renderer capability snapshot. Its
simulation, direct GPU-buffer draw and asynchronous observation operations are
independent facts: `VfxGpuSimulation`, `VfxGpuDraw` and `VfxGpuReadback`. Generic
compute support or an API name proves none of these operations. Native adapters
enable each fact only when its actual implementation and device support exist;
the new facts default to unavailable.

The existing capability bitset grew from 16 to 32 bits to hold these facts. Every
previous enum value and feature bit position remains unchanged. Rebuild engine,
module and header consumers together; the in-memory C++ layout changed. Repository
consumers use typed capability queries and do not persist this object as a binary
format. No saved project or asset schema conversion is introduced.

`HoroEngine::RenderApi` now publicly depends on the narrow
`HoroEngine::VfxSimulationData` target for canonical force/RNG values. It does not
depend on `HoroEngine::VfxApi`, scene ownership or a concrete renderer. Existing
CPU VFX instance buffers and their material uniform ABI remain separate from the
new explicit GPU word layout.

Logical seed/force copies, pending records and cancellation state remain charged
alongside the renderer's reserved native-buffer/result slice. The existing
`CancellationSource::StatePayloadBytes()` query exposes its state payload for
this accounting without exposing the private state type or allocating anything.
These are logical payload charges; allocator/control-block and native driver
overheads are not presented as byte-exact RSS measurements.

The typed GPU compute and direct additive billboard graph operations describe
exact resources and generations. Portable validation establishes neither native
residency nor execution, and a nonnull kernel interface alone is not a native
preparation receipt. Native adapters must validate their private preparation,
buffer capacities and current generations before encoding. GL 4.1 and Null reject
these operations explicitly.

HORO-1731 remains in progress. The qualified native slice is Metal with explicitly
prepared initial particles; it does not implement a complete authored GPU emitter.
Vulkan offline cooking is qualified, while its native integration is active work.
D3D12 native compute/direct-draw/readback integration remains open. Neither offline
artifacts nor the Metal qualification establish all-backend ticket completion.

## Supported initial-particle slice

`GpuParticleSimulator` currently admits `PreparedParticles`: caller-supplied initial
attributes and stable identities, ordered canonical force modules, semiimplicit
velocity/position integration, rotation, saturated age and finite lifetime kill.
Admission recomputes its finite arithmetic envelope from the actual seed/force
words and bounded step count/delta; unsafe inputs fail before activation.

Continuous spawning, per-step bursts, collision, over-life/payload modules,
gameplay output, other topologies and sorting are unsupported requirements, not
implicitly omitted stages. The source must have zero spawn rate, no collision,
finite lifetime with lifetime kill, billboard rendering and no sorting. The host
must declare the complete unit's required features; unsupported features fail
before activation. The native draw recipe is additive and unsorted. Prepared
particles are not a claim that authored birth, initialization and emitter replay
have been implemented on the GPU.

Existing CPU simulation remains available with its original behavior. The shared
`ParticleSimulationData` force definitions and full-simulation RNG version 1 are
the GPU reference. The older `CpuParticleSpawnPipeline` retains its distinct legacy
spawn stream; it is not substituted for the full-simulation RNG oracle.

## Mixed scene ownership and immutable extraction

The existing `VfxWorld::Prepare(scene, cpuBindings, budget)` remains available. Its mixed overload
also takes mutable `VfxGpuEmitterPreparation` ownership bindings. The host first creates each logical
simulator, prepares its real renderer resources, and activates it with the actual frontend-issued
receipt. World preparation validates the whole CPU/GPU inventory and allocates every world/frame slot
before moving any GPU owner. Failure leaves caller-owned simulators intact.

GPU units share the same scene owner and snapshot ring. `RenderWorldSnapshot::GpuBatches()` contains
issued preparation values, the last accepted generation, optional immutable pending work, a lifetime
revocation token and the captured render-origin delta. It contains no CPU particle substitutes,
live-world pointer or native lease. Generation one denotes the genuine uploaded initial state;
accepted submissions advance generation, while extraction and graph encoding do not.

At the scene owner safe phase, drain actual frontend acknowledgements and pass them to
`ApplyGpuAcknowledgements` in chunks of at most `VfxWorld::MaximumGpuAcknowledgements`, then call
`PrepareGpuSteps` only when the runtime intends to advance simulation. Extract afterwards. A pending
unacknowledged candidate survives repeated extraction unchanged. The host schedules that candidate
only when advancing; while paused it omits pending work and draws the captured accepted generation.
A candidate target generation is drawable only with that exact compute step in the same graph.
The host must retain the canonical runtime pause/activation authority rather than create another
pause state in the extractor.

CPU and GPU batches consume one aggregate batch, particle-lane and draw allowance. GPU capacity is
charged because its live count stays on the GPU. Frame-ring bytes include the real retained GPU batch
values and actual vector capacities; no emitter obtains a separate free frame allowance. The renderer's
native backing and observation ledger remains separate from this extraction slice and must also fit
the host's aggregate reservation.

Only the actual additive unsorted procedural-billboard GPU recipe is admitted here. Stop revokes the
unit token before its effect generation is advanced, preventing old retained snapshots from newly
submitting work. Already accepted leases retire through the renderer. A stopped GPU owner cannot
restart under its old receipt: `ReprepareGpuEffect` installs an entire effect's freshly admitted,
matching-generation GPU owners before Play, preserving the original capacity reservation. CPU restart
behavior remains unchanged. Shutdown and destruction revoke GPU logical admission; immutable frame
values may outlive the world. Native dispatch, direct GPU pixels and observation qualification still
require real cooked artifacts and backend execution; the focused scene tests do not forge receipts.

## Qualified Metal host path

Cold host composition creates the logical simulator, loads the complete genuinely
cooked compute/direct-draw family and calls `RenderFrontend::PrepareVfxGpuSimulation`
with the logical preparation and lifetime token. Only an actual frontend-issued
receipt may activate the logical simulator. Transfer that activated owner through
the mixed `VfxWorld::Prepare` overload; no VFX client selects or calls a native API.

For each intended logical step, use the owner-phase acknowledgement/step/extraction
sequence above. Supply the captured batch's receipt, generation, lifetime token and
origin delta to `RenderFrameScope::ScheduleVfxGpu`; include pending work only when
advancing. Merge its workloads, initial states and required waits into the real
frontend graph and submission. Encoding success does not advance the world:
authentic native-accepted receipts reach the owner-drained acknowledgement queue
only after actual submission acceptance. Explicit observation preparation is a
separate cold operation; observations are disabled by default and never provide
authoritative gameplay state.

The configured Metal end-to-end qualification (`MetalVfxGpuEndToEndSmoke.mm`,
run v7) passed **933 assertions with no failures or skips**, with Metal API
validation enabled and genuine offline-cooked libraries. It exercised the public
frontend, logical simulator and world, same-buffer compute/direct billboard draw,
meaningful pixels, and observation output compared by stable identity against the
full CPU simulator's ordered-force/RNG semantics. It also covered observation-off
zero particle-observation copy/map activity, opt-in result leases/backpressure,
submitted cancellation/timeout, authentic acknowledgements and both immediate Stop
revocation and next-frame retirement. The explicit fixture completion waits and
pixel diagnostic readback are test operations, not a normal-frame synchronization
or observation-off rendering requirement. This evidence applies to the Metal
`PreparedParticles` slice only.
