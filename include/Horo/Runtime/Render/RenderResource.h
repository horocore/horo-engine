#pragma once

/**
 * @file RenderResource.h
 * @brief Backend-neutral identities and lifecycle values for resident renderer resources.
 */

#include <compare>
#include <cstdint>

namespace Horo::Render {
    /** @brief Pixel extent of a render surface or offscreen target. */
    struct FramebufferExtent {
        std::uint32_t width{0};
        std::uint32_t height{0};

        /** @brief Reports whether both dimensions can back a render target. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return width > 0 && height > 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const FramebufferExtent &) const noexcept = default;
    };

    /** @brief Process-local identity of one frontend resource-registry lifetime. */
    struct RenderResourceOwnerId {
        std::uint64_t value{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderResourceOwnerId &) const noexcept = default;
    };

    /** @brief Identity of one asynchronous resource operation within a registry lifetime. */
    struct ResourceOperationId {
        std::uint64_t value{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const ResourceOperationId &) const noexcept = default;
    };

    /** @brief Known asynchronous creation outcome; unknown or evicted operations are query errors. */
    enum class RenderResourceOperationState : std::uint8_t {
        Pending,
        Succeeded,
        Failed
    };

    /** @brief Explicit lifecycle state of one resident resource generation. */
    enum class RenderResourceState : std::uint8_t {
        Pending,
        Ready,
        Retiring,
        Retired,
        Failed,
    };

    /** @brief Generation-safe identity of one resident buffer. */
    struct RenderBufferHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderBufferHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one immutable resident mesh. */
    struct RenderMeshHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMeshHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one resident texture. */
    struct RenderTextureHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderTextureHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one resident texture view. */
    struct RenderTextureViewHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderTextureViewHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one resident sampler. */
    struct RenderSamplerHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderSamplerHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one resident shader module. */
    struct RenderShaderModuleHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderShaderModuleHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one resident pipeline. */
    struct RenderPipelineHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderPipelineHandle &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one frontend-owned render target. */
    struct RenderTargetHandle {
        RenderResourceOwnerId owner;
        std::uint32_t slot{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderTargetHandle &) const noexcept = default;
    };

    /** @brief Reserved typed resource identity and its pending completion operation. */
    template <typename Handle> struct ResourceCreation {
        Handle handle;
        ResourceOperationId operation;
    };
}  // namespace Horo::Render
