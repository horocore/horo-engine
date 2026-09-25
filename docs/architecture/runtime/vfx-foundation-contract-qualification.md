# VFX Foundation Contract Qualification

This document records the focused qualification surface for issue [VFX-001.6 #1769](https://github.com/horocore/horo-engine/issues/1769).
It maps contract claims to regression tests and records which platform results the
current repository CI can establish. These tests qualify the backend-neutral
foundation; they do not qualify renderer execution or cross-platform floating-point
identity.

## Qualification Matrix

| Contract | Evidence | Qualified assertion |
|---|---|---|
| Stable identity and wire representation | `tests/unit/runtime/vfx/VfxIdentityTests.cpp` | Distinct typed identity domains, non-wrapping generations, a fixed 16-byte network-byte-order vector, direct decode, and exact re-encode. |
| Reproducible particle identity and samples | `tests/unit/runtime/vfx/CpuParticleSpawnPipelineTests.cpp`; `tests/unit/runtime/vfx/CpuParticleSimulatorTests.cpp` | Equal seed, activation, emitter, and ordered inputs reproduce particle IDs and state even when the backing buffer identity differs. Simulation comparisons are exact within one executable/platform. |
| Versioned schema and hostile source | `tests/unit/runtime/vfx/ParticleSystemDescriptorTests.cpp`; `tests/unit/runtime/vfx/VfxFoundationQualificationTests.cpp` | Duplicate keys, malformed JSON, unknown fields, unsupported versions, source-byte overflow, and excessive nesting fail with typed errors; semantic validation does not clamp invalid data. |
| Buffer ownership and capacity | `tests/unit/runtime/vfx/CpuParticleBufferTests.cpp` | Aligned packed streams, fixed capacity, generation-safe handles, byte-ceiling rejection, owner-thread rules, and zero counted allocations for prepared spawn/kill/view operations. |
| Cook tiers and quality resolution | `tests/unit/runtime/vfx/ParticleSystemDescriptorTests.cpp`; `tests/unit/runtime/vfx/VfxQualityPolicyTests.cpp`; `tests/unit/runtime/vfx/VfxFoundationQualificationTests.cpp` | Compact, Standard, and Large cook ceilings and Baseline through Ultra domain resolution are explicit; boundary overflow is rejected instead of silently clamped. |
| Source-path context | `tests/unit/runtime/vfx/VfxFoundationQualificationTests.cpp` | Relative, POSIX, Windows-style, and UTF-8 path spellings are preserved exactly in import/cook diagnostics and do not change accepted descriptor identity or data. The VFX layer treats this value as diagnostic context; it does not normalize or resolve the filesystem path. |
| Frame-hot allocation budget | `tests/unit/runtime/vfx/CpuParticleBufferTests.cpp`; `tests/unit/runtime/vfx/CpuParticleSpawnPipelineTests.cpp`; `tests/unit/runtime/vfx/CpuParticleSimulatorTests.cpp`; `tests/unit/runtime/vfx/VfxFoundationQualificationTests.cpp` | After preparation, the measured buffer operations and CPU stepping windows require zero additional calls counted by `AllocationProbe`. The simulator exercises 1,000 steady-state ticks; the end-to-end test also checks one prepared step. |

The allocation probe counts C++ `operator new` calls in the test executable. It is a
regression guard for these specific windows, not a measurement of native allocator
calls, total bytes reserved by the process, CPU time, or renderer allocations. No
performance claim beyond the checked zero-allocation windows is implied.

The focused executable is the `HoroVfxApiTests` CMake target, registered with the
`unit;vfx;headless` labels. Run its complete contract set after the build gate is
granted; do not interpret source inspection or formatting checks as a test pass:

```sh
cmake --build build/skeleton --target HoroVfxApiTests --parallel
ctest --test-dir build/skeleton -R '^HoroVfxApiTests::' --output-on-failure
```

## Platform Evidence And Deviations

| Platform | Current repository CI | Qualification status |
|---|---|---|
| Linux / GCC | Enabled in `.github/workflows/ci.yml` | Required active CI lane. |
| macOS / Clang | Enabled in `.github/workflows/ci.yml` | Required active CI lane. |
| Windows / MSVC | Dedicated `VFX Foundation · Windows / MSVC` job in `.github/workflows/ci.yml` builds and runs `HoroVfxApiTests` | Required focused CI lane. Qualification requires that job to pass on this PR head. |

The full Windows matrix entry remains disabled while the full-suite timeout is
investigated. The dedicated job qualifies this VFX target without waiting for the
entire Windows suite. Its owner is `abdullahbodur`; the remaining full-suite
deviation is tracked by this qualification issue,
[#1769](https://github.com/horocore/horo-engine/issues/1769). Earlier cache work in
[CI-006 #2826](https://github.com/horocore/horo-engine/issues/2826) is closed and
does not provide Windows qualification evidence.
Keep Windows marked unqualified until the dedicated job passes on the PR head.

Deterministic identity bytes, particle IDs, counts, and ordering are exact contracts.
Floating simulation values are only claimed bit-exact under an explicitly qualified
deterministic-math and Physics fingerprint; otherwise compare them under the declared
tolerance. This matches [ADR-123](../../adr/123-vfx-cpu-stage-order-determinism-and-gameplay-coupling.md)
and does not claim cross-platform bitwise equality for the current floating kernels.
