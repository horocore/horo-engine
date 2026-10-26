#pragma once
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTemplate.h"
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
        RenderGraphResourceLeasePool(RenderResourceRegistry &registry, std::size_t maximumPins, std::size_t maximumUiCaptures = 8);
        /** @brief Observes actual preallocated UI capture bytes across every native completion slot; does not allocate or mutate. */
        [[nodiscard]] std::size_t UiRetentionStorageBytes() const noexcept;
        /** @brief Observes the actual configured capture inventory in one canonical completion lease. */
        [[nodiscard]] std::size_t MaximumUiCaptures() const noexcept;
        /** @brief Acquires a bounded lease, rolling back every acquired pin on failure. */
        [[nodiscard]] Result<IRenderGraphResourceLease *> Acquire(std::span<const RenderGraphResource> resources,
                                                                  UiRenderSubmission *ui = nullptr,
                                                                  RenderGraphTransientResourceSet *transient = nullptr,
                                                                  std::span<const RenderGraphPassWorkload> workloads = {},
                                                                  std::shared_ptr<RenderGraphTemplateBindingState> templateBinding = {});
        /** @brief Captures the unique canonical UI contribution on this frame's existing completion lease.
         * @param resources Exact UI graph bindings. @param ui Canonical source leases to transfer on success.
         * @param transient Prepared UI isolation surfaces, distinct from any scene transient set.
         * @param existing Exact active frame lease or null before the frame's first native contribution.
         * @return Same completion lease with bounded added pins, or a typed failure preserving existing ownership.
         * @details Only the frame owner calls this before presentation. Partial added pins roll back; no old source,
         * scene transient set or completion authority is replaced. One lease accepts its preallocated bounded canonical canvas captures,
         * preserving ordered contributions.
         */
        [[nodiscard]] Result<IRenderGraphResourceLease *> AcquireUi(std::span<const RenderGraphResource> resources, UiRenderSubmission &ui,
                                                                    RenderGraphTransientResourceSet *transient,
                                                                    const IRenderGraphResourceLease *existing,
                                                                    std::span<const RenderGraphPassWorkload> workloads = {});
        /** @brief Retains material consumers and exact target/mesh pins using the same preallocated frame-completion pool.
         * @param pins Resolved target and mesh generations. @param draws Immutable materials to retain through completion.
         * @return Existing frame lease or typed capacity/residency failure with complete rollback. */
        [[nodiscard]] Result<IRenderGraphResourceLease *> AcquireMaterial(std::span<const RenderGraphResidentIdentity> pins,
                                                                          std::span<const RenderMaterialDraw> draws);

        /** @brief Extends this exact active frame lease, or acquires its first lease, with GUI sampled generations.
         * @param pins Bounded resolved view/sampler generations; duplicate identities are pinned once.
         * @param existing Exact lease already transferred by this frame, or null for its first completion lease.
         * @return Same active lease, or a typed identity/capacity error with every newly added pin rolled back.
         * @details Only the render owner may extend an active, not-yet-presented frame. Existing pins and
         * completion ownership remain unchanged on failure; this operation never submits or releases the old lease.
         */
        [[nodiscard]] Result<IRenderGraphResourceLease *> AcquireGui(std::span<const RenderGraphResidentIdentity> pins,
                                                                     IRenderGraphResourceLease *existing);

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
            [[nodiscard]] Result<void> PinResource(const RenderGraphResource &resource, RenderGraphTransientResourceSet *resourceTransient);
            /** @brief Adds one exact distinct registry pin without allocating storage. */
            [[nodiscard]] Result<void> PinIdentity(const Pin &pin);
            /** @brief Releases only a newly captured pin prefix, preserving prior native frame ownership. */
            void RollbackPins(std::size_t initialPins) noexcept;
            /** @brief Retains exact multiview mesh/material/view generations using this existing completion lease. */
            [[nodiscard]] Result<void> PinMultiview(const RenderGraphMultiview &workload);
            std::shared_ptr<RenderGraphTemplateBindingState> templateBinding;
            RenderGraphResourceLeasePool *pool{nullptr};
            bool active{false};
            std::vector<Pin> pins;

            struct UiCapture final {
                UiRenderSubmissionRetention source;
                std::array<std::shared_ptr<IResidentUiPaintKernel>, 8> kernels;
                RenderGraphTransientResourceSet *transient{};
            };

            std::vector<UiCapture> ui; /**< Product-admitted capacity allocated once before frame admission. */
            std::size_t uiCount{};
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
