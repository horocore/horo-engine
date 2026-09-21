# VFX Foundation Qualification

This guide records the qualification boundary for the VFX foundation. The
qualification composes the existing typed contracts; it does not add a renderer
backend, interpret an authoring graph, or create a second runtime asset model.

## Qualification matrix

`VfxFoundationQualificationTests.cpp` covers the following evidence:

| Contract | Qualification |
| --- | --- |
| Authored asset path | Strict JSON parse, full import validation, exact material evidence, cook-plan admission, policy resolution, and a headless CPU spawn/age step are composed in one test. |
| Hostile input | Duplicate fields, oversized source bytes, invalid lifetime/kill policy, and missing material evidence return typed failures without an admitted descriptor or cook plan. |
| Domain policy | Headless CPU-only capability evidence resolves a `PreferGPU` visual particle unit to CPU; an interactive GPU-capable snapshot resolves the same unit to GPU. Every `VfxQualityProfile` retains its captured profile in the result. |
| Cook tiers | Compact, Standard, and Large profiles admit their exact particle and spawn-rate boundaries. Values beyond a selected profile remain typed cook findings. |
| Frame-hot path | After `CpuParticleSpawnPipeline::Create`, the composed `Advance` operation performs no heap allocation. The lower-level buffer and spawn suites cover sustained churn and stale-slot safety. |
| Cross-platform source handling | The parser consumes UTF-8 source bytes and diagnostics retain a non-ASCII source name as data; no filesystem, native renderer, or host path is consulted by parse/validate/cook. |

The capability snapshot is the source of truth for domain selection. Product
profile labels are retained as evidence and never select a renderer or platform
backend. Headless execution has no GPU allocation path; a required GPU request
remains unavailable instead of silently switching the selected renderer.

## Running the qualification

Configure the normal headless test composition with testing enabled, then build
and run the VFX target:

```bash
cmake -S . -B build/skeleton -DBUILD_TESTING=ON
cmake --build build/skeleton --target HoroVfxApiTests --parallel 2
ctest --test-dir build/skeleton -R HoroVfxApiTests --output-on-failure
```

The focused qualification cases can be selected with:

```bash
ctest --test-dir build/skeleton -R 'VfxFoundationQualification|HoroVfxApiTests' --output-on-failure
```

## Ownership and limits

The active `SceneRuntime` remains the owner of a future `VfxWorld`; this
qualification only exercises the backend-neutral API foundation. The CPU
pipeline owns its prepared SoA storage on one simulation thread. Renderer-facing
extraction must consume immutable copies at the declared extraction boundary;
mutable spans, native handles, and renderer submission do not cross this API.

No test treats a local Linux result as evidence for another supported platform.
Windows and macOS matrix results remain CI evidence, and any deviation must be
recorded with its platform, compiler, build mode, test command, and follow-up
owner before the foundation is considered fully qualified.

## Local validation record

On 2026-09-21, the Linux x86_64 Debug/optimized default configuration completed:

- `cmake -S . -B build/skeleton -DBUILD_TESTING=ON`
- `cmake --build build/skeleton --target HoroVfxApiTests --parallel 2`
- `ctest --test-dir build/skeleton -R 'HoroVfxApiTests' --output-on-failure`

The VFX target built successfully and all 48 registered VFX tests passed. This
record is local Linux evidence only; the hosted Windows/macOS matrix remains the
source of truth for cross-platform qualification.

## References

- [VFX and Particles Architecture](../architecture/runtime/vfx-and-particles-architecture.md)
- [ADR-011: VFX Effect Ownership, Simulation Domain Policy and Renderer Boundary](../adr/011-vfx-effect-ownership-simulation-domain-and-renderer-boundary.md)
- [Particle-System Descriptor Migration](./particle-system-descriptor-migration.md)
- [CPU Particle Buffer Migration](./cpu-particle-buffer-migration.md)
- [CPU Particle Spawn Pipeline Migration](./cpu-particle-spawn-pipeline-migration.md)
