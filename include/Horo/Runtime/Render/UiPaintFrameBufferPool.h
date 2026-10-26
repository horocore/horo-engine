#pragma once
/** @file UiPaintFrameBufferPool.h
 * @brief Finite preparation-owned resident UI frame slots with completion-aware updates.
 */
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/UiPaintFrameUpload.h"

#include <thread>

namespace Horo::Render {
    class UiPaintProgram;

    /** @brief Fixed complete slot capacity admitted before any frame is opened. */
    struct UiPaintFrameBudget final {
        std::uint32_t vertices{}, indices{}, parameters{};
        /** @brief Checks positive counts against the portable bounded paint ABI. @return Whether all counts are representable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact successfully uploaded resident slot and its immutable publication revision. */
    struct UploadedUiPaintFrame final {
        UiPaintFrameBuffers buffers;
        std::uint64_t revision{};
    };

    /**
     * @brief Exclusive render-owner pool; native buffers are allocated once during preparation.
     * @details Updates borrow CPU views synchronously and never wait;
     * an in-use native slot returns Pending without advancing its revision. Host teardown closes new
     * updates, destroys graph borrowers, then releases this pool before the frontend. GPU completion
     * independently retains released buffer generations. If the frontend is destroyed first, its
     * revoked private marker rejects further updates and pool cleanup releases metadata only.
     * Creation, updates and destruction are serial on the same render thread; foreign operations
     * reject before reading owner state. No callback or job borrows the pool.
     */
    class UiPaintFrameBufferPool final {
        class ConstructionKey {
            ConstructionKey() noexcept = default;
            friend class UiPaintFrameBufferPool;
        };

    public:
        /** @brief Initializes metadata; Create owns the sole construction key.
         * @param frontend Selected renderer that outlives the pool. @param budget Validated fixed capacity.
         * @param slots Validated device-bounded slot count. @param key Factory-only construction authority. */
        UiPaintFrameBufferPool(const RenderFrontend &frontend, UiPaintFrameBudget budget, std::uint32_t slots, ConstructionKey key);
        /** @brief Allocates three host-visible buffers per finite slot at the render preparation safe point.
         * @param frontend Initialized selected renderer, with no frame active. @param budget Fixed complete geometry/uniform capacity.
         * @param slots Explicit count bounded by eight and device frames in flight.
         * @return Owned pool or typed failure; partially created generations are released through the same frontend. */
        [[nodiscard]] static Result<std::unique_ptr<UiPaintFrameBufferPool>> Create(RenderFrontend &frontend, UiPaintFrameBudget budget,
                                                                                    std::uint32_t slots);
        /** @brief Releases residents on the creating thread after the host has quiesced callers. */
        ~UiPaintFrameBufferPool();
        UiPaintFrameBufferPool(const UiPaintFrameBufferPool &) = delete;
        UiPaintFrameBufferPool &operator=(const UiPaintFrameBufferPool &) = delete;

        /** @brief Publishes one complete candidate to a free exact native slot without allocation on success.
         * @param slot Explicit admitted slot index. @param vertices Complete finite vertices. @param indices Complete valid indices.
         * @param parameters Complete ABI uniform records. @param cancellation Observed before native writes.
         * @return Exact accepted buffers/revision or original typed error; failure preserves the previous revision. */
        [[nodiscard]] Result<UploadedUiPaintFrame> Update(std::uint32_t slot, std::span<const UiPaintVertex> vertices,
                                                          std::span<const std::uint32_t> indices,
                                                          std::span<const UiPaintParameters> parameters,
                                                          const CancellationToken &cancellation = {});
        /** @brief Closes admission and releases owned handles on the creating thread; repeated close is safe.
         * @return Original release failure retaining unreleased handles for retry, or success. Wrong-thread calls leave admission
         * unchanged. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        friend class UiPaintProgram;
        std::weak_ptr<Detail::UiPaintFrontendState> frontendState_;
        std::shared_ptr<Detail::UiPaintOwnerRetirement> retirement_;
        UiPaintFrameBudget budget_;
        std::uint32_t count_;
        std::thread::id owner_;
        std::array<UiPaintFrameBuffers, 8> slots_{};
        std::array<std::array<ResourceOperationId, 3>, 8> operations_{};
        std::array<std::uint64_t, 8> revisions_{};
        bool closed_{};
    };
}  // namespace Horo::Render
