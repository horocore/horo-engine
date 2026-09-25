#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "editor/modals/build/BuildWorkflowPreviewState.h"

#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Editor-only preview of one build, test, release, or publish request. */
    class BuildWorkflowPreviewModal final : public EditorModal {
    public:
        BuildWorkflowPreviewModal(const EditorGuiContext &context, BuildWorkflowPreviewState &state, BuildPreviewKind kind);

        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;

    private:
        [[nodiscard]] std::string Label(const char *key) const;
        void DrawForm();
        void DrawPipeline() const;
        void DrawSummary() const;
        void DrawBuildForm();
        void DrawTestForm();
        void DrawReleaseForm();
        void DrawPublishForm();
        void DrawReview();
        void DrawReleaseReview() const;
        void DrawBuildReviewRows();
        void DrawTestReviewRows();
        void DrawPublishReviewRows();
        void DrawActivity() const;
        void DrawActivityNotice(const BuildPreviewJob &job) const;
        void DrawActivityLog(const BuildPreviewJob &job) const;
        void DrawFooter();
        void DrawActivityFooter(const BuildPreviewJob &job);
        void DrawRequestFooter();
        void DrawTextField(const char *labelKey, const char *id, std::string &value, const char *hintKey = nullptr) const;
        void DrawTextPair(const char *tableId, const char *leftLabelKey, const char *leftId, std::string &leftValue,
                          const char *rightLabelKey, const char *rightId, std::string &rightValue) const;
        void DrawComboField(const char *labelKey, const char *id, int &value, const char *const *keys, int count) const;
        void DrawReviewRow(const char *labelKey, const std::string &value);
        [[nodiscard]] bool CanSubmit() const;
        [[nodiscard]] const char *PrimaryActionKey() const noexcept;
        [[nodiscard]] std::string BuildTargetLabel() const;
        [[nodiscard]] std::string ReleaseTargetLabel() const;
        [[nodiscard]] std::string RequestOutput() const;
        [[nodiscard]] std::string ReleaseCandidatePath() const;
        [[nodiscard]] const char *ActivityResultKey() const noexcept;
        void Submit();

        const EditorGuiContext &context_;
        BuildWorkflowPreviewState &state_;
        BuildPreviewKind kind_;
        enum class Page {
            Form,
            Review,
            Activity
        } page_{Page::Form};
        int profile_{0};
        int buildProfile_{0};
        int platform_{0};
        int architecture_{0};
        int configuration_{0};
        int mode_{0};
        int content_{0};
        int testSource_{0};
        int testProfile_{0};
        int package_{0};
        int signing_{0};
        int protection_{0};
        int toolchain_{0};
        int channel_{0};
        int visibility_{0};
        int credential_{0};
        int destinationIndex_{1};
        bool testAfterBuild_{false};
        bool runAfterBuild_{false};
        bool showProfiles_{false};
        bool showAdvanced_{false};
        bool hasSavedProfile_{false};
        bool closeRequested_{false};
        std::string name_{"DesertRun"};
        std::string version_{"0.4.2"};
        std::string source_{"v0.4.2"};
        std::string notes_{"docs/releases/0.4.2.md"};
        std::string output_{"releases/0.4.2_windows_x86_64_shipping/"};
        std::string packagePath_{"builds/latest/"};
        std::string destination_{"Steam"};
        std::string testFilter_;
        std::string timeout_{"120"};
        std::string parallelJobs_{"16"};
        std::string profileName_;
        std::string savedProfileName_;
        std::vector<Ui::TableRow> reviewRows_;
    };
}  // namespace Horo::Editor
