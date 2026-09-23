#include "Horo/Runtime/Render/RenderCapabilities.h"

#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>

namespace Horo::Render {
    namespace {
        [[nodiscard]] std::string TextureFormatName(const RenderTextureFormat format) {
            constexpr std::array<std::string_view, RenderFormatCapabilities::FormatCount> names{
                "rgba8-unorm", "depth24-stencil8", "depth32-float", "r8-unorm",
                "rg8-unorm",   "rgba8-unorm-srgb", "bgra8-unorm",   "bgra8-unorm-srgb",
                "r16-float",   "rg16-float",       "rgba16-float",  "r32-float",
                "rg32-float",  "rgba32-float",     "depth16-unorm", "depth32-float-stencil8",
            };
            const auto index = static_cast<std::size_t>(format);
            return index < names.size() ? std::string{names[index]} : std::format("unknown({})", index);
        }

        [[nodiscard]] constexpr std::string_view TextureDimensionName(const RenderTextureDimension dimension) noexcept {
            using enum RenderTextureDimension;
            switch (dimension) {
                case TwoD:
                    return "2d";
                case OneD:
                    return "1d";
                case ThreeD:
                    return "3d";
            }
            return "unknown";
        }

        [[nodiscard]] constexpr std::string_view BufferAccessName(const RenderBufferAccess access) noexcept {
            using enum RenderBufferAccess;
            switch (access) {
                case DeviceLocal:
                    return "device-local";
                case HostVisible:
                    return "host-visible";
            }
            return "unknown";
        }

        void AppendUsage(std::string &result, const std::uint8_t bits, const std::uint8_t flag, const std::string_view name) {
            if ((bits & flag) == 0)
                return;
            if (!result.empty())
                result += '|';
            result += name;
        }

        [[nodiscard]] std::string TextureUsageName(const RenderTextureUsage usage) {
            const auto bits = static_cast<std::uint8_t>(usage);
            std::string result;
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderTextureUsage::Sampled), "sampled");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderTextureUsage::RenderAttachment), "attachment");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderTextureUsage::CopySource), "copy-source");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderTextureUsage::CopyDestination), "copy-destination");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderTextureUsage::Storage), "storage");
            return result.empty() ? "none" : result;
        }

        [[nodiscard]] std::string BufferUsageName(const RenderBufferUsage usage) {
            const auto bits = static_cast<std::uint8_t>(usage);
            std::string result;
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::Vertex), "vertex");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::Index), "index");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::CopySource), "copy-source");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::CopyDestination), "copy-destination");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::Uniform), "uniform");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::Storage), "storage");
            AppendUsage(result, bits, static_cast<std::uint8_t>(RenderBufferUsage::Indirect), "indirect");
            return result.empty() ? "none" : result;
        }

        [[nodiscard]] std::string SupportedSampleCounts(const std::uint64_t mask) {
            std::string result;
            for (const std::uint32_t count : {1U, 2U, 4U, 8U, 16U, 32U}) {
                if ((mask & (std::uint64_t{1} << count)) == 0)
                    continue;
                if (!result.empty())
                    result += '|';
                result += std::to_string(count);
            }
            return result.empty() ? "none" : result;
        }

        void AppendReason(std::string &reasons, const std::string_view reason) {
            if (!reasons.empty())
                reasons += "; ";
            reasons += reason;
        }
    }  // namespace

    /** @copydoc DescribeRenderBufferRequest */
    std::string DescribeRenderBufferRequest(const RenderBufferDescriptor &descriptor) {
        return std::format("buffer bytes={} usage={} access={}", descriptor.byteSize, BufferUsageName(descriptor.usage),
                           BufferAccessName(descriptor.access));
    }

    /** @copydoc DescribeRenderTextureRequest */
    std::string DescribeRenderTextureRequest(const RenderTextureDescriptor &descriptor) {
        return std::format("texture {}x{} format={} dimension={} usage={} samples={} mips={} layers={} depth={}", descriptor.extent.width,
                           descriptor.extent.height, TextureFormatName(descriptor.format), TextureDimensionName(descriptor.dimension),
                           TextureUsageName(descriptor.usage), descriptor.sampleCount, descriptor.mipCount, descriptor.layerCount,
                           descriptor.depth);
    }

    /** @copydoc DescribeRenderBufferAdmissionFailure */
    std::string DescribeRenderBufferAdmissionFailure(const RenderBufferDescriptor &descriptor,
                                                     const RenderCapabilitySnapshot &capabilities) {
        std::string reasons;
        if (!descriptor.IsValid())
            AppendReason(reasons, "descriptor is structurally invalid");
        if (!capabilities.features.Supports(RenderCapability::BufferResources))
            AppendReason(reasons, "buffer resources are unavailable");
        if (capabilities.limits.maxBufferBytes == 0)
            AppendReason(reasons, "no buffer bytes are admitted");
        else if (descriptor.byteSize > capabilities.limits.maxBufferBytes)
            AppendReason(reasons, std::format("size exceeds max {} bytes", capabilities.limits.maxBufferBytes));
        return DescribeRenderBufferRequest(descriptor) + " rejected: " + reasons;
    }

    /** @copydoc DescribeRenderTextureAdmissionFailure */
    std::string DescribeRenderTextureAdmissionFailure(const RenderTextureDescriptor &descriptor,
                                                      const RenderCapabilitySnapshot &capabilities) {
        const std::string format = TextureFormatName(descriptor.format);
        std::string reasons;
        if (!descriptor.IsValid())
            AppendReason(reasons, "descriptor is structurally invalid");
        if (!capabilities.features.Supports(RenderCapability::TextureResources))
            AppendReason(reasons, "texture resources are unavailable");
        if (descriptor.dimension != RenderTextureDimension::TwoD)
            AppendReason(reasons, "only 2d textures are admitted");
        if (capabilities.limits.maxTextureDimension2D == 0)
            AppendReason(reasons, "no 2d texture extent is admitted");
        else if (descriptor.extent.width > capabilities.limits.maxTextureDimension2D ||
                 descriptor.extent.height > capabilities.limits.maxTextureDimension2D)
            AppendReason(reasons, std::format("extent exceeds max {}", capabilities.limits.maxTextureDimension2D));

        const auto formatIndex = static_cast<std::size_t>(descriptor.format);
        if (formatIndex >= capabilities.formats.usages.size()) {
            AppendReason(reasons, "format is unknown");
        } else {
            const RenderTextureUsage allowed = capabilities.formats.usages[formatIndex];
            const auto missing =
                static_cast<RenderTextureUsage>(static_cast<std::uint8_t>(descriptor.usage) & ~static_cast<std::uint8_t>(allowed));
            if (allowed == RenderTextureUsage::None)
                AppendReason(reasons, std::format("{} has no admitted usages", format));
            else if (missing != RenderTextureUsage::None)
                AppendReason(reasons, std::format("{} usage unavailable for {} (supported: {})", TextureUsageName(missing), format,
                                                  TextureUsageName(allowed)));
        }
        if (descriptor.sampleCount >= 64 || (capabilities.formats.sampleCountMask & (std::uint64_t{1} << descriptor.sampleCount)) == 0)
            AppendReason(reasons, std::format("sample count {} unavailable (supported: {})", descriptor.sampleCount,
                                              SupportedSampleCounts(capabilities.formats.sampleCountMask)));

        return DescribeRenderTextureRequest(descriptor) + " rejected: " + reasons;
    }
}  // namespace Horo::Render
