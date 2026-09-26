# CPU Simulation Qualification

This records the CPU reference for [VFX-002.6 / #1806](https://github.com/horocore/horo-engine/issues/1806). The executable contract is `CpuSimulationQualificationTests.cpp`; the machine-readable CPU/GPU comparison fixture is `tests/fixtures/vfx/cpu_simulation_baseline.json`. Both use the same scene definitions in `tests/support/VfxCpuQualificationFixtures.h`. The standalone `HoroVfxCpuSimulationBenchmark` measures the same workloads without renderer or GPU work.

## Replay and equivalence contract

The three fixed-tick scenes use RNG version 1, seed `0xA55A1234`, delta `1/60` seconds, an initial burst of the declared particle count, no later births, and checkpoints after ticks 0, 59, and 119. `gravity-point` uses gravity and point emission (64 particles); `turbulent-box` adds semantic-channel noise and box emission (512); `bouncing-plane` uses gravity and a declared analytic plane (2,048). They exercise initialization, forces, integration, collision, stable identity, and extraction. They do not exercise GPU execution or external Physics queries.

The JSON fixture is versioned. Each checkpoint publishes the step result, the ordered live identity and packed-color stream hashes, and the first, middle, and last particle IDs, packed colors, positions, and velocities. The hashes are 64-bit FNV-1a with offset `14695981039346656037` and prime `1099511628211`, processing each identity as eight little-endian bytes and each color as four little-endian bytes, in dense order. A GPU comparison harness can read this file directly and compare exact fields to its normalized output; it must preserve semantic particle order and apply the declared numeric tolerance to sampled floats. The fixture does not claim that three sampled particles establish full floating-stream equivalence.

Step counts, generations, IDs, identity/color hashes, and sampled packed colors are exact fields. Sampled positions and velocities use `abs(actual - expected) <= 0.002 + 0.0002 * max(abs(actual), abs(expected))`. This is a qualification tolerance for supported platform floating math, not a claim of bitwise cross-platform floats. Within one executable, two simulators with different buffer identities must replay every step and sampled checkpoint exactly. The test runs in the VFX CI target on Linux/GCC, macOS/Clang, and the focused Windows/MSVC lane; a platform is qualified only when its run on the PR head passes.

## Measured CPU step budgets

The benchmark prepares each scene once, executes 30 warmup ticks and 240 measured ticks, and times only `CpuParticleSimulator::Advance` with `steady_clock`. The first burst is in warmup; the measured window holds the stated active count. The p99 is sorted sample index `floor(240 * 0.99)`. The executable checks the reference p99 ceilings below. These are reference-host qualification ceilings, not portable performance guarantees or a product-wide admission policy.

| Scene | Active particles | Reference p99 ceiling | Local mean | Local p99 | Local max |
|---|---:|---:|---:|---:|---:|
| gravity-point | 64 | 0.020 ms | 0.002009 ms | 0.002034 ms | 0.002034 ms |
| turbulent-box | 512 | 0.100 ms | 0.034595 ms | 0.041929 ms | 0.047850 ms |
| bouncing-plane | 2,048 | 0.500 ms | 0.151700 ms | 0.175371 ms | 0.214504 ms |

Measurement: 2026-09-26, Linux x86_64, AMD Ryzen 7 9800X3D, GCC 15.2, CMake `Release`, headless VFX API, one sample run. The benchmark checks p99 and exits nonzero on an exceeded ceiling. Re-measure on the reference host after simulation changes and record the new result with its compiler, build mode, processor, sample count, and workload. Do not interpret these measurements as GPU timings or as evidence for macOS/Windows CPU budgets.

To reproduce after obtaining the repository build slot:

```sh
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --target HoroVfxApiTests HoroVfxCpuSimulationBenchmark --parallel 2
ctest --test-dir build/skeleton -R '^HoroVfxApiTests::' --output-on-failure
./build/skeleton/tests/HoroVfxCpuSimulationBenchmark
./build/skeleton/tests/HoroVfxCpuSimulationBenchmark --baseline
```

The final command emits a candidate baseline to standard output. Baseline changes require review against the source simulation contract; a local regeneration alone does not justify replacing the published expected data. The current GPU simulation harness is a subsequent VFX stage, so this qualification publishes its comparison input without claiming GPU parity has already passed.
