#pragma once
/** @file UiPaintFrameUpload.h @brief Finite reusable UI vertex/index/uniform ring updates at the owning render safe point. */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/UiPaintWorkload.h"

namespace Horo::Render {
    /** @brief Stable outcomes of complete reusable UI slot admission, owned by RenderApi. */
    namespace UiPaintFrameErrors {
        extern const ErrorCodeDescriptor InvalidInput; /**< Malformed payload, stale revision or incompatible residents. */
        extern const ErrorCodeDescriptor Pending;      /**< A prior native submission still retains the slot. */
        extern const ErrorCodeDescriptor Cancelled;    /**< Cancellation observed before any mapped write. */
        extern const ErrorCodeDescriptor Unavailable;  /**< The selected native paint owner is unavailable. */
    }  // namespace UiPaintFrameErrors

    /** @brief Three distinct ready host-visible residents allocated once for one bounded native frame slot. */
    struct UiPaintFrameBuffers final {
        RenderBufferHandle vertices;
        RenderBufferHandle indices;
        RenderBufferHandle parameters;
    };

    /** @brief Synchronously borrowed complete frame payload; no native owner retains the borrowed CPU views. */
    struct UiPaintFrameUpdate final {
        static constexpr std::size_t ParameterStride = 256; /**< Fixed native uniform offsets; unsupported alignment is rejected. */
        std::uint64_t revision{};                           /**< Positive strictly advancing revision of these exact three residents. */
        std::span<const UiPaintVertex> vertices;
        std::span<const std::uint32_t> indices;
        std::span<const UiPaintParameters> parameters;
        CancellationToken cancellation;
    };

    /** @brief Exact frontend-resolved native identities; each backend independently proves its resource namespace and GPU use. */
    struct NativeUiPaintFrameUpdate final {
        std::array<std::uint64_t, 3> instances{};
        UiPaintFrameUpdate update;
    };

    /** @brief Validates bounded complete finite geometry and parameter ABI before any native write.
     * @param update Immutable borrowed data for one free reusable slot.
     * @return Success without mutation, allocation, or waiting; typed UiPaintFrameErrors::InvalidInput failure.
     * Error construction may allocate. */
    [[nodiscard]] Result<void> ValidateUiPaintFrameUpdate(const UiPaintFrameUpdate &update);
}  // namespace Horo::Render
