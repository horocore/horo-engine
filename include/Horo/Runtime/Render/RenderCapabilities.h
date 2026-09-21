#pragma once

/**
 * @file RenderCapabilities.h
 * @brief Backend-neutral renderer feature, queue, limit, and format contracts.
 */

#include "Horo/Runtime/Render/RenderResourceDescriptors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Horo::Render {
    /** @brief Independently reported renderer capabilities. */
    enum class RenderCapability : std::uint8_t {
        Presentation,
        OffscreenTargets,
        TimestampQueries,
        Compute,
        BindlessResources,
        RayTracing,
        BufferResources,
        MeshResources,
        TextureResources,
        RenderTargetResources,
        Count, /**< Number of defined capabilities; not itself a capability bit. */
    };

    /** @brief Bounded bitset of backend-neutral renderer capabilities. */
    struct RenderCapabilitySet final {
        std::uint16_t bits{0}; /**< Enabled capability bits. */

        /** @brief Number of capability values currently defined by the public contract. */
        static constexpr std::uint16_t CapabilityCount = static_cast<std::uint16_t>(RenderCapability::Count);

        static_assert(CapabilityCount <= std::numeric_limits<std::uint16_t>::digits,
                      "Render capability count exceeds the capability bitset width.");

        /** @brief Mask containing every capability bit defined by the public contract. */
        static constexpr std::uint16_t KnownBits = static_cast<std::uint16_t>((std::uint32_t{1} << CapabilityCount) - 1U);

        /**
         * @brief Returns the bit for one capability.
         * @param capability Capability to encode.
         * @return The corresponding single-bit mask.
         */
        [[nodiscard]] static constexpr std::uint16_t Bit(const RenderCapability capability) noexcept {
            const auto value = static_cast<std::uint8_t>(capability);
            return value < CapabilityCount ? static_cast<std::uint16_t>(std::uint32_t{1} << value) : 0;
        }

        /**
         * @brief Reports whether one capability is present.
         * @param capability Capability to query.
         * @return `true` when the capability is enabled.
         */
        [[nodiscard]] constexpr bool Supports(const RenderCapability capability) const noexcept {
            const std::uint16_t bit = Bit(capability);
            return bit != 0 && (bits & bit) != 0;
        }

        /**
         * @brief Adds one capability to this set.
         * @param capability Capability to enable.
         */
        constexpr void Enable(const RenderCapability capability) noexcept {
            bits = static_cast<std::uint16_t>(bits | Bit(capability));
        }

        /**
         * @brief Reports whether the set contains only known capability bits.
         * @return `true` when no reserved capability bits are set.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return (bits & static_cast<std::uint16_t>(~KnownBits)) == 0;
        }
    };

    /** @brief Queue families exposed by a backend-neutral renderer. */
    enum class RenderQueueKind : std::uint8_t {
        Graphics,
        Compute,
        Copy,
        Present,
    };

    /** @brief Immutable queue support reported by one backend instance. */
    struct RenderQueueCapabilities final {
        bool graphics{false}; /**< Graphics queue support. */
        bool compute{false};  /**< Compute queue support. */
        bool copy{false};     /**< Copy queue support. */
        bool present{false};  /**< Presentation queue support. */

        /**
         * @brief Reports whether the requested logical queue is available.
         * @param queue Logical queue family to query.
         * @return `true` when the queue family is available.
         */
        [[nodiscard]] constexpr bool Supports(const RenderQueueKind queue) const noexcept {
            switch (queue) {
                case RenderQueueKind::Graphics:
                    return graphics;
                case RenderQueueKind::Compute:
                    return compute;
                case RenderQueueKind::Copy:
                    return copy;
                case RenderQueueKind::Present:
                    return present;
                default:
                    return false;
            }
        }
    };

    /** @brief Finite backend-neutral resource and frame limits. */
    struct RenderResourceLimits final {
        std::uint64_t maxBufferBytes{0};        /**< Maximum admitted buffer size in bytes. */
        std::uint32_t maxTextureDimension2D{0}; /**< Maximum admitted two-dimensional texture extent. */
        std::uint32_t maxColorAttachments{0};   /**< Maximum simultaneous color attachments. */
        std::uint32_t maxVertexAttributes{0};   /**< Maximum vertex attributes per vertex layout. */
        std::uint32_t maxFramesInFlight{0};     /**< Maximum bounded frames retained by the backend. */

        /**
         * @brief Reports whether the limits are bounded and structurally valid.
         * @return `true` when the limits satisfy the shared contract.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maxFramesInFlight <= 8;
        }

        /**
         * @brief Reports whether a buffer fits the advertised limit.
         * @param descriptor Buffer request to validate.
         * @return `true` when the descriptor is valid and within the limit.
         */
        [[nodiscard]] constexpr bool Supports(const RenderBufferDescriptor &descriptor) const noexcept {
            return descriptor.IsValid() && maxBufferBytes != 0 && descriptor.byteSize <= maxBufferBytes;
        }

        /**
         * @brief Reports whether a two-dimensional texture fits the advertised limits.
         * @param descriptor Texture request to validate.
         * @return `true` when the descriptor is valid and within the limits.
         */
        [[nodiscard]] constexpr bool Supports(const RenderTextureDescriptor &descriptor) const noexcept {
            return descriptor.IsValid() && maxTextureDimension2D != 0 && descriptor.dimension == RenderTextureDimension::TwoD &&
                   descriptor.extent.width <= maxTextureDimension2D && descriptor.extent.height <= maxTextureDimension2D;
        }
    };

    /** @brief Complete usage and sample-count support for backend-neutral texture formats. */
    struct RenderFormatCapabilities final {
        static constexpr std::size_t FormatCount = static_cast<std::size_t>(RenderTextureFormat::Depth32FloatStencil8) + 1;

        std::array<RenderTextureUsage, FormatCount> usages{}; /**< Supported usage bits by texture format. */
        std::uint64_t sampleCountMask{0};                     /**< Supported sample counts encoded by bit position. */

        /**
         * @brief Reports whether a complete format, usage, and sample-count request is supported.
         * @param descriptor Texture request to validate.
         * @return `true` when format, usage, dimension, and sample count are supported.
         */
        [[nodiscard]] constexpr bool Supports(const RenderTextureDescriptor &descriptor) const noexcept {
            if (!descriptor.IsValid() || descriptor.sampleCount >= 64)
                return false;
            const auto format = static_cast<std::size_t>(descriptor.format);
            if (format >= usages.size())
                return false;
            return HasTextureUsage(usages[format], descriptor.usage) &&
                   (sampleCountMask & (std::uint64_t{1} << descriptor.sampleCount)) != 0;
        }
    };

    /** @brief Immutable capability snapshot used for resource and plan admission. */
    struct RenderCapabilitySnapshot final {
        std::uint64_t deviceIncarnation{0};  /**< Non-zero identity for the admitted device instance. */
        std::uint64_t capabilityRevision{0}; /**< Non-zero revision for this capability snapshot. */
        bool synthetic{false};               /**< Whether the snapshot represents a non-native test or null device. */
        RenderCapabilitySet features;        /**< Feature support advertised by the backend. */
        RenderQueueCapabilities queues;      /**< Queue families available to the backend. */
        RenderResourceLimits limits;         /**< Bounded resource and frame limits. */
        RenderFormatCapabilities formats;    /**< Format, usage, and sample-count support. */

        /**
         * @brief Reports whether identity and capability data are bounded and coherent.
         * @return `true` when the snapshot can be used for admission decisions.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return deviceIncarnation != 0 && capabilityRevision != 0 && features.IsValid() && limits.IsValid();
        }

        /**
         * @brief Reports whether the snapshot admits one buffer request.
         * @param descriptor Buffer request to validate.
         * @return `true` when the feature and size limits admit the request.
         */
        [[nodiscard]] constexpr bool Supports(const RenderBufferDescriptor &descriptor) const noexcept {
            return features.Supports(RenderCapability::BufferResources) && limits.Supports(descriptor);
        }

        /**
         * @brief Reports whether the snapshot admits one texture request.
         * @param descriptor Texture request to validate.
         * @return `true` when feature, extent, format, usage, and sample limits admit the request.
         */
        [[nodiscard]] constexpr bool Supports(const RenderTextureDescriptor &descriptor) const noexcept {
            return features.Supports(RenderCapability::TextureResources) && limits.Supports(descriptor) && formats.Supports(descriptor);
        }
    };
}  // namespace Horo::Render
