#include "VulkanResourceConversions.h"

#include <array>

namespace Horo::Render::Detail::VulkanResourceConversion {
    namespace {
        [[nodiscard]] bool IsDepthFormat(const RenderTextureFormat format) noexcept {
            return format == RenderTextureFormat::Depth16Unorm || format == RenderTextureFormat::Depth24Stencil8 ||
                   format == RenderTextureFormat::Depth32Float || format == RenderTextureFormat::Depth32FloatStencil8;
        }

    }  // namespace

    VkBufferUsageFlags BufferUsage(const RenderBufferUsage usage) noexcept {
        VkBufferUsageFlags flags{0};
        if (HasBufferUsage(usage, RenderBufferUsage::Vertex))
            flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (HasBufferUsage(usage, RenderBufferUsage::Index))
            flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (HasBufferUsage(usage, RenderBufferUsage::CopySource))
            flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (HasBufferUsage(usage, RenderBufferUsage::CopyDestination))
            flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (HasBufferUsage(usage, RenderBufferUsage::Uniform))
            flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (HasBufferUsage(usage, RenderBufferUsage::Storage))
            flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (HasBufferUsage(usage, RenderBufferUsage::Indirect))
            flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        return flags;
    }

    VkFormat TextureFormat(const RenderTextureFormat format) noexcept {
        using Mapping = std::pair<RenderTextureFormat, VkFormat>;
        constexpr std::array mappings{
            Mapping{RenderTextureFormat::Rgba8Unorm, VK_FORMAT_R8G8B8A8_UNORM},
            Mapping{RenderTextureFormat::Depth24Stencil8, VK_FORMAT_D24_UNORM_S8_UINT},
            Mapping{RenderTextureFormat::Depth32Float, VK_FORMAT_D32_SFLOAT},
            Mapping{RenderTextureFormat::R8Unorm, VK_FORMAT_R8_UNORM},
            Mapping{RenderTextureFormat::Rg8Unorm, VK_FORMAT_R8G8_UNORM},
            Mapping{RenderTextureFormat::Rgba8UnormSrgb, VK_FORMAT_R8G8B8A8_SRGB},
            Mapping{RenderTextureFormat::Bgra8Unorm, VK_FORMAT_B8G8R8A8_UNORM},
            Mapping{RenderTextureFormat::Bgra8UnormSrgb, VK_FORMAT_B8G8R8A8_SRGB},
            Mapping{RenderTextureFormat::R16Float, VK_FORMAT_R16_SFLOAT},
            Mapping{RenderTextureFormat::Rg16Float, VK_FORMAT_R16G16_SFLOAT},
            Mapping{RenderTextureFormat::Rgba16Float, VK_FORMAT_R16G16B16A16_SFLOAT},
            Mapping{RenderTextureFormat::R32Float, VK_FORMAT_R32_SFLOAT},
            Mapping{RenderTextureFormat::Rg32Float, VK_FORMAT_R32G32_SFLOAT},
            Mapping{RenderTextureFormat::Rgba32Float, VK_FORMAT_R32G32B32A32_SFLOAT},
            Mapping{RenderTextureFormat::Depth16Unorm, VK_FORMAT_D16_UNORM},
            Mapping{RenderTextureFormat::Depth32FloatStencil8, VK_FORMAT_D32_SFLOAT_S8_UINT},
        };
        const auto mapping = std::ranges::find_if(mappings, [format](const Mapping candidate) {
            return candidate.first == format;
        });
        return mapping == mappings.end() ? VK_FORMAT_UNDEFINED : mapping->second;
    }

    VkImageUsageFlags ImageUsage(const RenderTextureDescriptor &descriptor) noexcept {
        VkImageUsageFlags flags{0};
        const RenderTextureUsage usage = descriptor.usage;
        if (HasTextureUsage(usage, RenderTextureUsage::Sampled))
            flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (HasTextureUsage(usage, RenderTextureUsage::RenderAttachment))
            flags |= IsDepthFormat(descriptor.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (HasTextureUsage(usage, RenderTextureUsage::CopySource))
            flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (HasTextureUsage(usage, RenderTextureUsage::CopyDestination))
            flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (HasTextureUsage(usage, RenderTextureUsage::Storage))
            flags |= VK_IMAGE_USAGE_STORAGE_BIT;
        return flags;
    }

    VkImageType ImageType(const RenderTextureDimension dimension) noexcept {
        if (dimension == RenderTextureDimension::OneD)
            return VK_IMAGE_TYPE_1D;
        if (dimension == RenderTextureDimension::ThreeD)
            return VK_IMAGE_TYPE_3D;
        return VK_IMAGE_TYPE_2D;
    }

    VkImageCreateFlags ImageFlags(const RenderTextureDescriptor &descriptor) noexcept {
        return descriptor.dimension == RenderTextureDimension::TwoD && descriptor.extent.width == descriptor.extent.height &&
                       descriptor.layerCount >= 6
                   ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
                   : 0;
    }

    VkImageCreateInfo ImageCreateInfo(const RenderTextureDescriptor &descriptor) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .flags = ImageFlags(descriptor),
                .imageType = ImageType(descriptor.dimension),
                .format = TextureFormat(descriptor.format),
                .extent = {descriptor.extent.width, descriptor.extent.height, descriptor.depth},
                .mipLevels = descriptor.mipCount,
                .arrayLayers = descriptor.layerCount,
                .samples = static_cast<VkSampleCountFlagBits>(descriptor.sampleCount),
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = ImageUsage(descriptor),
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    }

    VkImageViewType ViewType(const RenderTextureViewDimension dimension) noexcept {
        using enum RenderTextureViewDimension;
        switch (dimension) {
            case OneD:
                return VK_IMAGE_VIEW_TYPE_1D;
            case TwoD:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TwoDArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case Cube:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case CubeArray:
                return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            case ThreeD:
                return VK_IMAGE_VIEW_TYPE_3D;
        }
        return VK_IMAGE_VIEW_TYPE_2D;
    }

    VkImageAspectFlags Aspect(const RenderTextureAspect aspect) noexcept {
        if (aspect == RenderTextureAspect::Color)
            return VK_IMAGE_ASPECT_COLOR_BIT;
        if (aspect == RenderTextureAspect::Depth)
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        if (aspect == RenderTextureAspect::Stencil)
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    bool ViewDimensionMatches(const RenderTextureDescriptor &texture, const RenderTextureViewDescriptor &view) noexcept {
        return texture.format == view.format && ValidateRenderTextureViewCompatibility(texture, view).HasValue();
    }

    bool PlacementMatches(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
        const bool identityMatches = placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass;
        const bool accountingMatches = placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes;
        return placement.IsValid() && identityMatches && accountingMatches && placement.provenance == plan.provenance &&
               placement.compatibility == plan.compatibility && placement.offsetBytes == 0;
    }

    RenderMemoryCompatibilityId Compatibility(const std::uint32_t memoryTypeBits, const RenderMemoryClass memoryClass) noexcept {
        return RenderMemoryCompatibilityId{(static_cast<std::uint64_t>(memoryTypeBits) << 32U) |
                                           (static_cast<std::uint64_t>(memoryClass) + 1U)};
    }

    bool RequirementsValid(const VkMemoryRequirements &requirements, const std::size_t payloadBytes) noexcept {
        return requirements.memoryTypeBits != 0 && requirements.size >= payloadBytes && requirements.alignment != 0 &&
               (requirements.alignment & (requirements.alignment - 1U)) == 0;
    }

    bool RequirementsMatch(const VkMemoryRequirements &requirements, const RenderMemoryCostPlan &plan) noexcept {
        return requirements.size == plan.requiredBytes && requirements.alignment == plan.alignment &&
               Compatibility(requirements.memoryTypeBits, plan.memoryClass) == plan.compatibility;
    }
}  // namespace Horo::Render::Detail::VulkanResourceConversion
