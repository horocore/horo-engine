# GPU Particle Simulation Qualification

This record covers VFX-003, VFX-003.2, VFX-003.3 and VFX-003.4. The normative
execution, observation and fallback requirements remain in
[VFX And Particles Architecture](vfx-and-particles-architecture.md) and the
[Render Backend Parity Contract](render-backend-parity-contract.md).
ADR-124 currently has Proposed status.

## Equivalence policy version 1

`tests/support/VfxGpuQualification.h` owns the executable policy. Vulkan's
`VulkanVfxGpuNativeSmoke.inl` and Metal's `MetalVfxGpuEndToEndFixture.mm` compare
actual normalized native observations with independent deterministic
`CpuParticleSimulator` instances. The native tests seed eight particles from
the CPU initialization path with effect seed `0x123456789abcdef0` and compare
every particle after each of twelve 0.1-second steps. Box emission and authored
speed, size and lifetime ranges produce distinct seed lanes, exposing stream or
particle indexing mistakes as observation mismatches.

The cases are gravity alone, wind alone, simple attraction alone and the ordered
gravity/wind/attraction/noise composition. GPU initialization, force lowering,
native dispatch, submission acknowledgement, bounded asynchronous observation
and CPU reference advancement use their ordinary production contracts.

For finite binary32 values, a property passes when
`abs(actual - reference) <= absolute + relative * abs(reference)`.

| Observation properties | Absolute tolerance | Relative tolerance |
| --- | --- | --- |
| positionX, positionY, positionZ | 0.00002 | 0.0002 |
| velocityX, velocityY, velocityZ | 0.00002 | 0.0002 |
| rotation, age | 0.00002 | 0.0002 |
| sizeX, sizeY, angularVelocity, maximumAge | 0 | 0 |

Particle identity, packed color, custom flags, alive state and reserved observation
words compare exactly. Nonfinite actual or reference values fail. Headless tests
exercise mismatch detection, absolute-boundary inclusion, the next representable
value beyond the boundary and nonfinite inputs for every property. Changing the
formula or limits requires a policy version change and renewed native evidence.

Missing native compute produces a visible Catch skip containing the existing typed
capability error. Known unavailable Vulkan loader/adapter results retain their
original code and message. Native validation, malformed packages, dispatch failures,
timeouts and numerical mismatches fail. CTest recognizes all-skipped return code 4.

## Budget policy version 1

The opt-in `HoroVulkanVfxGpuBudget` entry runs 65,536 prepared particles with
gravity, wind and attraction, at a fixed simulation delta of 1/60 second. It uses
a 64 by 64 offscreen RGBA8 target, the existing direct billboard draw and an
authored disabled observation policy. Eight warmup frames precede 64 measured
frames. The host declares a finite 128 MiB admission envelope and a 64 MiB renderer
retention slice. Cold preparation, seed construction and shader loading occur
before measurement.

The metric is CPU wall time from ordinary world preparation through graph
scheduling, compute/draw submission and explicit completion observation. The
nearest-rank p95 is sorted sample 61 of 64 and must be at most 16.667 ms.
The executable prints p50, p95, maximum, workload, adapter and budget identity.
This bounded test observes completion with `DeterministicTest` purpose. Production
frame paths retain their asynchronous submission behavior. Diagnostics must show
zero observation copies and copied bytes throughout this workload.

`HORO_VULKAN_TEST_DEVICE` must name the exact native adapter for a budget run.
The test's loader host rejects other physical devices before selection and prints
the admitted name and raw driver version. A missing named adapter retains the
backend's typed unavailable result. An unset name visibly skips the budget test.
The Windows fixture leases `vulkan-1.dll` from System32. Linux retains its existing
system-loader paths. Locked JSON and HLSL evidence uses canonical LF text digests,
including on CRLF Windows checkouts. Binary shader payload digests remain exact.

## Compute-less tier matrix

The `HoroVfxApiTests` qualification case exercises Baseline, Standard, High and
Ultra with both Unsupported and Unknown compute facts. Each case uses the same
explicit authored CPU variant, identity 73, with 512 particles replacing a
2,048-particle GPU primary. The variant declares 64 bytes per particle and a
4 ms CPU work ceiling. Resolution must record CompatibleCpu, the exact variant,
particle count, 32,768-byte memory projection and capability/policy revisions.
A 4.0001 ms variant and a RequireGPU source both reject with MissingVariant.

| Authored composition | Permitted result and visible difference |
| --- | --- |
| Dual CPU/GPU primary kernels, PreferGPU, compute unavailable | Same declared CPU primary, full count, CPU cost admission |
| GPU primary plus admitted compatible CPU variant | Exact recorded authored variant, declared reduced count and CPU cost |
| RequireGPU with unavailable compute | Typed rejection |
| Unsupported source features such as spawning, collisions or over-life programs | Typed unsupported before native activation |
| Headless cosmetic source with explicit NullSuppression variant | Recorded suppression under the existing headless resolver contract |
| Gameplay-required source | CPU authority and compatible requirements remain mandatory |

The compute-less matrix proves pure resolver behavior. Native copy, map, dispatch,
completion and driver qualification require the separate native runs. GPU
observations retain their authored cosmetic/diagnostic purpose and never grant
gameplay authority.

## Evidence status

The initial Windows inventory reports Vulkan instance version 1.4.341, NVIDIA
GeForce RTX 5080 with driver 610.88 and AMD Radeon(TM) Graphics with driver
2026.04.17. Inventory alone leaves native simulation, parity and budget evidence
pending. Metal requires a compatible macOS host and the authenticated cooked
package selected by `HORO_VFX_GPU_ASSET_ROOT`.

| Backend and composition | Required evidence status |
| --- | --- |
| Vulkan, Windows RTX 5080 | Native parity and high-count budget pending |
| Vulkan, Windows AMD Radeon | Native parity and high-count budget pending |
| Metal, compatible macOS | External host and genuine cooked package required |
| Compute-less profiles | Headless resolver matrix prepared, execution pending |
| Null and protocol fakes | Logical ownership/readback evidence only |

Native and headless execution results, exact commands and local analysis status
will be recorded after the shared Windows heavy-build queue permits validation.
