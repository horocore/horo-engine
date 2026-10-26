#include "editor/modals/build_release/BuildReleaseModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/design_system/components/EditorBusyIndicator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::array PlatformKeys{"workspace.build_release.windows", "workspace.build_release.macos",
                                          "workspace.build_release.linux"};
        constexpr std::array ArchitectureKeys{"workspace.build_release.x64", "workspace.build_release.arm64"};
        constexpr std::array ConfigurationKeys{"workspace.build_release.shipping", "workspace.build_release.development",
                                               "workspace.build_release.test_config"};
        constexpr std::array ProductKeys{"workspace.build_release.game_runtime",
                                         "workspace.build.dedicated_server",
                                         "workspace.build_release.engine_editor",
                                         "workspace.build.engine_cli",
                                         "workspace.build.sdk",
                                         "workspace.build_release.diagnostics"};
        constexpr std::array PackageKeys{"workspace.prepare_release.portable", "workspace.prepare_release.installer",
                                         "workspace.prepare_release.store_package"};
        constexpr std::array ContentKeys{"workspace.build.all_content", "workspace.build.selected_content",
                                         "workspace.prepare_release.base_content"};
        constexpr std::array TestKeys{"workspace.prepare_release.smoke", "workspace.prepare_release.fast_smoke",
                                      "workspace.prepare_release.full_smoke"};
        constexpr std::array StageKeys{"stage_validate",  "stage_configure", "stage_build",    "stage_cook",   "stage_package",
                                       "stage_preverify", "stage_sign",      "stage_finalize", "stage_verify", "stage_publish"};

        /** @brief Only provider-owned queued or active states keep the job running. */
        bool Running(const Release::ReleaseJobSnapshot *job) {
            return job && (job->state == Release::ReleaseJobState::Queued || job->state == Release::ReleaseJobState::Running ||
                           job->state == Release::ReleaseJobState::Cancelling);
        }

        /** @brief Draws a value while preserving the field's stable widget identity. */
        void Input(const char *key, std::string &value, const Theme::Fonts &fonts) {
            ImGui::PushID(key);
            (void)Ui::InputTextControl("##value", value, 4096, fonts);
            ImGui::PopID();
        }

        /** @brief Resolves choice captions through the editor's localization authority. */
        void Select(const EditorGuiContext &context, const char *key, int &value, const std::initializer_list<const char *> options) {
            std::array<const char *, 8> labels{};
            std::size_t count{};
            for (const auto *option : options)
                labels[count++] = context.localization.Get("editor", option).c_str();
            ImGui::PushID(key);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            (void)Ui::ComboControl("##value", &value, labels.data(), static_cast<int>(count), context.theme.fonts);
            ImGui::PopID();
        }

        /** @brief Separate label and control rows prevent one input's baseline from shifting the next label. */
        template <class Left, class Right>
        void Pair(const char *id, const EditorGuiContext &context, const char *leftKey, const char *rightKey, Left left, Right right) {
            const auto label = [&](const char *key) {
                Ui::FieldLabel(context.localization.Get("editor", key).c_str(), context.theme.fonts);
            };
            if (ImGui::GetContentRegionAvail().x < 500) {
                label(leftKey);
                left();
                label(rightKey);
                right();
                return;
            }
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX))
                return;
            ImGui::TableNextColumn();
            label(leftKey);
            ImGui::TableNextColumn();
            label(rightKey);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            left();
            ImGui::TableNextColumn();
            right();
            ImGui::EndTable();
        }

        /** @brief Provides a bounded notice using the shared text and theme authority. */
        void Notice(const char *id, const char *text, const Theme::Fonts &fonts) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16, 12});
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
            ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const auto start = ImGui::GetWindowPos();
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text, Theme::Muted(), fonts);
            ImGui::GetWindowDrawList()->AddRectFilled(start, {start.x + 3, start.y + ImGui::GetWindowSize().y}, Theme::U32(Theme::Accent()),
                                                      2);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }
    }  // namespace

    BuildReleaseModal::BuildReleaseModal(const EditorGuiContext &context, const BuildReleasePage page,
                                         Application::GameplayBuildRequest request, Application::GameplayBuildService *builds,
                                         std::optional<std::uint64_t> &retainedBuild, IBuildReleaseJobs *releases,
                                         const BuildReleaseTab initialTab)
        : context_(context), page_(page), request_(std::move(request)), builds_(builds), retainedBuild_(retainedBuild) {
        draft_.view = static_cast<int>(initialTab);
        if (releases)
            releases_.emplace(*releases);
    }

    ModalId BuildReleaseModal::Id() const {
        return ModalId{0x4255494C4452454CULL};
    }

    ModalPresentation BuildReleaseModal::Presentation() const {
        return {.size = ModalSizePolicy::Large};
    }

    ModalClosePolicy BuildReleaseModal::ClosePolicy() const {
        return {};
    }

    CloseDecision BuildReleaseModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    Result<void> BuildReleaseModal::OnOpen(EditorModalContext &) {
        if (releases_)
            releases_->Open();
        draft_.name = request_.projectRoot.filename().string();
        draft_.output = "releases";
#if defined(__APPLE__)
        draft_.platform = 1;
#elif !defined(_WIN32)
        draft_.platform = 2;
#endif
        Refresh();
        return Result<void>::Success();
    }

    /** @copydoc BuildReleaseModal::Refresh */
    void BuildReleaseModal::Refresh() {
        if (releases_)
            releases_->Refresh();
    }

    void BuildReleaseModal::OnUpdate(const float dt) {
        if (!std::isfinite(dt) || dt <= 0)
            return;
        refreshSeconds_ += dt;
        if (refreshSeconds_ >= 0.25F) {
            refreshSeconds_ = 0;
            Refresh();
        }
    }

    /** @copydoc BuildReleaseModal::Text */
    const std::string &BuildReleaseModal::Text(const char *key) const {
        return context_.localization.Get("editor", std::string{"workspace.build_release."} + key);
    }

    /** @copydoc BuildReleaseModal::Label */
    const std::string &BuildReleaseModal::Label(const char *key) const {
        return context_.localization.Get("editor", key);
    }

    /** @copydoc BuildReleaseModal::Field */
    void BuildReleaseModal::Field(const char *key, std::string &value) {
        Ui::FieldLabel(Label(key).c_str(), context_.theme.fonts);
        Input(key, value, context_.theme.fonts);
    }

    /** @copydoc BuildReleaseModal::Choice */
    void BuildReleaseModal::Choice(const char *key, int &value, const std::initializer_list<const char *> options) {
        Ui::FieldLabel(Label(key).c_str(), context_.theme.fonts);
        Select(context_, key, value, options);
    }

    /** @copydoc BuildReleaseModal::Target */
    std::string BuildReleaseModal::Target() const {
        return Label(PlatformKeys[draft_.platform]) + " · " + Label(ArchitectureKeys[draft_.architecture]) + " · " +
               Label(ConfigurationKeys[draft_.configuration]);
    }

    /** @copydoc BuildReleaseModal::CandidatePath */
    std::string BuildReleaseModal::CandidatePath() const {
        if (draft_.version.empty() || draft_.output.empty())
            return Label("workspace.prepare_release.path_pending");
        constexpr std::array platforms{"windows", "macos", "linux"};
        constexpr std::array architectures{"x86_64", "arm64"};
        constexpr std::array configurations{"shipping", "development", "test"};
        return (std::filesystem::path{draft_.output} / (draft_.version + "_" + platforms[draft_.platform] + "_" +
                                                        architectures[draft_.architecture] + "_" + configurations[draft_.configuration]))
                   .generic_string() +
               "/";
    }

    /** @copydoc BuildReleaseModal::DrawSummary */
    void BuildReleaseModal::DrawSummary() {
        const auto &fonts = context_.theme.fonts;
        const auto *job = releases_ ? releases_->Selected() : nullptr;
        constexpr std::array states{"queued", "running", "cancelling", "succeeded", "failed", "cancelled"};
        const auto status = job ? Text(states[static_cast<std::size_t>(job->state)]) : Text("draft_badge");
        const Ui::BadgeProps badge{.label = status.c_str(),
                                   .tone = job && job->state == Release::ReleaseJobState::Succeeded ? Ui::BadgeTone::Success
                                           : job && !Running(job)                                   ? Ui::BadgeTone::Error
                                                                                                    : Ui::BadgeTone::Accent,
                                   .leadingIndicator = true};
        const bool wide = ImGui::GetContentRegionAvail().x >= 700;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{16, 2});
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{0, 0});
        if (wide && ImGui::BeginTable("##ReleaseSummary", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, Ui::BadgeWidth(badge, fonts) + 24);
            ImGui::TableSetupColumn("##target", ImGuiTableColumnFlags_WidthFixed, 270);
            ImGui::TableSetupColumn("##path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
            Ui::Badge(badge, fonts);
            ImGui::TableNextColumn();
            Ui::FieldLabel(Text("target").c_str(), fonts);
            const auto target = job ? "#" + std::to_string(job->target.value) : Target();
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, target, Theme::Text(), fonts);
            ImGui::TableNextColumn();
            Ui::FieldLabel(Label("workspace.prepare_release.candidate_path").c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, job ? Label("workspace.prepare_release.path_unavailable") : CandidatePath(),
                            Theme::Text(), fonts);
            ImGui::EndTable();
        } else {
            Ui::Badge(badge, fonts);
            Ui::FieldLabel(Text("target").c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, job ? "#" + std::to_string(job->target.value) : Target(), Theme::Text(),
                            fonts);
            Ui::FieldLabel(Label("workspace.prepare_release.candidate_path").c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, job ? Label("workspace.prepare_release.path_unavailable") : CandidatePath(),
                            Theme::Text(), fonts);
        }
        ImGui::PopStyleVar(2);
    }

    /** @copydoc BuildReleaseModal::DrawConfiguration */
    void BuildReleaseModal::DrawConfiguration() {
        Ui::SectionTitle(Label("workspace.prepare_release.identity").c_str(), context_.theme.fonts);
        Pair("##ReleaseIdentity", context_, "workspace.prepare_release.product_name", "workspace.prepare_release.version", [this] {
            Input("workspace.prepare_release.product_name", draft_.name, context_.theme.fonts);
        }, [this] {
            Input("workspace.prepare_release.version", draft_.version, context_.theme.fonts);
        });
        Pair("##ReleaseSource", context_, "workspace.prepare_release.source", "workspace.prepare_release.notes", [this] {
            Input("workspace.prepare_release.source", draft_.branch, context_.theme.fonts);
        }, [this] {
            Input("workspace.prepare_release.notes", draft_.notes, context_.theme.fonts);
        });
    }

    /** @copydoc BuildReleaseModal::DrawBuildOptions */
    void BuildReleaseModal::DrawBuildOptions() {
        if (!ImGui::TreeNodeEx(Label("workspace.build.advanced").c_str(),
                               ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding))
            return;
        const auto &fonts = context_.theme.fonts;
        Ui::FieldLabel(Text("toolchain").c_str(), fonts);
        std::string compiler = request_.environment.cxxCompiler ? request_.environment.cxxCompiler->string() : Text("host_toolchain");
        ImGui::BeginDisabled();
        (void)Ui::InputTextControl("##ReleaseCompiler", compiler, std::max<std::size_t>(4096, compiler.size() + 1), fonts);
        ImGui::EndDisabled();
        Ui::FieldLabel(Text("parallel_jobs").c_str(), fonts);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        (void)Ui::InputIntControl("##ReleaseJobs", &draft_.jobs, fonts);
        if (draft_.jobs < 1 || draft_.jobs > 256)
            Ui::ErrorText(Text("jobs_invalid").c_str(), fonts);
    }

    /** @copydoc BuildReleaseModal::DrawPage */
    void BuildReleaseModal::DrawPage() {
        const auto &fonts = context_.theme.fonts;
        DrawConfiguration();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        Ui::SectionTitle(Label("workspace.prepare_release.target_content").c_str(), fonts);
        Choice("workspace.prepare_release.product_profile", draft_.product,
               {ProductKeys[0], ProductKeys[1], ProductKeys[2], ProductKeys[3], ProductKeys[4], ProductKeys[5]});
        Pair("##ReleaseTarget", context_, "workspace.build_release.platform", "workspace.build_release.architecture", [this] {
            Select(context_, "workspace.build_release.platform", draft_.platform, {PlatformKeys[0], PlatformKeys[1], PlatformKeys[2]});
        }, [this] {
            Select(context_, "workspace.build_release.architecture", draft_.architecture, {ArchitectureKeys[0], ArchitectureKeys[1]});
        });
        Pair("##ReleasePackage", context_, "workspace.build_release.configuration", "workspace.prepare_release.package_format", [this] {
            Select(context_, "workspace.build_release.configuration", draft_.configuration,
                   {ConfigurationKeys[0], ConfigurationKeys[1], ConfigurationKeys[2]});
        }, [this] {
            Select(context_, "workspace.prepare_release.package_format", draft_.packageFormat,
                   {PackageKeys[0], PackageKeys[1], PackageKeys[2]});
        });
        Choice("workspace.prepare_release.content", draft_.inclusion, {ContentKeys[0], ContentKeys[1], ContentKeys[2]});
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        Ui::SectionTitle(Label("workspace.prepare_release.quality_gates").c_str(), fonts);
        Choice("workspace.prepare_release.test_profile", draft_.testProfile, {TestKeys[0], TestKeys[1], TestKeys[2]});
        Ui::Hint(Label("workspace.prepare_release.smoke_required").c_str(), fonts);
        Notice("##ReleaseGates", Label("workspace.prepare_release.gates_notice").c_str(), fonts);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        Ui::SectionTitle(Label("workspace.prepare_release.security_output").c_str(), fonts);
        Pair("##ReleaseSecurity", context_, "workspace.prepare_release.signing_reference", "workspace.prepare_release.protection", [this] {
            Select(context_, "workspace.prepare_release.signing_reference", draft_.signing,
                   {"workspace.build_release.none", "workspace.build_release.configured_identity"});
        }, [this] {
            Select(context_, "workspace.prepare_release.protection", draft_.protection,
                   {"workspace.build_release.none", "workspace.build_release.project_policy"});
        });
        Field("workspace.prepare_release.output_root", draft_.output);
        ImGui::Spacing();
        DrawBuildOptions();
        Notice("##ReleaseLocalOnly", Label("workspace.prepare_release.local_only").c_str(), fonts);
    }

    /** @copydoc BuildReleaseModal::DrawReview */
    void BuildReleaseModal::DrawReview() {
        const auto &fonts = context_.theme.fonts;
        Ui::SectionTitle(Label("workspace.prepare_release.review_heading").c_str(), fonts);
        Ui::Hint(Label("workspace.prepare_release.review_hint").c_str(), fonts);
        ImGui::Spacing();
        const bool wide = ImGui::GetContentRegionAvail().x >= 500;
        const int groupsPerPage = wide && ImGui::GetContentRegionAvail().y >= 450 ? 2 : 1;
        const int pages = (4 + groupsPerPage - 1) / groupsPerPage;
        auto *storage = ImGui::GetStateStorage();
        const auto pageId = ImGui::GetID("##ReleaseReviewPage");
        const int page = std::clamp(storage->GetInt(pageId), 0, pages - 1);
        float noticeHeight{};
        {
            Theme::ScopedTextStyle style{fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans};
            noticeHeight = ImGui::CalcTextSize(Label("workspace.prepare_release.review_notice").c_str(), nullptr, false,
                                               std::max(1.0F, ImGui::GetContentRegionAvail().x - 34))
                               .y +
                           28;
        }
        const float reserve = noticeHeight + (wide ? 48.0F : 110.0F);
        ImGui::PushID(page);
        ImGui::BeginChild("##ReleaseReviewDetails", {0, std::max(120.0F, ImGui::GetContentRegionAvail().y - reserve)}, false);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{12, 8});
        if (ImGui::BeginTable("##ReleaseReview", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, std::min(190.0F, ImGui::GetContentRegionAvail().x * 0.4F));
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            const auto row = [&](const char *key, const std::string &value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, Label(key), Theme::Muted(), fonts);
                ImGui::TableNextColumn();
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value.empty() ? "—" : value, Theme::Text(), fonts);
            };
            const auto section = [&](int group, const char *key) {
                if (group < page * groupsPerPage || group >= (page + 1) * groupsPerPage)
                    return false;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Ui::FieldLabel(Label(key).c_str(), fonts);
                ImGui::TableNextColumn();
                return true;
            };
            if (section(0, "workspace.prepare_release.release_identity")) {
                row("workspace.prepare_release.product_name", draft_.name);
                row("workspace.prepare_release.version", draft_.version);
                row("workspace.prepare_release.source", draft_.branch);
                row("workspace.prepare_release.notes", draft_.notes);
            }
            if (section(1, "workspace.prepare_release.target_package")) {
                row("workspace.prepare_release.product_profile", Label(ProductKeys[draft_.product]));
                row("workspace.build_release.target", Target());
                row("workspace.prepare_release.package_format", Label(PackageKeys[draft_.packageFormat]));
                row("workspace.prepare_release.content", Label(ContentKeys[draft_.inclusion]));
            }
            if (section(2, "workspace.prepare_release.verification_security")) {
                row("workspace.prepare_release.test_profile", Label(TestKeys[draft_.testProfile]));
                row("workspace.prepare_release.signing_reference", Text(draft_.signing ? "configured_identity" : "none"));
                row("workspace.prepare_release.protection", Text(draft_.protection ? "project_policy" : "none"));
            }
            if (section(3, "workspace.prepare_release.local_output")) {
                row("workspace.prepare_release.output_root", draft_.output);
                row("workspace.prepare_release.candidate_path", CandidatePath());
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::Spacing();
        const auto pageLabel = std::to_string(page + 1) + " / " + std::to_string(pages);
        const char *nextLabel = Label("project_creation.next").c_str();
        const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        float actionsWidth{};
        {
            Theme::ScopedTextStyle style{fonts.sans, metrics.fontSize, Theme::FontPx::Sans};
            actionsWidth = ImGui::CalcTextSize("<").x + ImGui::CalcTextSize(pageLabel.c_str()).x + ImGui::CalcTextSize(nextLabel).x +
                           metrics.paddingX * 4 + ImGui::GetStyle().ItemSpacing.x * 2;
        }
        const bool inlinePages = ImGui::GetContentRegionAvail().x >= actionsWidth;
        if (inlinePages)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - actionsWidth);
        if (Ui::Button({.label = "<", .variant = Ui::ButtonVariant::Secondary, .enabled = page > 0, .font = fonts.sans}))
            storage->SetInt(pageId, page - 1);
        Ui::ShowTooltip(Label("render_graph.previous").c_str(), &fonts);
        if (inlinePages)
            ImGui::SameLine();
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, pageLabel, Theme::Muted(), fonts);
        if (inlinePages)
            ImGui::SameLine();
        if (Ui::Button({.label = nextLabel, .variant = Ui::ButtonVariant::Secondary, .enabled = page + 1 < pages, .font = fonts.sans}))
            storage->SetInt(pageId, page + 1);
        ImGui::Spacing();
        Notice("##ReleaseReadiness", Label("workspace.prepare_release.review_notice").c_str(), fonts);
    }

    /** @copydoc BuildReleaseModal::DrawStages */
    void BuildReleaseModal::DrawStages() {
        const auto *job = releases_ ? releases_->Selected() : nullptr;
        if (!job)
            return;
        const int columns = ImGui::GetContentRegionAvail().x >= 900 ? 9 : ImGui::GetContentRegionAvail().x >= 600 ? 5 : 3;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float width = std::max(1.0F, (ImGui::GetContentRegionAvail().x - gap * (columns - 1)) / columns);
        for (std::size_t i = 0; i < 9; ++i) {
            if (i % columns)
                ImGui::SameLine();
            ImGui::BeginGroup();
            const auto pos = ImGui::GetCursorScreenPos();
            const auto state = job->stages[i].state;
            const auto color = state == Release::ReleaseStageState::Succeeded ? Theme::Ok()
                               : state == Release::ReleaseStageState::Failed  ? Theme::Err()
                                                                              : Theme::Border();
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(pos, {pos.x + width, pos.y + 4}, Theme::U32(color), 2);
            if (state == Release::ReleaseStageState::Running) {
                const float phase =
                    Theme::GetActiveTokens().motion.reduceMotion ? 0.5F : static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1));
                const float segment = width * 0.3F, offset = phase * (width + segment) - segment;
                draw->AddRectFilled({pos.x + std::max(0.0F, offset), pos.y}, {pos.x + std::min(width, offset + segment), pos.y + 4},
                                    Theme::U32(Theme::Accent()), 2);
            }
            ImGui::Dummy({width, 4});
            const auto &caption = Text(StageKeys[i]);
            const auto start = caption.find_first_not_of("0123456789 ");
            const char *label = caption.c_str() + (start == std::string::npos ? caption.size() : start);
            Ui::WrappedText(width, label, state == Release::ReleaseStageState::Running ? Theme::Accent() : Theme::Muted(),
                            context_.theme.fonts);
            ImGui::EndGroup();
        }
    }

    /** @copydoc BuildReleaseModal::DrawRelease */
    void BuildReleaseModal::DrawRelease() {
        const auto &fonts = context_.theme.fonts;
        const auto *job = releases_ ? releases_->Selected() : nullptr;
        if (!job)
            return;
        for (const auto &diagnostic : releases_->SelectedDiagnostics())
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, diagnostic.message,
                            diagnostic.severity == ErrorSeverity::Error ? Theme::Err() : Theme::Text(), fonts);
        if (job->recentDiagnostics.empty())
            Ui::Hint(Text("status").c_str(), fonts);
        if (job->terminal && std::holds_alternative<Release::ReleaseFailed>(*job->terminal))
            Ui::ErrorText(std::get<Release::ReleaseFailed>(*job->terminal).cause.message.c_str(), fonts);
        if (releases_->SelectedCandidateIsFinalVerified())
            Ui::Hint(Label("workspace.prepare_release.final_verified").c_str(), fonts);
    }

    /** @copydoc BuildReleaseModal::DrawFooter */
    bool BuildReleaseModal::DrawFooter() {
        const auto &fonts = context_.theme.fonts;
        const auto *job = releases_ ? releases_->Selected() : nullptr;
        const bool review = draft_.view == static_cast<int>(BuildReleaseTab::Review);
        const char *primary =
            job ? Label("workspace.build.background").c_str()
                : Label(review ? "workspace.prepare_release.prepare_local" : "workspace.prepare_release.review_request").c_str();
        const char *secondary = job      ? (Running(job) ? Text("cancel").c_str() : nullptr)
                                : review ? Label("workspace.content_browser.navigation.back").c_str()
                                         : nullptr;
        if (job && !Running(job))
            primary = Text("close").c_str();
        const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        float width{};
        {
            Theme::ScopedTextStyle style{fonts.sans, metrics.fontSize, Theme::FontPx::Sans};
            width = ImGui::CalcTextSize(primary).x + metrics.paddingX * 2;
            if (secondary)
                width += ImGui::CalcTextSize(secondary).x + metrics.paddingX * 2 + ImGui::GetStyle().ItemSpacing.x;
        }
        const bool inlineActions = ImGui::GetContentRegionAvail().x >= width;
        const bool wide = ImGui::GetContentRegionAvail().x >= width + 220;
        if (job) {
            if (Running(job)) {
                const auto pos = ImGui::GetCursorScreenPos();
                Ui::DrawBusyIndicator(*ImGui::GetWindowDrawList(), {pos.x + 8, pos.y + 8}, 6, Theme::Accent());
                ImGui::Dummy({16, 16});
                ImGui::SameLine();
            }
            const char *status = Text(job->state == Release::ReleaseJobState::Succeeded    ? "succeeded"
                                      : job->state == Release::ReleaseJobState::Failed     ? "failed"
                                      : job->state == Release::ReleaseJobState::Cancelled  ? "cancelled"
                                      : job->state == Release::ReleaseJobState::Running    ? "running"
                                      : job->state == Release::ReleaseJobState::Cancelling ? "cancelling"
                                                                                           : "queued")
                                     .c_str();
            if (Running(job) && job->activeStage) {
                const auto &caption = Text(StageKeys[static_cast<std::size_t>(*job->activeStage)]);
                status = caption.c_str() + std::min(caption.find_first_not_of("0123456789 "), caption.size());
            }
            Ui::WrappedText(wide ? std::max(1.0F, ImGui::GetContentRegionAvail().x - width - 24) : ImGui::GetContentRegionAvail().x, status,
                            Theme::Muted(), fonts);
            if (wide)
                ImGui::SameLine();
        }
        if (wide || !job)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - width));
        if (secondary) {
            if (Ui::Button({.label = secondary, .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans})) {
                if (job) {
                    const auto cancelled = releases_->RequestCancel();
                    if (cancelled.HasError())
                        error_ = cancelled.ErrorValue();
                } else
                    draft_.view = static_cast<int>(BuildReleaseTab::Configuration);
            }
            if (inlineActions)
                ImGui::SameLine();
        }
        if (Ui::Button({.label = primary, .enabled = job || !review, .font = fonts.sans})) {
            if (job)
                return true;
            draft_.view = static_cast<int>(BuildReleaseTab::Review);
        }
        if (!job && review)
            Ui::ShowTooltip(Text("release_unavailable").c_str(), &fonts);
        return false;
    }

    ModalFrameResult BuildReleaseModal::Draw() {
        const auto *job = releases_ ? releases_->Selected() : nullptr;
        Ui::ScopedModalShell shell({.id = "BuildReleaseWorkflow",
                                    .title = Text("release_title").c_str(),
                                    .requestedSize = {1120, 800},
                                    .viewportPadding = 32,
                                    .minimumWidth = 280,
                                    .minimumHeight = 240,
                                    .headerHeight = 38,
                                    .headerHorizontalPadding = 13,
                                    .footerHeight = ImGui::GetMainViewport()->WorkSize.x < 500 ? 136.0F : 60.0F,
                                    .showClose = true,
                                    .closeButtonVariant = Ui::CloseButtonVariant::Compact,
                                    .titleFontSize = Theme::TextPx::Label()},
                                   context_.theme.fonts);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8, 8});
        ImGui::BeginChild("##BuildReleaseBody", {0, shell.BodyHeight()}, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float available = ImGui::GetContentRegionAvail().x;
        if (job) {
            const int rows = available >= 932 ? 1 : available >= 632 ? 2 : 3;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16, 8});
            ImGui::BeginChild("##PrepareReleaseStages", {0, rows * 52.0F}, false,
                              ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawStages();
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        if (job) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20, 10});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
            ImGui::BeginChild("##PrepareReleaseSummary", {0, available >= 700 ? 64.0F : 150.0F}, false,
                              ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawSummary();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 14 * Theme::GetActiveTokens().sizes.uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22, 16});
        const bool fixedReview = !job && draft_.view == static_cast<int>(BuildReleaseTab::Review) && available >= 700;
        ImGui::BeginChild("##PrepareReleaseContent", {0, 0}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding |
                              (fixedReview ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                           : ImGuiWindowFlags_AlwaysVerticalScrollbar));
        if (job)
            DrawRelease();
        else if (draft_.view == static_cast<int>(BuildReleaseTab::Review))
            DrawReview();
        else
            DrawPage();
        if (error_)
            Ui::ErrorText(error_->message.c_str(), context_.theme.fonts);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        shell.BeginFooter({20, 12});
        const bool close = DrawFooter();
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
