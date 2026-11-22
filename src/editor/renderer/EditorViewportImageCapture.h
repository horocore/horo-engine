#pragma once

/** @file EditorViewportImageCapture.h @brief Detached, bounded editor viewport image evidence. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Editor {
    /** @brief A pixel rectangle in the offscreen viewport, with a top-left origin. */
    struct EditorViewportImageRegion {
        static constexpr std::uint32_t MaximumDimension = 256;
        static constexpr std::size_t Channels = 4;
        std::uint32_t x{};
        std::uint32_t y{};
        std::uint32_t width{};
        std::uint32_t height{};

        /** @brief Checks the resolution budget and exact source extent without overflowing offsets. */
        [[nodiscard]] constexpr bool Fits(std::uint32_t sourceWidth, std::uint32_t sourceHeight) const noexcept {
            return width > 0 && height > 0 && width <= MaximumDimension && height <= MaximumDimension && x <= sourceWidth &&
                   y <= sourceHeight && width <= sourceWidth - x && height <= sourceHeight - y;
        }

        /** @brief Size of a tightly packed RGBA8 copy after region admission. */
        [[nodiscard]] constexpr std::size_t Bytes() const noexcept {
            return std::size_t{width} * height * Channels;
        }
    };

    enum class EditorViewportImagePhase {
        Pending,
        Ready,
        Cancelled,
        Resized,
        Failed,
        Expired
    };

    /** @brief CPU-owned pixels from exactly one completed viewport copy, in top-to-bottom row order. */
    struct EditorViewportImageArtifact {
        EditorViewportImageRegion region;
        std::vector<std::byte> rgba;
    };

    /** @brief Observation of one request, with detached pixels only when ready. */
    struct EditorViewportImageSnapshot {
        EditorViewportImagePhase phase;
        std::shared_ptr<const EditorViewportImageArtifact> artifact;
    };
}  // namespace Horo::Editor
