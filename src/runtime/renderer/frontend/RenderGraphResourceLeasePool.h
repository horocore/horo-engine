#pragma once
#include "Horo/Runtime/Render/MaterialBinding.h"
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
                                                                  RenderGraphTransientResourceSet *transient = nullptr,
                                                                  std::span<const RenderGraphPassWorkload> workloads = {});
        /** @brief Retains material consumers and exact target/mesh pins using the same preallocated frame-completion pool.
         * @param pins Resolved target and mesh generations. @param draws Immutable materials to retain through completion.
         * @return Existing frame lease or typed capacity/residency failure with complete rollback. */
        [[nodiscard]] Result<IRenderGraphResourceLease *> AcquireMaterial(std::span<const RenderGraphResidentIdentity> pins,
                                                                          std::span<const RenderMaterialDraw> draws);

    private:
        using Pin = RenderGraphResidentIdentity;

        class Lease final : public IRenderGraphResourceLease {
        public:
            void Release() noexcept override;
            /**
             * @brief Retains one distinct physical generation in this active lease.
             * @param resource Imported or admitted transient logical binding.
             * @return Success, including an unused declaration or existing pin, or typed binding/capacity failure.
             * @details The outer acquisition transaction owns rollback of an already retained prefix.
             */
            [[nodiscard]] Result<void> PinResource(const RenderGraphResource &resource);
            /** @brief Adds one exact distinct registry pin without allocating storage. */
            [[nodiscard]] Result<void> PinIdentity(const Pin &pin);
            /** @brief Retains exact multiview mesh/material/view generations using this existing completion lease. */
            [[nodiscard]] Result<void> PinMultiview(const RenderGraphMultiview &workload);
            RenderGraphResourceLeasePool *pool{nullptr};
            bool active{false};
            std::vector<Pin> pins;
            UiRenderSubmissionRetention ui;
            RenderGraphTransientResourceSet *transient{nullptr};
            std::array<std::shared_ptr<const ResidentMaterialBinding>, RenderMaterialPass::MaximumDraws> materials;
            std::size_t materialCount{0};
        };

        RenderResourceRegistry *registry_;
        std::size_t maximumPins_;
        std::size_t activePins_{0};
        std::array<Lease, 8> leases_;
    };
}  // namespace Horo::Render::Detail
