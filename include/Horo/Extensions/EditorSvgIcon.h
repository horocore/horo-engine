#pragma once
/** @file EditorSvgIcon.h @brief Bounded renderer-neutral rasterization of packaged static SVG activity icons. */
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Owned tightly packed straight-alpha RGBA8 pixels; it carries no native renderer resource. */
    struct EditorSvgIcon final {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> pixels;
    };

    /**
     * @brief Validates and rasterizes the bounded static-vector SVG icon profile outside frame traversal.
     * @param svg Owned-by-caller UTF-8 SVG bytes, at most 64 KiB; external resources and active content are rejected.
     * @return A fixed 48 by 48 RGBA icon, or a typed descriptor rejection; no file, network, font or GPU access occurs.
     * @note Supports svg/g/defs/path/rect/circle/ellipse/line/polyline/polygon and local gradients only.
     */
    [[nodiscard]] Result<EditorSvgIcon> RasterizeEditorSvgIcon(std::string_view svg);
}  // namespace Horo::Extensions
