#pragma once

/** @file BuildReleaseModal.h
 * @brief Native menu workflow observing host-owned build and release jobs.
 */
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "editor/modals/build_release/BuildReleaseModalSession.h"

#include <initializer_list>

namespace Horo::Editor {
    /** @brief Initial workflow chosen by the application menu. */
    enum class BuildReleasePage {
        Build,
        Release
    };

    /** @brief Form and review views of the release preparation workflow. */
    enum class BuildReleaseTab {
        Configuration,
        Review
    };

    /** @brief Transient observer; closing never cancels or destroys service-owned
     * work. */
    class BuildReleaseModal final : public EditorModal {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace"};
        /** @brief Shared build/release status vocabulary, distinct from this form's workspace labels. */
        static constexpr GuiLocalizationScope kReleaseTextScope{"workspace.build_release"};

        /** @brief Borrows host services and the retained build identity; all borrows
         * outlive the modal.
         * @param context Shared theme and localization.
         * @param page Menu-selected workflow.
         * @param request Project and SDK inputs captured by the host.
         * @param builds Optional native build authority.
         * @param retainedBuild Host-owned last build identity, preserved across modal
         * closes.
         * @param releases Optional shared release job capability.
         * @param initialTab Initial form or review view.
         */
        BuildReleaseModal(const EditorGuiContext &context, BuildReleasePage page, Application::GameplayBuildRequest request,
                          Application::GameplayBuildService *builds, std::optional<std::uint64_t> &retainedBuild,
                          IBuildReleaseJobs *releases = nullptr, BuildReleaseTab initialTab = BuildReleaseTab::Configuration);
        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &) override;
        void OnUpdate(float dt) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason) override;

    private:
        /** @brief Transient form values; never treated as accepted execution or
         * capability evidence. */
        struct Draft {
            int view{}, product{}, platform{}, architecture{}, configuration{}, signing{}, protection{}, inclusion{}, packageFormat{},
                testProfile{};
            int jobs{2};
            std::string name, version, notes, minimumEngine, defines, beforeBuild, afterBuild, output, branch, symbols, endpoint;
            bool debugSymbols{true}, crashReporter{true}, allowDowngrades{}, lto{true}, stripEditor{true}, unitTests{};
            bool failUnresolved{true}, delta{}, compress{true};
        } draft_;

        /** @brief Draws observed stages, never placeholder progress. */
        void DrawStages();
        /** @brief Draws the full-width, sectioned release request form. */
        void DrawPage();
        /** @brief Draws the identity fields in responsive paired rows. */
        void DrawConfiguration();
        /** @brief Draws advanced toolchain and parallel job preferences. */
        void DrawBuildOptions();
        /** @brief Draws the immutable observation or authored draft summary. */
        void DrawSummary();
        /** @brief Draws request values for review without submitting a job. */
        void DrawReview();
        /** @brief Draws footer actions; returns true only for closing/backgrounding. */
        [[nodiscard]] bool DrawFooter();
        /** @brief Formats the current authored target, not provider capability evidence. */
        [[nodiscard]] std::string Target() const;
        /** @brief Formats the authored local candidate path preview. */
        [[nodiscard]] std::string CandidatePath() const;
        /** @brief Draws a measured shared field with its localized label. */
        void Field(const char *key, std::string &value);
        /** @brief Draws a bounded choice from a closed localized option set. */
        void Choice(const char *key, int &value, std::initializer_list<const char *> options);
        /** @brief Resolves a workspace-relative form label without constructing
         * a qualified lookup string during drawing. */
        [[nodiscard]] const std::string &Label(const char *key) const;
        /** @brief Refreshes bounded service observations without starting work. */
        void Refresh();
        /** @brief Draws shared release observations or an explicit disconnected
         * state. */
        void DrawRelease();
        /** @brief Resolves copy from the closed localization vocabulary. */
        [[nodiscard]] const std::string &Text(const char *key) const;
        const EditorGuiContext &context_;
        BuildReleasePage page_;
        Application::GameplayBuildRequest request_;
        Application::GameplayBuildService *builds_;
        std::optional<std::uint64_t> &retainedBuild_;
        std::optional<Application::GameplayBuildSnapshot> build_;
        std::optional<BuildReleaseModalSession> releases_;
        std::optional<Error> error_;
        float refreshSeconds_{};
    };
}  // namespace Horo::Editor
