#pragma once
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderResourceRegistry.h"
#include "UiRenderSubmissionRetention.h"

#include <array>
#include <vector>

namespace Horo::Render::Detail {
    struct RenderGraphTransientResourceSet;
    /** @brief Typed imported resident identity shared by graph resolution and submission pinning. */
    struct RenderGraphResidentIdentity {
        RenderResourceClass resourceClass;
        RenderResourceIdentity identity;
    };

    /**
     * @brief Maps an imported binding without touching registry or lease state.
     * @param binding Typed buffer/texture binding, or an unsupported unbound resource.
     * @return Exact resource class/generation, or UnsupportedWorkload for an unbound resource.
     */
    [[nodiscard]] Result<RenderGraphResidentIdentity> ResolveGraphResidentIdentity(const RenderGraphResourceBinding &binding);

    /** @brief Preallocated owner-thread leases preserving resident generations and memory charges during graph submission. */
    class RenderGraphResourceLeasePool final {
    public:
        /** @brief Reserves all pin storage before the first frame. */
        RenderGraphResourceLeasePool(RenderResourceRegistry &registry, std::size_t maximumPins);
        /** @brief Acquires a bounded lease, rolling back every acquired pin on failure. */
        [[nodiscard]] Result<IRenderGraphResourceLease *> Acquire(std::span<const RenderGraphResource> resources,
                                                                  UiRenderSubmission *ui = nullptr,
                                                                  RenderGraphTransientResourceSet *transient = nullptr);

    private:
        using Pin = RenderGraphResidentIdentity;

        class Lease final : public IRenderGraphResourceLease {
        public:
            void Release() noexcept override;
            RenderGraphResourceLeasePool *pool{nullptr};
            bool active{false};
            std::vector<Pin> pins;
            UiRenderSubmissionRetention ui;
            RenderGraphTransientResourceSet *transient{nullptr};
        };

        RenderResourceRegistry *registry_;
        std::size_t maximumPins_;
        std::size_t activePins_{0};
        std::array<Lease, 8> leases_;
    };
}  // namespace Horo::Render::Detail
