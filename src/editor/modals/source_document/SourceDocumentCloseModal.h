#pragma once

/** @file SourceDocumentCloseModal.h @brief Exclusive revision-fenced close presentation for one source document. */

#include "Horo/Editor/BlackboardSchemaDocument.h"
#include "Horo/Editor/DesignSystem/DesignTokens.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/UiCanvasDocument.h"
#include "editor/screens/workspace/ai/decisions/panels/DecisionStateMachineAuthoring.h"
#include "editor/screens/workspace/ai/queries/EnvironmentQueryDocumentFence.h"

namespace Horo::Editor {
    class EditorWorkspaceController;

    /** @brief Owns the close choice, while the controller owns saving and source lifetime.
     * @details All calls belong to the GUI owner thread. The workspace revokes the weak lifetime before controller destruction. */
    class SourceDocumentCloseModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.documents"};

        /** @brief Captures an exact source revision and borrowed controller guarded by its workspace lifetime.
         * @param context Shared GUI services. @param controller Source lifecycle owner.
         * @param lifetime Revocable workspace lifetime. @param request Frozen source revision.
         * @param source Validated source identity shown to the user. */
        SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller, std::weak_ptr<const void> lifetime,
                                 SourceSaveRequest request, std::string source);
        /** @brief Captures a canvas owner state using the same workspace close presentation. */
        SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller, std::weak_ptr<const void> lifetime,
                                 UiCanvasDocumentStateId request, std::string source);
        /** @brief Captures an exact Blackboard schema revision for the shared close workflow. */
        SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller, std::weak_ptr<const void> lifetime,
                                 BlackboardDocumentFence request, std::string source);
        /** @brief Captures an exact Decision source revision for the shared close workflow. */
        SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller, std::weak_ptr<const void> lifetime,
                                 DecisionGraphDocumentFence request, std::string source);
        /** @brief Captures an exact authored Query revision for the shared close workflow. */
        SourceDocumentCloseModal(const EditorGuiContext &context, EditorWorkspaceController &controller, std::weak_ptr<const void> lifetime,
                                 EnvironmentQueryDocumentFence request, std::string source);
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
        std::variant<SourceSaveRequest, UiCanvasDocumentStateId, BlackboardDocumentFence, DecisionGraphDocumentFence,
                     EnvironmentQueryDocumentFence>
            request_;
        std::string source_;
        bool failed_{};
    };
}  // namespace Horo::Editor
