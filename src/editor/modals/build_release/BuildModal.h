#pragma once

/** @file BuildModal.h
 * @brief Project build request and activity views matching Mock Studio's Build workflow.
 */
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"

#include <initializer_list>

namespace Horo::Editor {
    /** @brief Transient build presentation; the application service owns every job. */
    class BuildModal final : public EditorModal {
    public:
        /** @brief Borrows host capabilities and its retained build ID; all borrows outlive the modal. */
        BuildModal(const EditorGuiContext &context, Application::GameplayBuildRequest request, Application::GameplayBuildService *builds,
                   std::optional<std::uint64_t> &retainedBuild, const IBuildOutputQuery *output = nullptr);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        void OnUpdate(float dt) override;
        [[nodiscard]] ModalFrameResult Draw() override;

    private:
        [[nodiscard]] const std::string &Text(const char *key) const;
        void Refresh();
        void DrawSummary();
        void DrawForm();
        void DrawActivity();
        void DrawStages();
        [[nodiscard]] bool DrawFooter();
        void Choice(const char *key, int &value, std::initializer_list<const char *> options);
        void DrawTargetRow(bool productRow);
        [[nodiscard]] bool Supported() const;
        [[nodiscard]] bool Running() const;
        [[nodiscard]] const char *Status() const;

        const EditorGuiContext &context_;
        Application::GameplayBuildRequest request_;
        Application::GameplayBuildService *builds_;
        std::optional<std::uint64_t> &retainedBuild_;
        const IBuildOutputQuery *output_;
        std::optional<Application::GameplayBuildSnapshot> build_;
        BuildOutputSnapshot outputSnapshot_;
        std::optional<Error> error_;
        bool showDraft_{true};
        bool testAfter_{}, runAfter_{};
        int profile_{}, product_{}, workflow_{}, platform_{}, architecture_{}, configuration_{}, content_{};
        float refreshSeconds_{};
    };
}  // namespace Horo::Editor
