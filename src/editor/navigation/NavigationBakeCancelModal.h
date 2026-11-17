#pragma once
/** @file NavigationBakeCancelModal.h
 * @brief Explicit keep-running/cancel/return decision for an exact accepted bake.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"

namespace Horo::Editor {
    /** @brief Transient cancellation decision; closing always keeps the accepted operation running. */
    class NavigationBakeCancelModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.navigation_bake"};

        /** @brief Captures the exact operation rather than a mutable latest-operation selector.
         * @param context Shared GUI services. @param decision Owned exact-operation action.
         */
        NavigationBakeCancelModal(const EditorGuiContext &context, NavigationBakeCancelDecision decision);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;

    private:
        const EditorGuiContext &context_;
        NavigationBakeCancelDecision decision_;
        std::optional<Error> error_;
    };
}  // namespace Horo::Editor
