#pragma once

#include "RuntimeUiPaintProjection.h"

namespace Horo::Application::Internal {
    namespace PaintProjectionDetail {
        using namespace Runtime::Ui;

        /** @brief Borrowed same-owner computed inputs for both private preparation and actual frame extraction. */
        struct Presentation final {
            const UiComputedStyleSnapshot &styles;
            const UiLayoutSnapshot &layout;
            const UiLayoutClipSnapshot *clipping{};

            [[nodiscard]] const UiComputedStyleSnapshot &Styles() const noexcept {
                return styles;
            }

            [[nodiscard]] const UiLayoutSnapshot &Layout() const noexcept {
                return layout;
            }

            [[nodiscard]] const UiLayoutClipSnapshot *Clipping() const noexcept {
                return clipping;
            }
        };

        /** @brief Load-time resolved source row; all pointers are pinned by the containing exact generation lease. */
        struct Row final {
            const UiDocumentElement *source{};
            UiElementHandle element;
            std::uint32_t parent{NoUiRenderIndex};
            std::uint32_t end{};
            std::uint32_t component{NoUiRenderIndex};
            std::uint32_t authoredMask{NoUiRenderIndex};
            bool isolate{};
            std::uint32_t firstCommand{};
            std::uint32_t mask{NoUiRenderIndex};
            std::uint32_t group{NoUiRenderIndex};
        };
    }  // namespace PaintProjectionDetail

    /** @brief Application-private reserved projection inventory; no second canonical publisher or renderer state. */
    struct RuntimeUiPaintProjection::Storage final {
        Runtime::Ui::UiReloadLease source;
        const Runtime::Ui::UiRuntimeInstance *expectedInstance{};
        Runtime::Ui::RuntimeUiInstanceId instance;
        const Runtime::Ui::UiReloadGeneration *candidate{};
        const Runtime::Ui::UiReloadCanvas *canvas{};
        const Runtime::Ui::UiCanvasDescriptor *authored{};
        RuntimeUiPaintDeclaration declaration;
        Runtime::Ui::UiRenderExtractor extractor;
        Runtime::Ui::UiRenderExtractorDescriptor descriptor;
        std::vector<PaintProjectionDetail::Row> rows;
        std::vector<Runtime::Ui::UiDrawCommand> commands;
        std::vector<Runtime::Ui::UiClip> clips;
        std::vector<Runtime::Ui::UiMask> masks;
        std::vector<Runtime::Ui::UiOpacityGroup> groups;
        std::vector<Runtime::Ui::UiLogicalTransform> transforms;
        std::uint64_t revision{};

        explicit Storage(Runtime::Ui::UiReloadLease pin, RuntimeUiPaintDeclaration input, Runtime::Ui::UiRenderExtractor render,
                         Runtime::Ui::UiRenderExtractorDescriptor policy)
            : source(std::move(pin)), declaration(std::move(input)), extractor(std::move(render)), descriptor(policy) {}

        [[nodiscard]] Result<void> AdmitRows();
        [[nodiscard]] Result<void> Build(const PaintProjectionDetail::Presentation &frame);
        [[nodiscard]] Result<void> PaintRow(std::size_t index, const PaintProjectionDetail::Presentation &frame);
        [[nodiscard]] Result<void> BeginScopes(std::size_t index, const Runtime::Ui::UiDrawCommand &paint);
        [[nodiscard]] Result<void> CloseGroups();
    };
}  // namespace Horo::Application::Internal
