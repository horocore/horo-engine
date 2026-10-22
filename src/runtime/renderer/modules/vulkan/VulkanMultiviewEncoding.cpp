#include "VulkanMultiviewRuntime.h"

#include <bit>

namespace Horo::Render::Detail {
    /** @copydoc VulkanMultiviewRecorder::Initialize */
    void VulkanMultiviewRecorder::Initialize(const VkDevice device, const std::uint64_t deviceIncarnation, const bool vertexLayerEnabled,
                                             const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver) {
        Reset();
        device_ = device;
        deviceIncarnation_ = deviceIncarnation;
        vertexLayerEnabled_ = vertexLayerEnabled;
        available_ = false;
        native_ = {};
        if (!device || deviceIncarnation == 0 || !resolver)
            return;
        native_ = {.begin = std::bit_cast<PFN_vkCmdBeginRendering>(resolver(device, "vkCmdBeginRendering")),
                   .end = std::bit_cast<PFN_vkCmdEndRendering>(resolver(device, "vkCmdEndRendering")),
                   .viewport = std::bit_cast<PFN_vkCmdSetViewport>(resolver(device, "vkCmdSetViewport")),
                   .scissor = std::bit_cast<PFN_vkCmdSetScissor>(resolver(device, "vkCmdSetScissor")),
                   .pipeline = std::bit_cast<PFN_vkCmdBindPipeline>(resolver(device, "vkCmdBindPipeline")),
                   .vertices = std::bit_cast<PFN_vkCmdBindVertexBuffers>(resolver(device, "vkCmdBindVertexBuffers")),
                   .indices = std::bit_cast<PFN_vkCmdBindIndexBuffer>(resolver(device, "vkCmdBindIndexBuffer")),
                   .descriptors = std::bit_cast<PFN_vkCmdBindDescriptorSets>(resolver(device, "vkCmdBindDescriptorSets")),
                   .draw = std::bit_cast<PFN_vkCmdDrawIndexed>(resolver(device, "vkCmdDrawIndexed"))};
        available_ = device && native_.begin && native_.end && native_.viewport && native_.scissor && native_.pipeline &&
                     native_.vertices && native_.indices && native_.descriptors && native_.draw;
    }

    /** @copydoc VulkanMultiviewRecorder::Capabilities */
    MultiviewCapabilities VulkanMultiviewRecorder::Capabilities() const noexcept {
        return {MultiviewExtractionLimits::HardMaximumViews, available_, available_, available_ && vertexLayerEnabled_};
    }

    /** @copydoc VulkanMultiviewRecorder::Reset */
    void VulkanMultiviewRecorder::Reset() noexcept {
        prepared_ = false;
        targetCount_ = 0;
        drawCount_ = 0;
    }

    /** @copydoc VulkanMultiviewRecorder::Begin */
    void VulkanMultiviewRecorder::Begin(const VkCommandBuffer command, const Target &target, const bool array) const noexcept {
        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = array ? target.colorArray : target.color,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {
                                                  .color = {{clearColor_.red, clearColor_.green, clearColor_.blue, clearColor_.alpha}}}};
        const VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = array ? target.depthArray : target.depth,
                                              .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .clearValue = {.depthStencil = {depthClear_, 0}}};
        const auto &rectangle = target.rectangle;
        const VkRect2D area{{static_cast<std::int32_t>(rectangle.x), static_cast<std::int32_t>(rectangle.y)},
                            {rectangle.width, rectangle.height}};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = area,
                                        .layerCount = array ? target.colorDescriptor.layerCount : 1U,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color,
                                        .pDepthAttachment = target.depth ? &depth : nullptr};
        native_.begin(command, &rendering);
        const VkViewport viewport{static_cast<float>(rectangle.x),
                                  static_cast<float>(rectangle.y + rectangle.height),
                                  static_cast<float>(rectangle.width),
                                  -static_cast<float>(rectangle.height),
                                  0.0F,
                                  1.0F};
        native_.viewport(command, 0, 1, &viewport);
        native_.scissor(command, 0, 1, &area);
    }

    /** @copydoc VulkanMultiviewRecorder::DrawInstances */
    void VulkanMultiviewRecorder::DrawInstances(const VkCommandBuffer command, const Draw &draw, const std::uint32_t firstView,
                                                const std::uint32_t count) const noexcept {
        native_.pipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
        constexpr VkDeviceSize offset = 0;
        native_.vertices(command, 0, 1, &draw.mesh.vertex.buffer, &offset);
        native_.indices(command, draw.mesh.index.buffer, 0,
                        draw.mesh.descriptor.indexFormat == RenderIndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        native_.descriptors(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, 0, static_cast<std::uint32_t>(draw.sets.size()),
                            draw.sets.data(), 0, nullptr);
        native_.draw(command, draw.mesh.descriptor.indexCount, count, 0, 0, firstView);
    }

    /** @copydoc VulkanMultiviewRecorder::EncodeView */
    void VulkanMultiviewRecorder::EncodeView(const VkCommandBuffer command, const std::uint32_t view) const noexcept {
        Begin(command, targets_[view], false);
        for (std::uint32_t index = 0; index < drawCount_; ++index)
            if ((draws_[index].views & (std::uint32_t{1} << view)) != 0)
                DrawInstances(command, draws_[index], view, 1);
        native_.end(command);
    }

    /** @copydoc VulkanMultiviewRecorder::Encode */
    void VulkanMultiviewRecorder::Encode(const VkCommandBuffer command) const noexcept {
        HORO_INVARIANT_MSG(prepared_, "Multiview encoding requires complete native preparation.");
        if (mode_ == MultiviewExecutionMode::Multipass) {
            for (std::uint32_t view = 0; view < targetCount_; ++view)
                EncodeView(command, view);
            return;
        }
        Begin(command, targets_[0], true);
        for (std::uint32_t index = 0; index < drawCount_; ++index) {
            auto mask = draws_[index].views;
            while (mask != 0) {
                const auto first = static_cast<std::uint32_t>(std::countr_zero(mask));
                std::uint32_t count = 0;
                for (auto view = first; view < targetCount_ && (mask & (std::uint32_t{1} << view)) != 0; ++view) {
                    ++count;
                    mask &= ~(std::uint32_t{1} << view);
                }
                DrawInstances(command, draws_[index], first, count);
            }
        }
        native_.end(command);
    }
}  // namespace Horo::Render::Detail
