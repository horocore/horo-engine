#pragma once

/**
 * @file MetalResourceFormats.h
 * @brief Backend-private Metal texture-format translation.
 */

#include "Horo/Runtime/Render/RenderResourceDescriptors.h"

#include <cstdint>

namespace Horo::Render::Detail {
    /** @brief Returns the native Metal pixel-format value for a backend-neutral format. */
    [[nodiscard]] std::uint64_t MetalPixelFormatValue(RenderTextureFormat format) noexcept;

}  // namespace Horo::Render::Detail
