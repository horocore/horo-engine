#pragma once
/** @file VulkanMultiviewRuntime.h
 * @brief Bounded native indexed scene encoder borrowed by the canonical graph command owner.
 */
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "VulkanResourceRuntime.h"

#include <array>

namespace Horo::Render::Detail {
    /** @brief Native command encoder only; owns no queue, command buffer, image allocation, timeline or completion lease. */
    class VulkanMultiviewRecorder final {
    public:
        /** @brief Resolves optional draw dispatch for this exact selected device; no native work or allocation.
         * @param device Borrowed selected device. @param vertexLayerEnabled Actual enabled Vulkan shaderOutputLayer feature.
         * @param resolver Exact selected-device dispatch resolver. */
        void Initialize(VkDevice device, std::uint64_t deviceIncarnation, bool vertexLayerEnabled,
                        const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver);
        /** @brief Returns effective draw/index/target routing support, independent of XR runtime discovery. */
        [[nodiscard]] MultiviewCapabilities Capabilities() const noexcept;
        /** @brief Validates every native draw and target before any command is encoded; failure revokes previous preparation. */
        [[nodiscard]] Result<void> Prepare(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &workload,
                                           std::span<const VulkanGraphResource> resources, VulkanResourceRuntime &registry,
                                           VulkanCommandRetention &retention);
        /** @brief Records one fully admitted frame into the existing canonical command buffer, without submission or waiting. */
        void Encode(VkCommandBuffer command) const noexcept;
        /** @brief Revokes scratch admission without destroying caller-owned native data or synchronization state. */
        void Reset() noexcept;

    private:
        struct Target {
            VkImageView color{};
            VkImageView depth{};
            VkImageView colorArray{};
            VkImageView depthArray{};
            VkImage colorImage{};
            VkImage depthImage{};
            RenderTextureDescriptor colorDescriptor;
            RenderTextureDescriptor depthDescriptor;
            MultiviewRenderRectangle rectangle;
            std::uint32_t layer{};
            std::uint32_t depthLayer{};
        };

        struct Draw {
            VulkanMaterialMesh mesh;
            VkPipeline pipeline{};
            VkPipelineLayout layout{};
            std::span<const VkDescriptorSet> sets;
            RenderGraphicsPipelineState state;
            std::uint32_t views{};
        };

        struct Dispatch {
            PFN_vkCmdBeginRendering begin{};
            PFN_vkCmdEndRendering end{};
            PFN_vkCmdSetViewport viewport{};
            PFN_vkCmdSetScissor scissor{};
            PFN_vkCmdBindPipeline pipeline{};
            PFN_vkCmdBindVertexBuffers vertices{};
            PFN_vkCmdBindIndexBuffer indices{};
            PFN_vkCmdBindDescriptorSets descriptors{};
            PFN_vkCmdDrawIndexed draw{};
        };

        Dispatch native_;

        /** @brief Validates an actual registry-issued one-layer attachment view against its graph texture. */
        [[nodiscard]] Result<VulkanDrawViewBinding> Attachment(const RenderGraphExecutionRequest &, const VulkanResourceRuntime &,
                                                               const VulkanGraphResource &, RenderTextureViewHandle, std::uint32_t,
                                                               RenderTextureAspect) const;
        [[nodiscard]] Result<void> PrepareTargets(const RenderGraphExecutionRequest &, const RenderGraphMultiview &,
                                                  std::span<const VulkanGraphResource>, const VulkanResourceRuntime &);
        /** @brief Captures the actual mesh and validates its exact graph buffer dependencies and immutable ranges. */
        [[nodiscard]] Result<VulkanMaterialMesh> DrawMesh(const RenderGraphExecutionRequest &, const MultiviewGraphGeometry &,
                                                          RenderMeshHandle, std::span<const VulkanGraphResource>, VulkanResourceRuntime &,
                                                          VulkanCommandRetention &) const;
        /** @brief Admits the existing native material lease against device, shader routing and every target tuple. */
        [[nodiscard]] Result<void> PrepareMaterial(Draw &, const MultiviewGraphGeometry &) const;
        [[nodiscard]] Result<void> PrepareDraws(const RenderGraphExecutionRequest &, const RenderGraphMultiview &,
                                                std::span<const VulkanGraphResource>, VulkanResourceRuntime &, VulkanCommandRetention &);
        [[nodiscard]] bool CanRouteLayers() const noexcept;
        void Begin(VkCommandBuffer command, const Target &target, bool array) const noexcept;
        void DrawInstances(VkCommandBuffer command, const Draw &draw, std::uint32_t firstView, std::uint32_t count) const noexcept;
        /** @brief Encodes one admitted multipass view with its exact begin/draw/end recording boundary. */
        void EncodeView(VkCommandBuffer command, std::uint32_t view) const noexcept;
        VkDevice device_{};
        std::uint64_t deviceIncarnation_{};
        bool vertexLayerEnabled_{};
        bool available_{};
        bool prepared_{};
        MultiviewExecutionMode mode_{MultiviewExecutionMode::Multipass};
        std::array<Target, MultiviewExtractionLimits::HardMaximumViews> targets_;
        std::array<Draw, MultiviewExtractionLimits::HardMaximumInstances> draws_;
        std::uint32_t targetCount_{};
        std::uint32_t drawCount_{};
        ClearColor clearColor_;
        float depthClear_{};
    };
}  // namespace Horo::Render::Detail
