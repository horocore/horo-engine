#pragma once

#include "MetalBackendInternal.h"

#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    /** @brief Queries native admission facts without creating presentation or publishing a backend. */
    [[nodiscard]] MetalDeviceFacts QueryMetalDeviceFacts(id<MTLDevice> device, std::uint64_t revision, bool queueAvailable);
    /** @brief Selects only the host-requested adapter and reports the exact discovery generation. */
    [[nodiscard]] id<MTLDevice> FindMetalDevice(const std::optional<RenderAdapterId> &requested, std::uint64_t &revision);
}  // namespace Horo::Render::Detail
