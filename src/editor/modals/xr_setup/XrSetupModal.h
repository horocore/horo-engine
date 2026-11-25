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
        /** @brief Shows measured rates and ordered view dimensions from the last admitted Setup test. */
        void DrawDisplayObservation(const EditorXrDisplayObservation &display, float width) const;
        /** @brief Draws the owned redacted inspection report. */
        void DrawInspection(const EditorXrInspectionSnapshot &report) const;
        /** @brief Projects and submits one explicit preview request without invoking native output. */
        void DrawHapticPreview(IEditorXrHapticPreview &preview);
        /** @brief Shows consent controls and current redacted pointing evidence. */
        void DrawPlaneCapturePreview(IEditorXrPlaneCapturePreview &preview);
        void DrawSpatialAnchorPreview(IEditorXrSpatialAnchorPreview &preview);
        void DrawAnchorStorageConsent(IEditorXrAnchorStorageConsent &consent);
        void DrawEyeGazePreview(IEditorXrEyeGazePreview &preview);
        /** @brief Draws next-Play passthrough consent controls without exposing provider data. */
        void DrawPassthroughPreview(IEditorXrPassthroughPreview &preview);
        [[nodiscard]] const std::string &Text(const char *key) const;
        const EditorGuiContext &context_;
        IEditorXrSetup *setup_;
        EditorXrSetupTestId request_;
        EditorXrHapticPreviewId previewRequest_;
        bool previewRejected_{};
        bool planesRejected_{};
        bool spatialAnchorRejected_{};
        bool anchorStorageRejected_{};
        bool gazeRejected_{};
        bool passthroughRejected_{};
        bool rejected_{};
    };
}  // namespace Horo::Editor
