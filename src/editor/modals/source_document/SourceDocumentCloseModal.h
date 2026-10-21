#pragma once

/** @file SourceDocumentCloseModal.h @brief Exclusive revision-fenced close presentation for one source document. */

#include "Horo/Editor/DesignSystem/DesignTokens.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/SourceFileOpenService.h"

namespace Horo::Editor {
    class EditorWorkspaceController;

    /** @brief Owns the close choice, while the controller owns saving and source lifetime.
     * @details All calls belong to the GUI owner thread. The workspace revokes the weak lifetime before controller destruction. */
    class SourceDocumentCloseModal final : public EditorModal {
    public:
        /** @brief Captures an exact source revision and borrowed controller guarded by its workspace lifetime.
         * @param context Shared GUI services. @param controller Source lifecycle owner.
         * @param lifetime Revocable workspace lifetime. @param request Frozen source revision.
         * @param source Validated source identity shown to the user. */
        SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller, std::weak_ptr<const void> lifetime,
                                 SourceSaveRequest request, std::string source);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        /** @brief Applies one explicit close choice through the captured owner, preserving failed or stale documents.
         * @param decision Save, discard, or cancel the workflow without closing the document.
         * @return True when the modal may close; typed failure keeps it open and displays recovery copy.
         * @note Cancel needs no owner access. Save and discard never refresh the captured revision implicitly. */
        [[nodiscard]] Result<bool> Resolve(SourceCloseDecision decision);
        /** @brief Measures footer chrome in logical units before the modal shell applies display scaling.
         * @param tokens Valid resolved theme metrics. @return Logical height for all three close choices and padding. */
        [[nodiscard]] static float LogicalFooterHeight(const DesignSystem::DesignTokens &tokens) noexcept;

    private:
        [[nodiscard]] const std::string &Text(const char *suffix) const;
        const EditorGuiContext &context_;
        EditorWorkspaceController *controller_;
        std::weak_ptr<const void> lifetime_;
        SourceSaveRequest request_;
        std::string source_;
        bool failed_{};
    };
}  // namespace Horo::Editor
