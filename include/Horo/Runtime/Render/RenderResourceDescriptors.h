#pragma once

/**
 * @file RenderResourceDescriptors.h
 * @brief Backend-neutral buffer, texture, sampler, view, and initial-data contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Horo::Render {
    /** @brief Typed purposes permitted for one resident buffer. */
    enum class RenderBufferUsage : std::uint8_t {
        None = 0,
        Vertex = 1U << 0U,
        Index = 1U << 1U,
        CopySource = 1U << 2U,
        CopyDestination = 1U << 3U,
        Uniform = 1U << 4U,
        Storage = 1U << 5U,
        Indirect = 1U << 6U,
    };

    /** @brief Combines independent buffer purposes without exposing backend flags. */
    [[nodiscard]] constexpr RenderBufferUsage operator|(const RenderBufferUsage left, const RenderBufferUsage right) noexcept {
        return static_cast<RenderBufferUsage>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /** @brief Reports whether every requested usage bit is present. */
    [[nodiscard]] constexpr bool HasBufferUsage(const RenderBufferUsage value, const RenderBufferUsage requested) noexcept {
        return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(requested)) == static_cast<std::uint8_t>(requested);
    }

    /** @brief Backend-neutral CPU access policy for one resident buffer. */
    enum class RenderBufferAccess : std::uint8_t {
        DeviceLocal,
        HostVisible,
    };

    /** @brief Immutable structural policy for one resident buffer. */
    struct RenderBufferDescriptor {
        std::size_t byteSize{0};
        RenderBufferUsage usage{RenderBufferUsage::None};
        RenderBufferAccess access{RenderBufferAccess::DeviceLocal};

        /** @brief Reports whether the size and typed policy values are structurally valid. @return True for valid structure. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            constexpr std::uint8_t validUsageBits =
                static_cast<std::uint8_t>(RenderBufferUsage::Vertex) | static_cast<std::uint8_t>(RenderBufferUsage::Index) |
                static_cast<std::uint8_t>(RenderBufferUsage::CopySource) | static_cast<std::uint8_t>(RenderBufferUsage::CopyDestination) |
                static_cast<std::uint8_t>(RenderBufferUsage::Uniform) | static_cast<std::uint8_t>(RenderBufferUsage::Storage) |
                static_cast<std::uint8_t>(RenderBufferUsage::Indirect);
            const std::uint8_t usageBits = static_cast<std::uint8_t>(usage);
            const bool usageValid = usageBits != 0 && (usageBits & static_cast<std::uint8_t>(~validUsageBits)) == 0;
            const bool accessValid = access == RenderBufferAccess::DeviceLocal || access == RenderBufferAccess::HostVisible;
            return byteSize > 0 && usageValid && accessValid;
        }
    };

    /** @brief Backend-neutral dimensionality of resident texture storage. */
    enum class RenderTextureDimension : std::uint8_t {
        TwoD,
        OneD,
        ThreeD,
    };

    /** @brief Core backend-neutral uncompressed texture and attachment formats. */
    enum class RenderTextureFormat : std::uint8_t {
        Rgba8Unorm,
        Depth24Stencil8,
        Depth32Float,
        R8Unorm,
        Rg8Unorm,
        Rgba8UnormSrgb,
        Bgra8Unorm,
        Bgra8UnormSrgb,
        R16Float,
        Rg16Float,
        Rgba16Float,
        R32Float,
        Rg32Float,
        Rgba32Float,
        Depth16Unorm,
        Depth32FloatStencil8,
    };

    /** @brief Independent purposes permitted for one resident texture. */
    enum class RenderTextureUsage : std::uint8_t {
        None = 0,
        Sampled = 1U << 0U,
        RenderAttachment = 1U << 1U,
        CopySource = 1U << 2U,
        CopyDestination = 1U << 3U,
        Storage = 1U << 4U,
    };

    /** @brief Combines independent texture purposes without exposing backend flags. */
    [[nodiscard]] constexpr RenderTextureUsage operator|(const RenderTextureUsage left, const RenderTextureUsage right) noexcept {
        return static_cast<RenderTextureUsage>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /** @brief Reports whether every requested texture usage bit is present. */
    [[nodiscard]] constexpr bool HasTextureUsage(const RenderTextureUsage value, const RenderTextureUsage requested) noexcept {
        return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(requested)) == static_cast<std::uint8_t>(requested);
    }

    /** @brief Immutable structural policy for one resident texture. */
    struct RenderTextureDescriptor {
        RenderTextureDimension dimension{RenderTextureDimension::TwoD};
        FramebufferExtent extent;
        RenderTextureFormat format{RenderTextureFormat::Rgba8Unorm};
        std::uint32_t mipCount{1};
        std::uint32_t layerCount{1};
        std::uint32_t sampleCount{1};
        RenderTextureUsage usage{RenderTextureUsage::None};
        std::uint32_t depth{1}; /**< Texel depth for ThreeD; one for OneD and TwoD. */

        /** @brief Reports whether the complete backend-neutral texture structure is valid. @return True for valid structure. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return HasValidStorageShape() && HasValidFormatAndUsage() && HasValidSamplingPolicy() && HasValidSubresourceRange();
        }

    private:
        [[nodiscard]] constexpr bool HasValidStorageShape() const noexcept {
            if (!extent.IsValid())
                return false;
            using enum RenderTextureDimension;
            if (dimension == OneD)
                return extent.height == 1 && depth == 1;
            if (dimension == TwoD)
                return depth == 1;
            return dimension == ThreeD && depth > 0 && layerCount == 1;
        }

        [[nodiscard]] constexpr bool HasValidFormatAndUsage() const noexcept {
            constexpr std::uint8_t validUsageBits =
                static_cast<std::uint8_t>(RenderTextureUsage::Sampled) | static_cast<std::uint8_t>(RenderTextureUsage::RenderAttachment) |
                static_cast<std::uint8_t>(RenderTextureUsage::CopySource) | static_cast<std::uint8_t>(RenderTextureUsage::CopyDestination) |
                static_cast<std::uint8_t>(RenderTextureUsage::Storage);
            const std::uint8_t usageBits = static_cast<std::uint8_t>(usage);
            return static_cast<std::uint8_t>(format) <= static_cast<std::uint8_t>(RenderTextureFormat::Depth32FloatStencil8) &&
                   usageBits != 0 && (usageBits & static_cast<std::uint8_t>(~validUsageBits)) == 0;
        }

        [[nodiscard]] constexpr bool HasValidSamplingPolicy() const noexcept {
            return sampleCount != 0 && sampleCount <= 64 && (sampleCount & (sampleCount - 1U)) == 0 &&
                   (sampleCount == 1 || (dimension == RenderTextureDimension::TwoD && mipCount == 1));
        }

        [[nodiscard]] constexpr bool HasValidSubresourceRange() const noexcept {
            if (mipCount == 0 || layerCount == 0)
                return false;
            std::uint32_t largestExtent = extent.width > extent.height ? extent.width : extent.height;
            if (depth > largestExtent)
                largestExtent = depth;
            std::uint32_t maximumMipCount = 0;
            while (largestExtent > 0) {
                ++maximumMipCount;
                largestExtent >>= 1U;
            }
            constexpr std::uint64_t maximumSubresourceCount = 1'048'576;
            return mipCount <= maximumMipCount && static_cast<std::uint64_t>(mipCount) * layerCount <= maximumSubresourceCount;
        }
    };

    /** @brief Returns the byte width of one uncompressed texel for a valid backend-neutral format. */
    [[nodiscard]] constexpr std::optional<std::size_t> RenderTextureTexelBytes(const RenderTextureFormat format) noexcept {
        using enum RenderTextureFormat;
        switch (format) {
            case R8Unorm:
                return 1;
            case Rg8Unorm:
            case R16Float:
            case Depth16Unorm:
                return 2;
            case Rgba8Unorm:
            case Depth24Stencil8:
            case Depth32Float:
            case Rgba8UnormSrgb:
            case Bgra8Unorm:
            case Bgra8UnormSrgb:
            case Rg16Float:
            case R32Float:
                return 4;
            case Rgba16Float:
            case Rg32Float:
            case Depth32FloatStencil8:
                return 8;
            case Rgba32Float:
                return 16;
        }
        return std::nullopt;
    }

    /** @brief Returns a checked tightly packed byte count for the complete base level. */
    [[nodiscard]] constexpr std::optional<std::size_t> RenderTextureBaseLevelByteSize(const RenderTextureDescriptor &descriptor) noexcept {
        if (!descriptor.IsValid())
            return std::nullopt;
        const auto texelBytes = RenderTextureTexelBytes(descriptor.format);
        if (!texelBytes.has_value())
            return std::nullopt;
        constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
        const std::array factors{static_cast<std::size_t>(descriptor.extent.width), static_cast<std::size_t>(descriptor.extent.height),
                                 static_cast<std::size_t>(descriptor.depth), static_cast<std::size_t>(descriptor.layerCount), *texelBytes};
        std::size_t bytes = descriptor.sampleCount;
        for (const std::size_t factor : factors) {
            if (factor != 0 && bytes > maximum / factor)
                return std::nullopt;
            bytes *= factor;
        }
        return bytes;
    }

    /** @brief Selects the image plane exposed by a texture view. */
    enum class RenderTextureAspect : std::uint8_t {
        Color,
        Depth,
        DepthStencil,
        Stencil,
    };

    /** @brief Dimensional interpretation exposed by a texture view. */
    enum class RenderTextureViewDimension : std::uint8_t {
        OneD,
        TwoD,
        TwoDArray,
        Cube,
        CubeArray,
        ThreeD,
    };

    /** @brief Immutable view over one exact resident texture generation. */
    struct RenderTextureViewDescriptor {
        RenderTextureHandle texture;
        RenderTextureFormat format{RenderTextureFormat::Rgba8Unorm};
        RenderTextureAspect aspect{RenderTextureAspect::Color};
        std::uint32_t baseMip{0};
        std::uint32_t mipCount{1};
        std::uint32_t baseLayer{0};
        std::uint32_t layerCount{1};
        RenderTextureViewDimension dimension{RenderTextureViewDimension::TwoD}; /**< Shape exposed to shaders and attachments. */

        /** @brief Reports whether ranges and typed policy values are structurally valid. @return True for valid structure. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            const auto formatValue = static_cast<std::uint8_t>(format);
            const bool formatValid = formatValue <= static_cast<std::uint8_t>(RenderTextureFormat::Depth32FloatStencil8);
            const bool aspectValid = static_cast<std::uint8_t>(aspect) <= static_cast<std::uint8_t>(RenderTextureAspect::Stencil);
            const bool dimensionValid =
                static_cast<std::uint8_t>(dimension) <= static_cast<std::uint8_t>(RenderTextureViewDimension::ThreeD);
            return texture.IsValid() && formatValid && aspectValid && dimensionValid && mipCount > 0 && layerCount > 0;
        }
    };

    /** @brief Texel filtering mode independent of a native sampler enum. */
    enum class RenderSamplerFilter : std::uint8_t {
        Nearest,
        Linear,
    };

    /** @brief Mipmap filtering mode independent of a native sampler enum. */
    enum class RenderSamplerMipmapMode : std::uint8_t {
        Nearest,
        Linear,
    };

    /** @brief Texture-coordinate behavior outside the normalized image range. */
    enum class RenderSamplerAddressMode : std::uint8_t {
        Repeat,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    /** @brief Optional depth comparison performed by a sampling operation. */
    enum class RenderCompareFunction : std::uint8_t {
        Never,
        Less,
        LessEqual,
        Equal,
        GreaterEqual,
        Greater,
        NotEqual,
        Always,
    };

    /** @brief Immutable backend-neutral filtering and addressing policy. */
    struct RenderSamplerDescriptor {
        RenderSamplerFilter minFilter{RenderSamplerFilter::Nearest};              /**< Minification filter. */
        RenderSamplerFilter magFilter{RenderSamplerFilter::Nearest};              /**< Magnification filter. */
        RenderSamplerMipmapMode mipmapMode{RenderSamplerMipmapMode::Nearest};     /**< Mipmap selection filter. */
        RenderSamplerAddressMode addressU{RenderSamplerAddressMode::ClampToEdge}; /**< First-coordinate addressing. */
        RenderSamplerAddressMode addressV{RenderSamplerAddressMode::ClampToEdge}; /**< Second-coordinate addressing. */
        RenderSamplerAddressMode addressW{RenderSamplerAddressMode::ClampToEdge}; /**< Third-coordinate addressing. */
        float minimumLod{0.0F};                                                   /**< Inclusive finite minimum LOD. */
        float maximumLod{0.0F};                                                   /**< Inclusive finite maximum LOD. */
        float maximumAnisotropy{1.0F};                                            /**< Requested finite ratio, at least one. */
        bool compareEnabled{false};                                               /**< Whether depth comparison is requested. */
        RenderCompareFunction compare{RenderCompareFunction::Always};             /**< Known comparison, ignored when disabled. */

        /** @brief Reports whether enum values and numeric policy are structurally valid. @return True for valid structure. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            const auto finite = [](const float value) constexpr noexcept {
                constexpr float maximum = std::numeric_limits<float>::max();
                return value == value && value >= -maximum && value <= maximum;
            };
            return static_cast<std::uint8_t>(minFilter) <= static_cast<std::uint8_t>(RenderSamplerFilter::Linear) &&
                   static_cast<std::uint8_t>(magFilter) <= static_cast<std::uint8_t>(RenderSamplerFilter::Linear) &&
                   static_cast<std::uint8_t>(mipmapMode) <= static_cast<std::uint8_t>(RenderSamplerMipmapMode::Linear) &&
                   static_cast<std::uint8_t>(addressU) <= static_cast<std::uint8_t>(RenderSamplerAddressMode::ClampToBorder) &&
                   static_cast<std::uint8_t>(addressV) <= static_cast<std::uint8_t>(RenderSamplerAddressMode::ClampToBorder) &&
                   static_cast<std::uint8_t>(addressW) <= static_cast<std::uint8_t>(RenderSamplerAddressMode::ClampToBorder) &&
                   finite(minimumLod) && finite(maximumLod) && minimumLod >= 0.0F && maximumLod >= minimumLod &&
                   finite(maximumAnisotropy) && maximumAnisotropy >= 1.0F &&
                   static_cast<std::uint8_t>(compare) <= static_cast<std::uint8_t>(RenderCompareFunction::Always);
        }
    };

    /**
     * @brief Synchronously borrowed optional initial bytes for one buffer.
     *
     * The view owns no storage. A caller that queues asynchronous work must copy the bytes
     * before the validating/enqueueing call returns. An empty view requests no initial data.
     */
    struct RenderBufferInitialDataView {
        std::span<const std::byte> bytes; /**< Borrowed bytes, empty when no initialization is requested. */
    };

    /**
     * @brief Synchronously borrowed bytes and layout for one texture subresource.
     *
     * The byte view owns no storage and is valid only for the synchronous call receiving it.
     */
    struct RenderTextureSubresourceInitialDataView {
        std::uint32_t mipLevel{0};        /**< Zero-based source mip. */
        std::uint32_t arrayLayer{0};      /**< Zero-based source layer. */
        std::size_t rowPitchBytes{0};     /**< Byte distance between rows. */
        std::size_t slicePitchBytes{0};   /**< Byte distance between depth slices; total bytes for a 2D subresource. */
        std::span<const std::byte> bytes; /**< Borrowed bytes covering every depth slice. */
    };

    /**
     * @brief Canonically ordered synchronously borrowed texture initial-data records.
     *
     * Records are ordered by array layer and then mip level. The value validates layout only;
     * it does not grant texture-upload support or extend the lifetime of either span.
     */
    struct RenderTextureInitialDataView {
        std::span<const RenderTextureSubresourceInitialDataView> subresources; /**< Borrowed canonical records. */
    };

    /**
     * @brief Validates one buffer descriptor structurally without querying a backend.
     * @param descriptor Immutable backend-neutral structure.
     * @return Success or RenderResourceDescriptorErrors::BufferInvalid.
     */
    [[nodiscard]] Result<void> ValidateRenderBufferDescriptor(const RenderBufferDescriptor &descriptor);

    /**
     * @brief Validates one texture descriptor structurally without querying a backend.
     * @param descriptor Immutable backend-neutral structure.
     * @return Success or RenderResourceDescriptorErrors::TextureInvalid.
     */
    [[nodiscard]] Result<void> ValidateRenderTextureDescriptor(const RenderTextureDescriptor &descriptor);

    /**
     * @brief Validates one sampler descriptor structurally without creating a sampler.
     * @param descriptor Immutable filtering and addressing policy.
     * @return Success or RenderResourceDescriptorErrors::SamplerInvalid.
     */
    [[nodiscard]] Result<void> ValidateRenderSamplerDescriptor(const RenderSamplerDescriptor &descriptor);

    /**
     * @brief Validates one texture-view descriptor structurally without resolving its handle.
     * @param descriptor Immutable source identity and view policy.
     * @return Success or RenderResourceDescriptorErrors::TextureViewInvalid.
     */
    [[nodiscard]] Result<void> ValidateRenderTextureViewDescriptor(const RenderTextureViewDescriptor &descriptor);

    /**
     * @brief Validates optional initial buffer bytes against the immutable buffer descriptor.
     * @param descriptor Valid buffer structure.
     * @param initialData Borrowed bytes consumed only during this call.
     * @return Success, a descriptor error, or RenderResourceDescriptorErrors::BufferInitialDataInvalid.
     */
    [[nodiscard]] Result<void> ValidateRenderBufferInitialData(const RenderBufferDescriptor &descriptor,
                                                               RenderBufferInitialDataView initialData);

    /**
     * @brief Validates borrowed texture subresource layouts without uploading or retaining them.
     * @param descriptor Valid texture structure.
     * @param initialData Borrowed canonical subresource records consumed only during this call.
     * @return Success, a descriptor error, or RenderResourceDescriptorErrors::TextureInitialDataInvalid.
     */
    [[nodiscard]] Result<void> ValidateRenderTextureInitialData(const RenderTextureDescriptor &descriptor,
                                                                RenderTextureInitialDataView initialData);

    /**
     * @brief Validates view format, aspect, and ranges against an immutable source descriptor.
     * @param texture Immutable source texture structure.
     * @param view Structurally valid view policy.
     * @return Success, a descriptor error, or RenderResourceDescriptorErrors::TextureViewIncompatible.
     */
    [[nodiscard]] Result<void> ValidateRenderTextureViewCompatibility(const RenderTextureDescriptor &texture,
                                                                      const RenderTextureViewDescriptor &view);
}  // namespace Horo::Render
