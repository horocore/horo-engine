#pragma once
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderResourceRegistry.h"

#include <array>
#include <vector>

namespace Horo::Render::Detail {
    /** @brief Preallocated owner-thread leases preserving resident generations and memory charges during graph submission. */
    class RenderGraphResourceLeasePool final {
    public:
        /** @brief Reserves all pin storage before the first frame. */
        RenderGraphResourceLeasePool(RenderResourceRegistry &registry, std::size_t maximumPins);
        /** @brief Acquires a bounded lease, rolling back every acquired pin on failure. */
        [[nodiscard]] Result<IRenderGraphResourceLease *> Acquire(std::span<const RenderGraphResource> resources);

    private:
        struct Pin {
            RenderResourceClass resourceClass;
            RenderResourceIdentity identity;
        };

        class Lease final : public IRenderGraphResourceLease {
        public:
            void Release() noexcept override;
            RenderGraphResourceLeasePool *pool{nullptr};
            bool active{false};
            std::vector<Pin> pins;
        };

        RenderResourceRegistry *registry_;
        std::size_t maximumPins_;
        std::size_t activePins_{0};
        std::array<Lease, 8> leases_;
    };
}  // namespace Horo::Render::Detail
