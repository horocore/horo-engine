#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"

#include <memory>
#ifdef __OBJC__
#include "MetalParallelRecording.h"
#endif

namespace Horo::Render::Detail {
    /** @brief Owns Metal realizations for backend-neutral resident resources. */
    class MetalResourceRuntime final {
    public:
        MetalResourceRuntime();
        ~MetalResourceRuntime();

        MetalResourceRuntime(const MetalResourceRuntime &) = delete;
        MetalResourceRuntime &operator=(const MetalResourceRuntime &) = delete;

        void Initialize(void *device, void *commandQueue, std::uint64_t deviceIncarnation = 0) noexcept;
        /** @brief Publishes one exact mapped VFX uniform generation after its canonical native uses retire. */
        [[nodiscard]] Result<void> UpdateVfxFrame(const NativeVfxFrameUpdate &update);
        /** @brief Validates exact native material, geometry, target and accepted VFX slot publication. */
        [[nodiscard]] Result<void> ValidateVfxWorkload(const RenderGraphVfx &workload, const RenderGraphExecutionRequest &request) const;
        /** @brief Encodes admitted indexed instancing on the existing retained command buffer. */
        [[nodiscard]] Result<void> ExecuteVfxWorkload(void *commands, const RenderGraphVfx &workload,
                                                      const RenderGraphExecutionRequest &request);
        /** @brief Mutates only a complete reusable native frame slot; pending native references reject before writes. */
        [[nodiscard]] Result<void> UpdateLightFrame(const NativeLightFrameUpdate &update);
        /** @brief Loads an exact cooked Metal library and validates the fixed culling compute ABI before native publication. */
        [[nodiscard]] Result<std::shared_ptr<IResidentLightCullingKernel>> RealizeLightCullingKernel(
            const CookedLightCullingKernel &kernel);
        /** @brief Realizes exact cooked raster/stencil states at a preparation safe point; no runtime source compilation. */
        [[nodiscard]] Result<std::shared_ptr<IResidentUiPaintKernel>> RealizeUiPaintKernel(const CookedUiPaintKernel &kernel);
        /** @brief Atomically updates an admitted mapped UI slot after complete native-use validation. */
        [[nodiscard]] Result<void> UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &update);
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const;
        [[nodiscard]] Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> initialData,
                                                         const RenderMemoryPlacement &placement,
                                                         RenderResourceNativeLifetime lifetime = {});
        [[nodiscard]] Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertexBuffer,
                                                       std::uint64_t indexBuffer, RenderResourceNativeLifetime lifetime = {});
        [[nodiscard]] Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> initialData,
                                                          const RenderMemoryPlacement &placement,
                                                          RenderResourceNativeLifetime lifetime = {});
        [[nodiscard]] Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture,
                                                              RenderResourceNativeLifetime lifetime = {});
        [[nodiscard]] Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t colorAttachment,
                                                               std::uint64_t depthAttachment, RenderResourceNativeLifetime lifetime = {});
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const;
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &descriptor,
                                                                               std::size_t cookedBytes) const;
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &descriptor,
                                                                                   std::uint64_t vertex, std::uint64_t fragment) const;
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const;
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &descriptor,
                                                               std::span<const std::uint8_t> cookedCode,
                                                               const RenderMemoryPlacement &placement);
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &descriptor, std::uint64_t vertex,
                                                                   std::uint64_t fragment, const RenderMemoryPlacement &placement);
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &descriptor,
                                                          const RenderMemoryPlacement &placement);
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t pipeline,
                                                                           const MaterialBindingDescriptor &descriptor,
                                                                           ShaderBindingId binding) const;
        /** @brief Selected-device owner-thread preparation; no compilation, fallback or frame work. */
        [[nodiscard]] Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(
            const MaterialBindingDescriptor &descriptor, std::uint64_t pipeline, std::span<const MaterialResidentResource> resources);
        void DestroyShaderModule(std::uint64_t backendInstance) noexcept;
        void DestroyGraphicsPipeline(std::uint64_t backendInstance) noexcept;
        void DestroySampler(std::uint64_t backendInstance) noexcept;
        void DestroyBuffer(std::uint64_t backendInstance) noexcept;
        void DestroyMesh(std::uint64_t backendInstance) noexcept;
        void DestroyTexture(std::uint64_t backendInstance) noexcept;
        void DestroyTextureView(std::uint64_t backendInstance) noexcept;
        void DestroyRenderTarget(std::uint64_t backendInstance) noexcept;
        /** @brief Validates exact UI slot publication, kernel epoch, texture policy and primary target before encoding. */
        [[nodiscard]] Result<void> ValidateUiPaint(const RenderGraphUiPaint &paint, std::span<const RenderGraphResourceInstance> resources,
                                                   void *primary) const;
        /** @brief Encodes admitted UI paint into the current retained command buffer and tracks every resident use. */
        [[nodiscard]] Result<void> ExecuteUiPaint(void *commands, const RenderGraphUiPaint &paint,
                                                  std::span<const RenderGraphResourceInstance> resources, void *primary);
        /** @brief Validates exact native instances, resource policy and copy/attachment bounds. */
        [[nodiscard]] Result<void> ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                                         std::span<const RenderGraphResourceInstance> resources) const;
        /** @brief Encodes prevalidated resource work on a retained-reference command buffer. */
        [[nodiscard]] Result<void> ExecuteGraphWorkload(void *commandBuffer, const RenderGraphWorkload &workload,
                                                        std::span<const RenderGraphResourceInstance> resources);
        /** @brief Commits active resource pins, or abandons them while preserving older submitted uses. */
        void FinishGraphCommands(void *commandBuffer, bool committed) noexcept;
#ifdef __OBJC__
        /** @brief Owner-only freeze of validated native references, including their backing heaps. */
        [[nodiscard]] Result<MetalRecordedOperation> CaptureGraphOperation(const RenderGraphWorkload &workload,
                                                                           std::span<const RenderGraphResourceInstance> resources,
                                                                           id<MTLCommandBuffer> commands, id<MTLTexture> primary);
#endif
        /** @brief Marks all resolved graph residents against the final owner buffer preceding lease retirement. */
        void TrackParallelGraphUse(void *finalCommands, std::span<const RenderGraphResourceInstance> resources) noexcept;
        /** @brief Non-blockingly drains at most 64 native-complete retired instances. */
        void DrainGraphRetirements() noexcept;
        /** @brief Observes every submitted staging buffer without waiting; native failures remain typed terminal evidence. */
        [[nodiscard]] Result<bool> PollUploadDrain() const;
        void Shutdown() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
