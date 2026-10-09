#pragma once

/** @file UiRenderSubmission.h
 * @brief Synchronous host handoff of exact Runtime UI generations to renderer submission ownership.
 */

#include "Horo/Runtime/Render/RenderGraph.h"
#include "Horo/Runtime/Ui/UiGlyphAtlas.h"
#include "Horo/Runtime/Ui/UiImageResource.h"
#include "Horo/Runtime/Ui/UiRenderGeometry.h"
#include "Horo/Runtime/Ui/UiTextShaping.h"

#include <span>
#include <utility>

namespace Horo::Render {
    class RenderFrontend;
    inline constexpr std::size_t MaximumUiSubmissionImages = 64;
    /** @brief Maximum face/page bindings, not distinct faces; one face may span several atlas pages. */
    inline constexpr std::size_t MaximumUiSubmissionFonts = 32;
    inline constexpr std::size_t MaximumUiSubmissionGlyphs = 4'096;

    /** @brief Frontend-issued provenance for a texture created from one exact current UI image page.
     * @details Copying this value preserves provenance, not native lifetime. Submission pins its exact texture generation;
     * release and renderer restart make the handle stale. Callers cannot manufacture provenance for an arbitrary texture.
     */
    class UiRenderImageTexture final {
    public:
        /** @brief Returns native-neutral texture creation and readiness evidence. @return Exact creation operation. */
        [[nodiscard]] const ResourceCreation<RenderTextureHandle> &Creation() const noexcept {
            return creation_;
        }

        /** @brief Returns exact immutable extraction provenance copied at texture creation. @return Source reference. */
        [[nodiscard]] const Runtime::Ui::UiRenderResourceReference &Source() const noexcept {
            return source_;
        }

        /** @brief Returns the image generation realized by this texture. @return Exact image handle. */
        [[nodiscard]] Runtime::Ui::UiImageResourceHandle Image() const noexcept {
            return image_;
        }

        /** @brief Returns the source page index. @return Image page. */
        [[nodiscard]] std::uint32_t Page() const noexcept {
            return page_;
        }

        /** @brief Borrows retained exact publication ownership for synchronous source admission. @return Owned image generation. */
        [[nodiscard]] const Runtime::Ui::UiImageResourceSnapshot &SourceOwnership() const noexcept {
            return ownership_;
        }

    private:
        friend class RenderFrontend;

        UiRenderImageTexture(ResourceCreation<RenderTextureHandle> creation, Runtime::Ui::UiRenderResourceReference source,
                             Runtime::Ui::UiImageResourceSnapshot ownership, std::uint32_t page) noexcept
            : creation_(creation), source_(std::move(source)), image_(ownership.Handle()), page_(page), ownership_(std::move(ownership)) {}

        ResourceCreation<RenderTextureHandle> creation_;
        Runtime::Ui::UiRenderResourceReference source_;
        Runtime::Ui::UiImageResourceHandle image_;
        std::uint32_t page_;
        Runtime::Ui::UiImageResourceSnapshot ownership_;
    };

    /** @brief Frontend-issued exact glyph-page realization, invalidated by either UI-page or renderer generation replacement. */
    class UiRenderAtlasTexture final {
    public:
        /** @brief Returns native-neutral texture creation and readiness evidence. @return Exact creation operation. */
        [[nodiscard]] const ResourceCreation<RenderTextureHandle> &Creation() const noexcept {
            return creation_;
        }

        /** @brief Returns exact source atlas page ownership and generation. @return Atlas page. */
        [[nodiscard]] Runtime::Ui::UiGlyphAtlasPageId Page() const noexcept {
            return source_.Page();
        }

        /** @brief Returns the realized atlas revision. @return Exact atlas revision. */
        [[nodiscard]] Runtime::Ui::UiGlyphAtlasRevision Revision() const noexcept {
            return source_.Revision();
        }

        /** @brief Borrows owned source authority during synchronous admission. @return Exact atlas/page owner lease. */
        [[nodiscard]] const Runtime::Ui::UiGlyphAtlas::PageLease &Source() const noexcept {
            return source_;
        }

    private:
        friend class RenderFrontend;

        UiRenderAtlasTexture(ResourceCreation<RenderTextureHandle> creation, Runtime::Ui::UiGlyphAtlas::PageLease source) noexcept
            : creation_(creation), source_(std::move(source)) {}

        ResourceCreation<RenderTextureHandle> creation_;
        Runtime::Ui::UiGlyphAtlas::PageLease source_;
    };

    /** @brief Current image generation and exact resident graph texture resolved by the host's resource adapter. */
    struct UiRenderImageBinding final {
        const Runtime::Ui::UiImageResourceRegistry *registry{}; /**< Borrowed only during synchronous admission. */
        Runtime::Ui::UiImageResourceHandle image;  /**< Exact currently published generation, never a retained stale snapshot. */
        std::uint32_t page{};                      /**< Source page within the immutable image resource. */
        RenderGraphResourceId texture;             /**< Imported texture pinned by the same native submission. */
        const UiRenderImageTexture *realization{}; /**< Frontend-issued exact source-to-texture provenance, borrowed synchronously. */
    };

    /** @brief Immutable source face and one exact atlas-page texture containing its resolved glyphs.
     * @details Supply one binding for each page used by a face. Every snapshot glyph must match exactly one
     * binding through the sealed frame's owned face generation, page and UV placement; missing or duplicate
     * coverage is rejected before native submission. All bindings share the fixed submission capacity.
     */
    struct UiRenderFontBinding final {
        const Runtime::Ui::UiFontFace *face{};     /**< Borrowed synchronously; its owned immutable payload is retained on admission. */
        RenderGraphResourceId texture;             /**< Imported atlas texture pinned by the same native submission. */
        const UiRenderAtlasTexture *realization{}; /**< Frontend-issued page-to-texture provenance, borrowed synchronously. */
    };

    /**
     * @brief Explicit host submission handoff; all borrows end before ExecuteGraph returns.
     * @details Successful admission copies exact geometry, image and font leases into preallocated renderer storage and
     * transfers the sealed atlas pin set. Native completion, terminal loss or unsent abandonment releases those owners.
     * Present success, resize and source reload do not retire submitted leases. This adds lifetime ownership to existing
     * graph execution, not a UI draw workload or a pixel-rendering capability claim.
     * @pre UI and renderer owner operations, including completion polling, are serialized on the same host thread.
     */
    struct UiRenderSubmission final {
        const Runtime::Ui::UiRenderGeometryPlan *geometry{}; /**< Exact immutable plan, including its source snapshot lease. */
        std::span<const UiRenderImageBinding> images;        /**< At most MaximumUiSubmissionImages bindings. */
        std::span<const UiRenderFontBinding> fonts;          /**< At most MaximumUiSubmissionFonts bindings. */
        Runtime::Ui::UiGlyphAtlas::FrameLease atlas;         /**< Sealed exact pin set required whenever font bindings are present. */
    };
}  // namespace Horo::Render
