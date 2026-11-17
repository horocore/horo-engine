#include "XrSetupModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/XR/XRFeatureNegotiation.h"

#include <array>
#include <format>

namespace Horo::Editor {
    namespace {
        constexpr GuiLocalizationScope TextScope{"workspace.xr_setup"};

        /** @brief Labels the actual published test outcome. */
        const char *TestKey(const EditorXrSetupTestState test) noexcept {
            using enum EditorXrSetupTestState;
            switch (test) {
                case NotRun:
                    return "test_not_run";
                case Queued:
                    return "test_queued";
                case Passed:
                    return "test_passed";
                case Failed:
                    return "test_failed";
                case Cancelled:
                    return "test_cancelled";
            }
            return "test_failed";
        }

        /** @brief Pairs a redacted host result with recovery guidance. */
        const char *IssueKey(const EditorXrSetupIssue issue) noexcept {
            using enum EditorXrSetupIssue;
            switch (issue) {
                case None:
                    return "permissions";
                case Busy:
                    return "busy";
                case Unavailable:
                    return "unavailable";
                case Renderer:
                    return "renderer_incompatible";
                case Unsupported:
                    return "unsupported";
                case Permission:
                    return "permission_denied";
                case Stale:
                    return "stale";
                case Capacity:
                    return "capacity";
                case Invalid:
                    return "invalid";
                case Cleanup:
                    return "cleanup";
            }
            return "unavailable";
        }

        /** @brief Displays requested project policy without inferring effective capability admission. */
        const char *ProfileKey(XR::XRFeatureProfile profile) noexcept {
            switch (profile) {
                using enum XR::XRFeatureProfile;
                case Projection1_0:
                    return "profile_projection";
                case TrackedInteraction1_0:
                    return "profile_tracked";
                case Count:
                    return "profile_unavailable";
            }
            return "profile_unavailable";
        }
    }  // namespace

    /** @copydoc XrSetupModal::XrSetupModal */
    XrSetupModal::XrSetupModal(const EditorGuiContext &context, IEditorXrSetup *setup)
        : EditorModal(TextScope), context_(context), setup_(setup) {}

    ModalId XrSetupModal::Id() const {
        return ModalId{0x58525345545550ULL};
    }

    ModalPresentation XrSetupModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact};
    }

    ModalClosePolicy XrSetupModal::ClosePolicy() const {
        return {};
    }

    Result<void> XrSetupModal::OnOpen(EditorModalContext &) {
        return Result<void>::Success();
    }

    CloseDecision XrSetupModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    void XrSetupModal::OnClose(ModalCloseReason) {
        if (setup_)
            setup_->CancelSetupTest(request_);
    }

    const std::string &XrSetupModal::Text(const char *key) const {
        return LocalizedText(context_.localization, key);
    }

    ModalFrameResult XrSetupModal::Draw() {
        const auto &fonts = context_.theme.fonts;
        Ui::ScopedModalShell shell({.id = "XrSetup",
                                    .title = Text("title").c_str(),
                                    .requestedSize = {520, 380},
                                    .minimumWidth = 260,
                                    .minimumHeight = 260,
                                    .footerHeight = 108},
                                   fonts);
        const auto report = setup_ ? setup_->ReadSetup() : EditorXrSetupReport{};
        ImGui::BeginChild("##XrSetupBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        const float width = ImGui::GetContentRegionAvail().x;
        Ui::WrappedText(width, Text(report.loaderMajor ? "runtime_discovered" : "runtime_missing"), Theme::Text(), fonts);
        if (report.loaderMajor) {
            std::array<char, 17> version{};
            const auto display =
                std::format_to_n(version.data(), version.size(), "{}.{}.{}", report.loaderMajor, report.loaderMinor, report.loaderPatch);
            Ui::WrappedText(width, {version.data(), static_cast<std::size_t>(display.out - version.data())}, Theme::Text(), fonts);
        }
        Ui::WrappedText(width, Text(report.rendererReady ? "renderer_ready" : "renderer_missing"), Theme::Text(), fonts);
        if (report.requestedProfile)
            Ui::WrappedText(width, Text(ProfileKey(*report.requestedProfile)), Theme::Text(), fonts);
        Ui::WrappedText(width, Text(TestKey(report.test)), Theme::Text(), fonts);
        const auto issue = rejected_ ? EditorXrSetupIssue::Unavailable : report.issue;
        Ui::WrappedText(width, Text(IssueKey(issue)), Theme::Muted(), fonts);
        ImGui::EndChild();

        shell.BeginFooter({16, 12});
        if (Ui::Button({.label = Text("run_test").c_str(),
                        .variant = Ui::ButtonVariant::Primary,
                        .enabled = report.mayRequestTest,
                        .font = fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const auto queued = setup_->RequestSetupTest();
            rejected_ = queued.HasError();
            if (queued.HasValue())
                request_ = queued.Value();
        }
        const bool close = Ui::Button({.label = Text("close").c_str(),
                                       .variant = Ui::ButtonVariant::Secondary,
                                       .font = fonts.sans,
                                       .style = {.width = Ui::StyleWidth::FillAvailable}});
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Completed) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
