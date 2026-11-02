#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "VfxFrontendState.h"

#include <thread>
#include <vector>

namespace Horo::Render::Detail {
    class RenderResourceRegistry;

    /** @brief Cold simulation inventory pins in the existing registry, independent of the eight frame-completion leases.
     * @details The native preparation retains this lease for its entire simulation lifetime. Per-frame leases
     * remain separate. Release runs on the render owner after native retirement, or becomes a no-op after
     * frontend revocation; registry shutdown then owns invalidation and accounting after native drain.
     */
    class VfxGpuInventoryLease final : public IRenderGraphResourceLease {
    public:
        /** @brief Acquires all exact buffer pins transactionally before publishing native preparation.
         * @param registry Existing resource authority. @param buffers Distinct ready buffer generations.
         * @param authority Existing live frontend-incarnation marker.
         * @return Complete cold lease or original typed error; failure releases every acquired prefix pin.
         */
        [[nodiscard]] static Result<std::shared_ptr<VfxGpuInventoryLease>> Create(RenderResourceRegistry &registry,
                                                                                  std::span<const RenderBufferHandle> buffers,
                                                                                  std::weak_ptr<const VfxFrontendState> authority);

        /** @brief Releases any remaining inventory pins under the same retirement contract. */
        ~VfxGpuInventoryLease() override;
        VfxGpuInventoryLease(const VfxGpuInventoryLease &) = delete;
        VfxGpuInventoryLease &operator=(const VfxGpuInventoryLease &) = delete;

        /** @brief Releases inventory pins once on the render owner; never releases accepted native GPU use itself. */
        void Release() noexcept override;

        /** @brief Reports this lease's retained object and handle-vector payload, excluding allocator/control-block overhead. */
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        /** @brief Captures the existing registry and owner incarnation without retaining a frontend pointer. */
        explicit VfxGpuInventoryLease(RenderResourceRegistry &registry, std::weak_ptr<const VfxFrontendState> authority) noexcept;
        /** @brief Pins one distinct ready buffer generation after owner/representation checks. */
        [[nodiscard]] Result<void> Pin(RenderBufferHandle buffer);

        RenderResourceRegistry *registry_;
        std::weak_ptr<const VfxFrontendState> authority_;
        std::thread::id owner_;
        std::vector<RenderBufferHandle> buffers_;
        bool active_{true};
    };
}  // namespace Horo::Render::Detail
