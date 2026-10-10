#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

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

        void Initialize(void *device, void *commandQueue) noexcept;
        /** @brief Mutates only a complete reusable native frame slot; pending native references reject before writes. */
        [[nodiscard]] Result<void> UpdateLightFrame(const NativeLightFrameUpdate &update);
        /** @brief Loads an exact cooked Metal library and validates the fixed culling compute ABI before native publication. */
        [[nodiscard]] Result<std::shared_ptr<IResidentLightCullingKernel>> RealizeLightCullingKernel(
            const CookedLightCullingKernel &kernel);
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const;
        [[nodiscard]] Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const;
        [[nodiscard]] Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> initialData,
                                                         const RenderMemoryPlacement &placement);
        [[nodiscard]] Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertexBuffer,
                                                       std::uint64_t indexBuffer);
        [[nodiscard]] Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> initialData,
                                                          const RenderMemoryPlacement &placement);
        [[nodiscard]] Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture);
        [[nodiscard]] Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t colorAttachment,
                                                               std::uint64_t depthAttachment);
        void DestroyBuffer(std::uint64_t backendInstance) noexcept;
        void DestroyMesh(std::uint64_t backendInstance) noexcept;
        void DestroyTexture(std::uint64_t backendInstance) noexcept;
        void DestroyTextureView(std::uint64_t backendInstance) noexcept;
        void DestroyRenderTarget(std::uint64_t backendInstance) noexcept;
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
        void Shutdown() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Render::Detail
