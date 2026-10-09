#pragma once

/** @file UiRenderSubmissionRetention.h
 * @brief Preallocated exact UI-generation ownership attached to one native graph submission.
 */

#include "Horo/Runtime/Render/UiRenderSubmission.h"

#include <array>
#include <optional>

namespace Horo::Render::Detail {
    /** @brief Owner-thread retained sources; never holds a borrowed registry, face or geometry pointer. */
    class UiRenderSubmissionRetention final {
    public:
        /** @brief Validates bounded current source evidence and captures owned leases, rolling back on failure.
         * @param submission Synchronously borrowed inputs and transferable sealed atlas frame.
         * @param resources Exact imported resources pinned by this graph's native submission.
         * @return Success or an original typed stale, lifecycle, capacity or provenance failure.
         */
        [[nodiscard]] Result<void> Capture(UiRenderSubmission &submission, std::span<const RenderGraphResource> resources);
        /** @brief Releases all retained source generations and sealed atlas pins exactly once. */
        void Release() noexcept;

    private:
        /** @brief Captures and validates all source coverage; caller releases a partial prefix on failure. */
        [[nodiscard]] Result<void> CaptureSources(const UiRenderSubmission &submission, std::span<const RenderGraphResource> resources);
        /** @brief Captures one current image generation after exact page/reference/graph validation. */
        [[nodiscard]] Result<void> CaptureImage(const UiRenderImageBinding &binding, const Runtime::Ui::UiRenderSnapshot &snapshot,
                                                std::span<const RenderGraphResource> resources, std::size_t index);
        /** @brief Captures one immutable font generation after exact source/reference/graph validation. */
        [[nodiscard]] Result<void> CaptureFont(const UiRenderFontBinding &binding, const Runtime::Ui::UiRenderSnapshot &snapshot,
                                               std::span<const RenderGraphResource> resources,
                                               const Runtime::Ui::UiGlyphAtlas::FrameLease &atlas, std::size_t index);
        /** @brief Requires source ownership for every reference; font references may have multiple exact page bindings. */
        [[nodiscard]] Result<void> ValidateCoverage(const Runtime::Ui::UiRenderSnapshot &snapshot) const;
        /** @brief Counts owned exact image pages and font generations for one immutable reference. */
        [[nodiscard]] std::size_t CountSourceMatches(const Runtime::Ui::UiRenderResourceReference &reference) const;
        /** @brief Requires every sampled glyph to match exactly one retained page realization and owned font generation. */
        [[nodiscard]] Result<void> ValidateGlyphCoverage(const Runtime::Ui::UiRenderSnapshot &snapshot,
                                                         const Runtime::Ui::UiGlyphAtlas::FrameLease &atlas) const;

        std::optional<Runtime::Ui::UiRenderGeometryPlan> geometry_;
        std::array<std::optional<Runtime::Ui::UiImageResourceSnapshot>, MaximumUiSubmissionImages> images_;
        std::array<std::uint32_t, MaximumUiSubmissionImages> pages_{};
        std::array<std::optional<Runtime::Ui::UiFontFace>, MaximumUiSubmissionFonts> fonts_;
        std::array<Runtime::Ui::UiGlyphAtlasPageId, MaximumUiSubmissionFonts> fontPages_{};
        Runtime::Ui::UiGlyphAtlas::FrameLease atlas_;
    };
}  // namespace Horo::Render::Detail
