#pragma once

/** @file VfxFrameUpload.h @brief Frontend-issued, exact-material VFX uniform-slot publication. */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/VfxDrawData.h"
#include "Horo/Runtime/Render/VfxGraphWorkload.h"

namespace Horo::Render {
    class RenderFrontend;

    namespace Detail {
        struct VfxFrontendState;
    }

    /** @brief Stable VFX slot outcomes; selected-renderer fences remain the native completion authority. */
    namespace VfxFrameErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor Pending;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Unavailable;
    }  // namespace VfxFrameErrors

    /** @brief Frontend-issued proof for one exact reflected VFX block and resident material generation.
     * @details Created at a non-frame preparation point, retaining the canonical material and one fixed
     * native-size staging allocation. No buffer or pipeline is created here. All use/destruction is on the
     * render owner thread. Staging destruction allocates nothing and the slot stores no frontend pointer.
     * Releasing the canonical material lease still follows its existing registry/native retirement contract:
     * the host must release slots and graph borrowers before destroying the frontend. Shutdown revokes the weak
     * owner marker before teardown. Update requires unique shared-handle ownership: any facade copy,
     * including a retained graph revision, conservatively prevents CPU-slot reuse. This is not GPU completion
     * evidence; native aliases are checked independently. Do not create new borrowers during publication.
     * The slot itself cannot be copied, moved or constructed by callers.
     */
    class PreparedVfxFrameSlot final {
    public:
        ~PreparedVfxFrameSlot() = default;
        PreparedVfxFrameSlot(const PreparedVfxFrameSlot &) = delete;
        PreparedVfxFrameSlot &operator=(const PreparedVfxFrameSlot &) = delete;

        /** @brief Borrows the exact canonical material retained by this slot. @return Immutable preparation lease. */
        [[nodiscard]] const std::shared_ptr<const ResidentMaterialBinding> &Material() const noexcept {
            return material_;
        }

        /** @brief Returns accepted publication identity. @return Zero until successful publication. */
        [[nodiscard]] std::uint64_t Revision() const noexcept {
            return published_ ? revision_ : 0;
        }

        /** @brief Returns accepted finite draw size. @return Zero before publication, otherwise one through128. */
        [[nodiscard]] std::uint32_t InstanceCount() const noexcept {
            return published_ ? instanceCount_ : 0;
        }

        /** @brief Checks exact issued capture, native revision and draw range without interpreting any backend handle.
         * @param workload Retained canonical snapshot and view order. @param draw Exact draw record from that order.
         * @return Success or typed invalid/stale/unpublished proof failure; no allocation on success.
         */
        [[nodiscard]] Result<void> ValidateDraw(const RenderGraphVfx &workload, const VfxPreparedDraw &draw) const;

    private:
        friend class RenderFrontend;

        PreparedVfxFrameSlot(std::weak_ptr<const Detail::VfxFrontendState> owner, std::shared_ptr<const ResidentMaterialBinding> material,
                             RenderBufferHandle buffer, std::size_t bytes, std::uint64_t deviceIncarnation)
            : owner_(std::move(owner)), material_(std::move(material)), buffer_(buffer), staging_(bytes),
              deviceIncarnation_(deviceIncarnation) {}

        std::weak_ptr<const Detail::VfxFrontendState> owner_;
        std::shared_ptr<const ResidentMaterialBinding> material_;
        RenderBufferHandle buffer_;
        std::vector<std::byte> staging_;
        std::uint64_t deviceIncarnation_{};
        std::uint64_t revision_{};
        std::uint32_t instanceCount_{};
        bool published_{false};
        bool updating_{false}; /**< Owner-thread publication guard, reset before the retained mutation owner is released. */
        const VfxDrawOrder *drawPreparation_{}; /**< Synchronous renderer-only draw transaction, never a retained borrowed pointer. */
        /** @brief Records canonical capture provenance only after the complete renderer draw transaction succeeds. */
        void CaptureDraw(const RenderGraphVfx &workload, std::uint32_t firstOrder, std::uint32_t batch, RenderMeshHandle mesh);
        std::weak_ptr<const RenderWorldFrameData> capture_;
        std::weak_ptr<const VfxDrawOrder> order_;
        VfxGraphStage stage_{VfxGraphStage::OpaqueForward};
        std::uint32_t firstOrder_{};
        std::uint32_t batch_{};
        RenderMeshHandle mesh_;
    };

    /** @brief Explicit native write-boundary receipt; failure before Attempted preserves the old publication. */
    enum class VfxFrameWriteStatus : std::uint8_t {
        NotStarted,
        Attempted,
        Committed
    };

    /** @brief Synchronously borrowed selected-adapter publication, not a generic buffer-update interface.
     * @details Frontend resolves the issued slot's exact buffer/pipeline, borrows its retained native material
     * and packs the exact reflected VFX schema. Adapters independently verify native namespaces, matching
     * material range/schema, revision and all prior native use before writing. Nothing is borrowed afterward.
     * Error after an attempted native write invalidates the slot publication.
     */
    struct NativeVfxFrameUpdate final {
        std::uint64_t instance{};
        std::uint64_t pipeline{};
        const IResidentMaterialBinding *material{};
        const MaterialBindingLayout *layout{};
        std::uint64_t revision{};
        std::uint32_t instanceCount{};
        std::span<const std::byte> parameters;
        CancellationToken cancellation;
        VfxFrameWriteStatus *writeStatus{};
        std::uint64_t deviceIncarnation{};
    };
}  // namespace Horo::Render
