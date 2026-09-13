#pragma once

#include "VulkanBackendModule.h"

namespace Horo::Render::Detail::VulkanResourceConversion {
    [[nodiscard]] VkBufferUsageFlags BufferUsage(RenderBufferUsage usage) noexcept;
    [[nodiscard]] VkFormat TextureFormat(RenderTextureFormat format) noexcept;
    [[nodiscard]] VkImageUsageFlags ImageUsage(const RenderTextureDescriptor &descriptor) noexcept;
    [[nodiscard]] VkImageType ImageType(RenderTextureDimension dimension) noexcept;
    [[nodiscard]] VkImageCreateFlags ImageFlags(const RenderTextureDescriptor &descriptor) noexcept;
    [[nodiscard]] VkImageCreateInfo ImageCreateInfo(const RenderTextureDescriptor &descriptor) noexcept;
    [[nodiscard]] VkImageViewType ViewType(RenderTextureViewDimension dimension) noexcept;
    [[nodiscard]] VkImageAspectFlags Aspect(RenderTextureAspect aspect) noexcept;
    [[nodiscard]] bool ViewDimensionMatches(const RenderTextureDescriptor &texture, const RenderTextureViewDescriptor &view) noexcept;
    [[nodiscard]] bool PlacementMatches(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept;
    [[nodiscard]] RenderMemoryCompatibilityId Compatibility(std::uint32_t memoryTypeBits, RenderMemoryClass memoryClass) noexcept;
    [[nodiscard]] bool RequirementsValid(const VkMemoryRequirements &requirements, std::size_t payloadBytes) noexcept;
    [[nodiscard]] bool RequirementsMatch(const VkMemoryRequirements &requirements, const RenderMemoryCostPlan &plan) noexcept;
}  // namespace Horo::Render::Detail::VulkanResourceConversion
