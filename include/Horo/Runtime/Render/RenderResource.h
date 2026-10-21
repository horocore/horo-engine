#pragma once

/**
 * @file RenderResource.h
 * @brief Backend-neutral identities and lifecycle values for resident renderer resources.
 */

#include <compare>
#include <cstdint>
#include <utility>

namespace Horo::Render {
    /**
     * @brief Stable frontend-owned evidence for the one existing resource memory charge.
     * @details Calls are externally serialized on the render owner. Claim occurs before
     * native creation can enqueue work. Release marks terminal native retirement only;
     * it must not mutate the registry, acknowledge a budget, or invoke native callbacks.
     * The frontend keeps the lease address and its charge alive until that evidence is
     * drained. Failed creation, cancellation and host timeout cannot release accepted work.
     */
    class IRenderResourceRetirementLease {
    public:
        virtual ~IRenderResourceRetirementLease() = default;
        /** @brief Transfers the prepared charge to exactly one native allocation owner. */
        virtual void Claim() noexcept = 0;
        /** @brief Marks native backing and all accepted GPU/copy references terminal, exactly once. */
        virtual void Release() noexcept = 0;
    };

    /**
     * @brief Move-only native allocation ownership of a prepared frontend retirement lease.
     * @details A concrete backend moves this value into its actual allocation before
     * asynchronous work is accepted. Failure retains that allocation until terminal;
     * destruction releases the lease after native backing/copy references are gone.
     * An empty value is for direct native callers with no frontend ledger charge; it
     * does not authorize abandoning their native work. The lease never outlives its
     * externally retained frontend/registry owner.
     */
    class RenderResourceNativeLifetime final {
    public:
        /** @brief Creates an empty direct-native lifetime with no frontend memory charge. */
        RenderResourceNativeLifetime() noexcept = default;

        /** @brief Claims one stable prepared charge before crossing native creation.
         * @param lease Frontend-owned stable slot that outlives this allocation and its drain. */
        explicit RenderResourceNativeLifetime(IRenderResourceRetirementLease &lease) noexcept : lease_(&lease) {
            lease_->Claim();
        }

        /** @brief Releases terminal ownership; native objects must have been destroyed first. */
        ~RenderResourceNativeLifetime() {
            if (lease_ != nullptr)
                lease_->Release();
        }

        /** @brief Reports whether this native allocation owns a frontend charge. */
        [[nodiscard]] bool HasLease() const noexcept {
            return lease_ != nullptr;
        }

        RenderResourceNativeLifetime(const RenderResourceNativeLifetime &) = delete;
        RenderResourceNativeLifetime &operator=(const RenderResourceNativeLifetime &) = delete;

        /** @brief Moves the single retirement authority without a second claim. */
        RenderResourceNativeLifetime(RenderResourceNativeLifetime &&other) noexcept : lease_(std::exchange(other.lease_, nullptr)) {}

        /** @brief Retires any previous terminal allocation, then adopts the single incoming authority. */
        RenderResourceNativeLifetime &operator=(RenderResourceNativeLifetime &&other) noexcept {
            if (this != &other) {
                if (lease_ != nullptr)
                    lease_->Release();
                lease_ = std::exchange(other.lease_, nullptr);
            }
            return *this;
        }

    private:
        IRenderResourceRetirementLease *lease_{};
    };

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
