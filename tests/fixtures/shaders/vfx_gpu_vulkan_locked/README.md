# PreparedParticles Vulkan fixture

These seven files are the actual repository producer's Vulkan compute/draw family,
cross-cooked on macOS ARM64 from `assets/shaders/runtime/vfx_simulation.hlsl` and
`vfx_gpu_draw.hlsl`. They are test inputs, not build output used as a compiler
substitute. The native test verifies every file and source digest, decodes the real
reflection and validates the complete program family before native realization.

`vfx-gpu-vulkan-cook-provenance.json` records informational integrity, source and
artifact identities, and approved producer-tool digests. It does not approve a
Linux compiler, provide an independent trust root or establish runtime security
policy. No private signing key or host trust configuration is included. Normal
production cooking still requires the host's independently approved tool catalog
and trust configuration; see `docs/guides/vfx-gpu-vulkan-offline-cook.md`.

`HoroVulkanCommandExecutionSmoke` receives this directory through its CMake fixture
definition. `HORO_VFX_GPU_COOKED_ROOT` may select another complete family with the
same pinned identities. The test never cooks at runtime or substitutes CPU
simulation for native GPU output. Missing fixtures are explicitly skipped.
