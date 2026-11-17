#pragma once
/** @file XrSetupModal.h @brief Localized projection of the application-owned XR setup workflow. */

#include "EditorPlayXrHost.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"

namespace Horo::Editor {
    /** @brief Shows copied setup evidence and submits one cold session-test command. */
    class XrSetupModal final : public EditorModal {
    public:
        /** @brief Borrows the setup port from the application host, which outlives this modal. */
        XrSetupModal(const EditorGuiContext &context, IEditorXrSetup *setup);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;
        void OnClose(ModalCloseReason) override;

    private:
        [[nodiscard]] const std::string &Text(const char *key) const;
        const EditorGuiContext &context_;
        IEditorXrSetup *setup_;
        EditorXrSetupTestId request_;
        bool rejected_{};
    };
}  // namespace Horo::Editor
