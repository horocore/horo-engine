# GPU VFX Vulkan offline cooking

`HoroVfxGpuVulkanShaderCook` produces the canonical Simulation and Draw family as actual validated Vulkan 1.3 / SPIR-V 1.6 stages. It uses the unchanged sources `assets/shaders/runtime/vfx_simulation.hlsl` and `assets/shaders/runtime/vfx_gpu_draw.hlsl`, their canonical manifests, and authored reflection names. It does not create a runtime Vulkan provider or advertise GPU simulation support.

The nine positional arguments are:

```text
HoroVfxGpuVulkanShaderCook ABS-repository-root ABS-new-package-root ABS-existing-scratch ABS-catalog ABS-detached-signature ABS-host-trust ABS-dxc ABS-spirv-val ABS-spirv-cross
```

The catalog signature is verified using independently supplied active trust before installing exactly three authenticated tools. Host identity must match the real compiled host. The current local development catalog authorizes its exact macOS tool tuples; it grants no other host approval. No Apple tool, Metal SDK, ambient tool discovery, catalog creation, or observed-binary approval is required by this Vulkan factory. Processes retain the production empty-environment, timeout, output and cancellation bounds.

The compiler target carries exactly DXC and spirv-val. Cross is independently bound as the reflection producer. Each canonical stage is compiled and validated, packaged without translation, and reflected from those exact immutable SPIR-V bytes. The dedicated VFX normalizer uses the same bounded readonly/access/layout proof as the qualified Metal intermediate route; it does not broaden the generic Vulkan raster normalizer. Compute inputs and draw particles remain readonly. Native set/binding placements come from actual evidence and must agree with the binary profile. Compute dispatch parameters occupy 96 bytes and draw view parameters 128 bytes, with actual reflected matrix-major evidence preserved.

Publication requires both complete decoded programs before an atomic rename into an absent destination. Prefixes are `shaders/runtime/vfx-simulation-vulkan` and `shaders/runtime/vfx-gpu-draw-vulkan`, each with `.horoshdr` and `.reflection.json`. The package also retains the authenticated catalog/signature and `vfx-gpu-vulkan-cook-provenance.json`. Private `.vfx-gpu-vulkan-package-` candidate names are reserved. Failure removes only the candidate this invocation claimed and preserves pre-existing destinations and foreign candidates. Scratch and output trees must be disjoint.

The existing Metal CLI, four-tool authentication profile, package prefixes, provenance schema and native source qualification remain unchanged. Parser regressions may reuse genuine validated intermediate capture bytes; they do not establish a new Vulkan producer run or native execution. Actual offline production, native Vulkan compute/draw, synchronization, readback parity, World integration and D3D12 remain separate qualification requirements.
