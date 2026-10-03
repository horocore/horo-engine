# CPU force-module qualification (HORO-1728)

This is the CPU reference for the gravity, wind, attraction, and noise force
modules. It is not evidence that a GPU kernel exists or matches the reference.
The simulator captures a bounded ordered descriptor stack at preparation.
Each fixed tick executes Spawn → Initialize → Forces → Integrate → Collide → Kill
→ Extract. Force modules accumulate acceleration in authored order using the
positions at the Forces barrier; no force changes position or velocity directly.
The module-kind dispatch is outside the per-particle loop.

| Module | Acceleration contribution |
| --- | --- |
| Gravity, wind | `vector * strength` |
| Attraction | unit vector from particle to `center`, times `strength / (1 + falloff * distance)`; zero at the center |
| Noise | three independent `VfxRngV1` samples on the module's semantic channel and axis sample ordinals 0–2, mapped to `[-1, 1)` and scaled by `strength * frequency` |

Noise is a counter-based per-particle, per-tick force, not spatially continuous
Perlin/simplex/curl noise. `frequency` in this descriptor is an amplitude factor;
changing it to temporal frequency requires a new descriptor/RNG version. Channels
1–9 belong to initialization. Noise channels must be at least 10 and distinct
within one stack; default is 16. Invalid/nonfinite parameters and overflowing
constant contributions fail at preparation, before an instance is published.

## Replay and future GPU comparison

`CpuParticleForceQualificationTests` checks the analytic gravity/wind/attraction
result, zero-distance attraction, rejection, and a 120-tick two-instance noise
replay over 16 particles. Its first-tick, first-particle velocity golden is
`(-0.010308341, -0.016924892, 0.009331253)` with absolute tolerance `1e-6`.
For seed `0x123456789ABCDEF0`, activation `(93,4,1)`, emitter `(42,7,3)`,
particle ID `163953441912217`, tick 0, RNG version 1, the channel 19 axis
integer words (sample ordinals 0–2) are `0x04f64d5d01efd62e`,
`0x976c119bf4daf7d4`, `0xf9dad84f812b460f`; channel 20 gives
`0x39675054b2c1a634`, `0x49391e6616f8069b`, `0xaaf2463ee132b728`.
The exact two-instance comparison qualifies repeatability in one build, not
bit-identical float results across platforms. For cross-platform
and future GPU comparisons, use the same effect seed, compiled descriptor/channel
map, activation/emitter identities, particle IDs, ordered ticks and deltas, initial
state, numeric mode, and capacity. Compare stable identities, RNG integer words,
stage order, birth/death counts, and quantized payloads exactly; compare float
position/velocity with an explicitly declared fingerprint or tolerance. Do not
claim GPU equivalence from this CPU-only suite. Any future GPU force kernel must
publish its own comparison against this reference, including platform and backend.

## Reference workload and budget

`HoroCpuForceBenchmark` is a standalone Release target (not a normal CTest gate).
It prepares one point emitter, births 4,096 particles, then advances 20 warmup
and 200 measured fixed ticks at 60 Hz with gravity, wind, attraction, and noise
in that order. It times the entire `Advance` call, including candidate copy,
all stages, and commit, excluding preparation, birth, and extraction. It reports
mean/P99/maximum milliseconds and a position checksum. The initial guardrail
is P99 ≤ 16.667 ms, the full 60 Hz frame; this is an upper bound, not a promised
VFX allocation within a production frame. Machine-specific measured results and
any tighter sub-budget must be recorded below before claiming qualification.

| Platform / CPU / compiler / build | Mean | P99 | Max | Guardrail | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| Linux x86-64, Ryzen 7 9800X3D, GCC 15.2, Release (2026-09-26) | 0.273131 ms | 0.303472 ms | 0.303984 ms | 16.667 ms | Pass, 200 ticks |

Other supported CPU platforms and GPU backends remain unmeasured until their
corresponding runners execute the same fixture. Do not extrapolate this machine's
wall-clock timings to another platform.
