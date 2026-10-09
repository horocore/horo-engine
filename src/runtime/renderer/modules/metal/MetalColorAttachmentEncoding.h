#pragma once

/** @file MetalColorAttachmentEncoding.h
 * @brief Backend-private attachment translation shared by owner and independent worker encoding.
 */

#include "Horo/Runtime/Render/RenderBackend.h"

#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    /** @brief Maps an admitted Horo load operation; preserves the native DontCare fallback for invalid input. */
    [[nodiscard]] inline MTLLoadAction MetalLoadAction(const AttachmentLoadOperation operation) noexcept {
        switch (operation) {
            case AttachmentLoadOperation::Load:
                return MTLLoadActionLoad;
            case AttachmentLoadOperation::Clear:
                return MTLLoadActionClear;
            case AttachmentLoadOperation::DontCare:
                return MTLLoadActionDontCare;
        }
        return MTLLoadActionDontCare;
    }

    /** @brief Maps the complete Horo store contract without changing native resource ownership. */
    [[nodiscard]] inline MTLStoreAction MetalStoreAction(const AttachmentStoreOperation operation) noexcept {
        return operation == AttachmentStoreOperation::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
    }

    /** @brief Applies admitted color operations synchronously; never retains or publishes the borrowed attachment. */
    inline void ConfigureMetalColorAttachment(MTLRenderPassColorAttachmentDescriptor *attachment,
                                              const PrimaryOutputAttachment &operations) noexcept {
        attachment.loadAction = MetalLoadAction(operations.loadOperation);
        attachment.storeAction = MetalStoreAction(operations.storeOperation);
        const auto &clear = operations.clearColor;
        attachment.clearColor = MTLClearColorMake(clear.red, clear.green, clear.blue, clear.alpha);
    }
}  // namespace Horo::Render::Detail
