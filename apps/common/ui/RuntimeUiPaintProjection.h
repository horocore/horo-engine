#pragma once

/** @file RuntimeUiPaintProjection.h
 * @brief Non-installed application projection of one service-admitted source and its canonical derived frame.
 */
#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "Horo/Runtime/Ui/UiElementCatalog.h"
#include "Horo/Runtime/Ui/UiTheme.h"

namespace Horo::Runtime {
    class UiAnimationSourcePresentation;
}

namespace Horo::Application::Internal {
    /** @brief Actual source-component paint; geometry, ordering, visibility and structural fill are not component overrides. */
    struct RuntimeUiPaintComponent final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::UiDrawPayload payload;
    };

    /** @brief Source-owned mask shape; bounds and ancestor relationships are derived from the admitted tree/layout. */
    struct RuntimeUiPaintMask final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::UiMaskShape shape{Runtime::Ui::UiMaskShape::Rectangle};
        std::uint32_t resource{Runtime::Ui::NoUiRenderIndex};
        std::array<std::int32_t, 4> cornerRadii{};
    };

    /** @brief Owned source-component inputs supplied with the exact canonical source admission/reload, never a document replacement.
     * @details Text/image resources originate from the actual asset/control pipeline. No serialized paint schema is introduced.
     * Isolation names a source element; its opacity comes from the canonical computed style, never this declaration.
     */
    struct RuntimeUiPaintDeclaration final {
        Runtime::Ui::UiDocumentId document;
        Runtime::Ui::UiDocumentRevision revision;
        Runtime::Ui::UiCanvasId canvas;
        std::vector<RuntimeUiPaintComponent> components;
        std::vector<RuntimeUiPaintMask> masks;
        std::vector<Runtime::Ui::UiElementId> isolated;
        std::vector<Runtime::Ui::UiTextRun> textRuns;
        std::vector<Runtime::Ui::UiPositionedGlyph> glyphs;
        std::vector<Runtime::Ui::UiRenderResourceReference> resources;
    };

    /** @brief Bounded actual source-to-extractor adapter; it owns no styles, layout, controls or mutable document.
     * @details Create is explicit source-safe-point work. Extract uses the existing computed surface projector and reserved
     * arrays. The source lease pins the actual registry entry; an independent replacement cannot reuse this declaration.
     */
    class RuntimeUiPaintProjection final {
        struct Storage;

    public:
        /** @brief Reserves extraction storage against the actual service-issued source generation.
         * @param service Canonical host-owned source service. @param instance Exact admitted instance.
         * @param declaration Owned real source components. @param descriptor Finite extraction/view policy.
         * @return Complete adapter or original source/capacity/schema error without changing service admission.
         */
        [[nodiscard]] static Result<std::unique_ptr<RuntimeUiPaintProjection>> Create(
            Runtime::Ui::RuntimeUiService &service, Runtime::Ui::RuntimeUiInstanceId instance, RuntimeUiPaintDeclaration declaration,
            const Runtime::Ui::UiRenderExtractorDescriptor &descriptor);
        /** @brief Prepares the same bounded adapter against a real private reload candidate before canonical commit.
         * @param candidate Actual generation still owned by the caller's coordinated reload preparation.
         * @param declaration Complete matching source inputs. @param descriptor Exact extraction policy.
         * @return Fully allocated private projection; first Extract must acquire this exact committed instance issuance.
         * @pre Keep the candidate alive until reload succeeds or destroy this private adapter on reload failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RuntimeUiPaintProjection>> PrepareReload(
            const Runtime::Ui::UiReloadGeneration &candidate, RuntimeUiPaintDeclaration declaration,
            const Runtime::Ui::UiRenderExtractorDescriptor &descriptor);
        /** @brief Extracts potential roles from exact privately prepared canonical source caches for native preparation only.
         * @param source Same unpublished generation used by PrepareReload, pinned by the baseline owner.
         * @param baseline Actual same-owner computed initial presentation, not runtime dispatch evidence.
         * @return Preparation-only immutable snapshot; geometry upload/submission rejects this issuance.
         */
        [[nodiscard]] Result<Runtime::Ui::UiRenderSnapshot> ExtractPrototype(const Runtime::Ui::UiReloadGeneration &source,
                                                                             const Runtime::UiAnimationSourcePresentation &baseline);
        /** @brief Borrows the immutable authored canvas from the exact source pinned by this projection.
         * @return Canonical descriptor, valid until this projection is replaced or destroyed.
         */
        [[nodiscard]] const Runtime::Ui::UiCanvasDescriptor &Canvas() const noexcept;
        ~RuntimeUiPaintProjection();
        RuntimeUiPaintProjection(const RuntimeUiPaintProjection &) = delete;
        RuntimeUiPaintProjection &operator=(const RuntimeUiPaintProjection &) = delete;
        /** @brief Extracts the current derived publication using the exact admitted source; never advances animation clocks.
         * @param service Same actual canonical publisher. @param frame Actual animation publication.
         * @return Immutable extracted frame or original stale/capacity/projection failure; no partial snapshot is issued.
         */
        [[nodiscard]] Result<Runtime::Ui::UiRenderSnapshot> Extract(Runtime::Ui::RuntimeUiService &service,
                                                                    const Runtime::Ui::UiAnimationFrameLease &frame);

    private:
        explicit RuntimeUiPaintProjection(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Application::Internal
