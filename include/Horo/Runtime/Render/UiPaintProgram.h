#pragma once
/** @file UiPaintProgram.h
 * @brief Immutable prepared native UI pass structure with bounded double-buffered frame data.
 */
#include "Horo/Runtime/Render/UiPaintFrameBufferPool.h"
#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "Horo/Runtime/Render/UiRenderSubmission.h"

namespace Horo::Render {
    namespace Detail {
        struct UiPaintProgramIdentity;
    }

    namespace UiPaintProgramErrors {
        /** @brief Source roles, extent or resource generations require explicit structural-safe-point preparation. */
        extern const ErrorCodeDescriptor PreparationRequired;
        /** @brief Finite prepared pass/resource/draw capacity cannot represent the complete program. */
        extern const ErrorCodeDescriptor Capacity;
    }  // namespace UiPaintProgramErrors

    /** @brief Explicit product bounds and exact projection admitted before creating native residents. */
    struct UiPaintProgramDescriptor final {
        Runtime::Ui::UiResolvedScreenCanvas projection;
        UiPaintFrameBudget capacity;
        std::uint32_t frameSlots{2};
        std::uint32_t maximumPasses{256};
    };

    /** @brief Immutable whole-program admission bounds; observing them grants no execution or resource authority. */
    struct UiPaintProgramInventory final {
        std::size_t passes{};
        std::size_t resources{};
        std::uint32_t draws{}; /**< Maximum ordinary frame draw inventory reserved by this program. */
    };

    class UiPaintProgram;

    /** @brief Issued immutable observation of one accepted slot; never native ownership or a mutable frame authority. */
    class UiPaintPreparedFrame final {
    public:
        UiPaintPreparedFrame() noexcept = default;

        /** @brief Checks whether the issuing preparation still exists. @return Whether its private identity has not retired. */
        [[nodiscard]] bool IsValid() const noexcept {
            return !identity_.expired();
        }

    private:
        friend class UiPaintProgram;

        UiPaintPreparedFrame(std::weak_ptr<const Detail::UiPaintProgramIdentity> identity, std::uint32_t slot, std::uint64_t revision)
            : identity_(std::move(identity)), slot_(slot), revision_(revision) {}

        std::weak_ptr<const Detail::UiPaintProgramIdentity> identity_;
        std::uint32_t slot_{};
        std::uint64_t revision_{};
    };

    /** @brief Exact accepted program/source pair in the one ordered UI composition phase; borrowed program and frame stay live. */
    struct UiPaintProgramSubmission final {
        UiPaintProgram *program{};
        const UiPaintPreparedFrame *prepared{};
        UiRenderSubmission source;
    };

    /**
     * @brief Render-owner prepared consumer of the canonical immutable UI geometry and mask/group projection.
     * @details Compile/allocate occurs only in Create at an explicit structural safe point. Prepare mutates inactive reserved
     * frame storage, uploads a free resident slot, and publishes only after complete success. Visibility, paint colors, text
     * geometry and opacity reuse the same admitted structure; source role/extent/resource changes return PreparationRequired.
     * The host coalesces refresh, cancels superseded preparation and publishes a complete replacement. It may draw a retained
     * program only while its actual canonical source remains current. Submit consumes the ordinary UiRenderSubmission and
     * named ComposeUi path, retaining resources through the existing scene/UI/GUI completion lease. No source service,
     * backend selection, file I/O or shader compilation is discovered here.
     */
    class UiPaintProgram final {
        struct Storage;

    public:
        /** @brief Prepares complete immutable pass structure and finite resident frame slots.
         * @param frontend Selected initialized renderer; preparation runs on its owner outside an active frame.
         * @param descriptor Exact resolved projection and finite product bounds.
         * @param prototype Actual canonical potential-role snapshot; hidden roles remain declared with zero-opacity paint.
         * @param textures Complete unique current source texture handles, including admitted atlas pages.
         * @param kernel Actual offline cooked shader and exact primary output/stencil formats; offscreen linear surfaces are separate.
         * @param cancellation Superseded source/renderer generation fence observed between bounded preparation stages.
         * @return Owned complete preparation or original typed failure; partial residents are released without publication.
         * @details Copies bounded structural values from prototype during cold preparation; it does not retain prototype's
         * animation publication. Accepted frame geometry and queued native completion captures retain their own exact leases.
         */
        [[nodiscard]] static Result<std::unique_ptr<UiPaintProgram>> Create(
            RenderFrontend &frontend, const UiPaintProgramDescriptor &descriptor, const Runtime::Ui::UiRenderSnapshot &prototype,
            std::span<const RenderTextureHandle> textures, const CookedUiPaintKernel &kernel, const CancellationToken &cancellation = {});
        /** @brief Closes owner-thread admission and releases residents after callers are quiescent; GPU pins drain independently. */
        ~UiPaintProgram();
        UiPaintProgram(const UiPaintProgram &) = delete;
        UiPaintProgram &operator=(const UiPaintProgram &) = delete;
        /** @brief Returns exact prepared graph counts and admitted dynamic draw capacity without allocation.
         * @return Whole-program inventory, or zero after close. @pre Caller is the preparation owner thread. */
        [[nodiscard]] UiPaintProgramInventory Inventory() const noexcept;
        /** @brief Observes every owned creation after the host's explicit load-time resource pump and records terminal failures.
         * @return Success only when all exact prepared buffers/surfaces/stencil generations are ready; typed pending/native failure
         * otherwise.
         * @details Registry-retired failed creations relinquish only their exact local handle; the first original error is retained.
         * Unknown or evicted outcomes cannot authorize relinquishment. This owner mutation survives bounded journal eviction.
         * @pre Preparation owner thread, outside an active frame. This does not wait, encode or publish a source. */
        [[nodiscard]] Result<void> ValidatePreparation();
        /** @brief Maps an admitted exact texture into the selected slot's immutable graph identity.
         * @param slot Explicit resident frame slot. @param texture Exact prepared source texture handle.
         * @return Graph identity used by existing image/font submission bindings, or typed stale/preparation failure. */
        [[nodiscard]] Result<RenderGraphResourceId> TextureResource(std::uint32_t slot, RenderTextureHandle texture) const;
        /** @brief Projects and uploads a complete exact frame into an inactive reserved CPU/native slot.
         * @details Success replaces the prior same-slot CPU provenance; failure drops only the unpublished candidate.
         * Already submitted native captures retain their geometry independently until actual completion.
         * @param slot Explicit finite slot. @param source Actual current geometry/image/font/atlas evidence borrowed synchronously.
         * @param cancellation Checked before upload and publication; atlas ownership remains with the caller until Submit.
         * @return Issued accepted frame or original typed error; failure preserves the prior slot publication. */
        [[nodiscard]] Result<UiPaintPreparedFrame> Prepare(std::uint32_t slot, const UiRenderSubmission &source,
                                                           const CancellationToken &cancellation = {});
        /** @brief Encodes the exact accepted program after scene work and before editor GUI.
         * @param frame Actual active frontend frame scope. @param prepared Issued current slot publication.
         * @param source Exact corresponding submission, transferring its sealed atlas lease on admission.
         * @return Canonical ComposeUi outcome; successful encoding alone does not prove presentation or authorize input. */
        [[nodiscard]] Result<void> Submit(RenderFrameScope &frame, const UiPaintPreparedFrame &prepared, UiRenderSubmission source);
        /** @brief Validates every exact source/slot before submitting all canvases as one ordered native UI phase.
         * @param frame Same active scene/GUI frame owner. @param sources Bounded complete paint-order inventory, consumed synchronously.
         * @return Typed failure before encoding for any malformed entry, or canonical batch/native outcome. Duplicate program-slot
         * entries reject; native partial failure retains queued leases and produces no presentation acknowledgement.
         */
        [[nodiscard]] static Result<void> SubmitBatch(RenderFrameScope &frame, std::span<UiPaintProgramSubmission> sources);
        /** @brief Revokes issued frames and releases finite residents on the owning thread; repeated close is safe.
         * @return Original failure retaining unreleased handles for retry, or success. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        /** @brief Requires exact immutable slot/source correspondence before any batch member reaches the native frame. */
        [[nodiscard]] Result<UiPaintGraphComposition> ValidateSubmission(const UiPaintPreparedFrame &prepared, UiRenderSubmission source);
        /** @brief Realizes finite native residents under the factory candidate's rollback ownership.
         * @param frontend Actual selected owner. @param kernel Exact cooked interface and formats.
         * @param cancellation Source preparation fence. @return Complete native preparation or original failure. */
        [[nodiscard]] Result<void> PrepareNative(RenderFrontend &frontend, const CookedUiPaintKernel &kernel,
                                                 const CancellationToken &cancellation);
        explicit UiPaintProgram(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Render
