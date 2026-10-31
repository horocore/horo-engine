#include "editor/modals/build_release/BuildModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/modals/build_release/BuildActivity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Maps build lifecycle states to the three visible pipeline stages. */
        int BuildStageIndex(const Application::GameplayBuildState state) {
            using enum Application::GameplayBuildState;
            switch (state) {
                case Building:
                    return 1;
                case Validating:
                case Succeeded:
                    return 2;
                default:
                    return 0;
            }
        }

        /** @brief Maps diagnostic stage identity to its visible pipeline position. */
        int OutputStageIndex(const std::string_view stage) {
            if (stage == "build")
                return 1;
            if (stage == "validate")
                return 2;
            return 0;
        }

        /** @brief Resolves build lifecycle feedback independently of widget construction. */
        Ui::BadgeTone BuildBadgeTone(const Application::GameplayBuildState state) {
            if (state == Application::GameplayBuildState::Succeeded)
                return Ui::BadgeTone::Success;
            if (BuildIsRunning(state))
                return Ui::BadgeTone::Accent;
            return Ui::BadgeTone::Error;
        }

        /** @brief Resolves stage feedback while retaining completed-stage precedence. */
        ImVec4 BuildStageColor(const bool completed, const bool current, const bool running) {
            if (completed)
                return Theme::Ok();
            if (current && !running)
                return Theme::Err();
            return Theme::Border();
        }

        /** @brief Resolves footer feedback for active, succeeded and failed builds. */
        ImVec4 BuildStatusColor(const bool running, const bool succeeded) {
            if (running)
                return Theme::Accent();
            if (succeeded)
                return Theme::Ok();
            return Theme::Err();
        }

        constexpr int HostPlatform() {
#if defined(_WIN32)
            return 0;
#elif defined(__APPLE__)
            return 1;
#else
            return 2;
#endif
        }

        constexpr int HostArchitecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
            return 1;
#else
            return 0;
#endif
        }
    }  // namespace

    BuildModal::BuildModal(const EditorGuiContext &context, Application::GameplayBuildRequest request,
                           Application::GameplayBuildService *builds, std::optional<std::uint64_t> &retainedBuild,
                           const IBuildOutputQuery *output)
        : EditorModal(kTextScope), context_(context), request_(std::move(request)), builds_(builds), retainedBuild_(retainedBuild),
          output_(output), platform_(HostPlatform()), architecture_(HostArchitecture()) {
        if (request_.environment.configuration == "Release")
            configuration_ = 2;
        else if (request_.environment.configuration == "RelWithDebInfo")
            configuration_ = 1;
    }

    ModalId BuildModal::Id() const {
        return ModalId{0x4255494C445549ULL};
    }

    ModalPresentation BuildModal::Presentation() const {
        return {.size = ModalSizePolicy::Large};
    }

    ModalClosePolicy BuildModal::ClosePolicy() const {
        return {};
    }

    CloseDecision BuildModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    Result<void> BuildModal::OnOpen(EditorModalContext &) {
        Refresh();
        showDraft_ = !build_.has_value();
        return Result<void>::Success();
    }

    const std::string &BuildModal::Text(const char *key) const {
        return LocalizedText(context_.localization, key);
    }

    bool BuildModal::Running() const {
        return build_ && BuildIsRunning(build_->state);
    }

    const char *BuildModal::Status() const {
        if (build_)
            return context_.localization.Get("editor", BuildStatusKey(build_->state)).c_str();
        return Text("build_release.draft_badge").c_str();
    }

    void BuildModal::Refresh() {
        if (builds_ && !request_.projectRoot.empty()) {
            build_ = builds_->QueryActiveProject(request_.projectRoot);
            if (build_)
                retainedBuild_ = build_->id;
            else if (retainedBuild_)
                build_ = builds_->Query(*retainedBuild_);
        }
        if (output_) {
            if (auto snapshot = output_->SnapshotIfChanged(outputSnapshot_.revision))
                outputSnapshot_ = std::move(*snapshot);
        }
    }

    void BuildModal::OnUpdate(const float dt) {
        if (!std::isfinite(dt) || dt <= 0)
            return;
        refreshSeconds_ += dt;
        if (refreshSeconds_ >= 0.25F) {
            refreshSeconds_ = 0;
            Refresh();
        }
    }

    bool BuildModal::Supported() const {
        return product_ == 0 && workflow_ == 1 && platform_ == HostPlatform() && architecture_ == HostArchitecture() && !testAfter_ &&
               !runAfter_;
    }

    void BuildModal::Choice(const char *key, int &value, const std::initializer_list<const char *> options) {
        std::array<const char *, 5> labels{};
        std::size_t count{};
        for (const auto *option : options)
            labels[count++] = Text(option).c_str();
        ImGui::PushID(key);
        Ui::FieldLabel(Text(key).c_str(), context_.theme.fonts);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        (void)Ui::ComboControl("##value", &value, labels.data(), static_cast<int>(count), context_.theme.fonts,
                               {.componentSize = Ui::ComponentSize::Medium});
        ImGui::PopID();
    }

    void BuildModal::DrawTargetRow(const bool productRow) {
        const bool columns = ImGui::GetContentRegionAvail().x >= 500;
        if (columns &&
            !ImGui::BeginTable(productRow ? "##ProductWorkflow" : "##PlatformArchitecture", 2, ImGuiTableFlags_SizingStretchSame))
            return;
        if (columns)
            ImGui::TableNextColumn();
        ImGui::BeginGroup();
        if (productRow)
            Choice("build.product", product_,
                   {"build_release.game_runtime", "build.dedicated_server", "build_release.engine_editor", "build.engine_cli",
                    "build.sdk"});
        else
            Choice("build_release.platform", platform_, {"build_release.windows", "build_release.macos", "build_release.linux"});
        ImGui::EndGroup();
        if (columns)
            ImGui::TableNextColumn();
        ImGui::BeginGroup();
        if (productRow)
            Choice("build.workflow", workflow_, {"build.playable", "build.compile_only", "build.qa_package"});
        else
            Choice("build_release.architecture", architecture_, {"build_release.x64", "build_release.arm64"});
        ImGui::EndGroup();
        if (columns)
            ImGui::EndTable();
    }

    void BuildModal::DrawForm() {
        const auto &fonts = context_.theme.fonts;
        const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        float manageWidth{};
        {
            Theme::ScopedTextStyle textStyle(fonts.sans, metrics.fontSize, Theme::FontPx::Sans);
            manageWidth = ImGui::CalcTextSize(Text("build.manage_profiles").c_str()).x + metrics.paddingX * 2;
        }
        const bool profileRow = ImGui::GetContentRegionAvail().x >= 500 &&
                                ImGui::BeginTable("##BuildProfile", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX);
        if (profileRow) {
            ImGui::TableSetupColumn("##profile", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##manage", ImGuiTableColumnFlags_WidthFixed, manageWidth);
            ImGui::TableNextColumn();
        }
        ImGui::BeginGroup();
        Choice("build.profile", profile_, {"build.host_profile"});
        ImGui::EndGroup();
        if (profileRow) {
            ImGui::TableNextColumn();
            ImGui::Dummy({0, Theme::TextPx::Label()});
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - manageWidth));
        }
        ImGui::BeginDisabled();
        (void)Ui::Button({.label = Text("build.manage_profiles").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans});
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Ui::ShowTooltip(Text("build.profiles_unavailable").c_str(), &fonts);
        if (profileRow)
            ImGui::EndTable();
        DrawTargetRow(true);
        DrawTargetRow(false);
        Choice("build_release.configuration", configuration_,
               {"build_release.development", "build_release.test_config", "build_release.shipping"});
        if (workflow_ != 1)
            Choice("build.content", content_, {"build.all_content", "build.selected_content"});
        ImGui::Spacing();
        const auto checkboxSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2{checkboxSpacing.x, checkboxSpacing.y + 4 * Theme::GetActiveTokens().sizes.uiScale});
        (void)Ui::CheckboxControl(Text("build.test_after").c_str(), &testAfter_, fonts);
        (void)Ui::CheckboxControl(Text("build.run_after").c_str(), &runAfter_, fonts);
        ImGui::PopStyleVar();
        Ui::Card advanced({.id = "##BuildAdvanced", .defaultExpanded = false});
        advanced.DrawTitleBar({.id = "##title", .title = Text("build.advanced").c_str(), .fonts = fonts});
        if (advanced.BeginBody()) {
            auto toolchain =
                request_.environment.cxxCompiler ? request_.environment.cxxCompiler->string() : Text("build_release.host_toolchain");
            auto project = request_.projectRoot.string();
            Ui::FieldLabel(Text("build_release.toolchain").c_str(), fonts);
            ImGui::BeginDisabled();
            (void)Ui::InputTextControl("##BuildToolchain", toolchain, std::max<std::size_t>(1024, toolchain.size() + 1), fonts);
            ImGui::EndDisabled();
            Ui::FieldLabel(Text("build_release.project").c_str(), fonts);
            ImGui::BeginDisabled();
            (void)Ui::InputTextControl("##BuildProject", project, std::max<std::size_t>(1024, project.size() + 1), fonts);
            ImGui::EndDisabled();
        }
        advanced.Finish();
        Ui::Hint(Text("build.native_scope").c_str(), fonts);
        if (!builds_ || request_.environment.gameplaySdkPackage.empty())
            Ui::Hint(Text("build_release.build_unavailable").c_str(), fonts);
        if (request_.projectRoot.empty())
            Ui::Hint(Text("build_release.no_project").c_str(), fonts);
        if (error_)
            Ui::ErrorText(error_->message.c_str(), fonts);
    }

    void BuildModal::DrawSummary() {
        const auto &fonts = context_.theme.fonts;
        const bool wide = ImGui::GetContentRegionAvail().x >= 500;
        const Ui::BadgeProps badge{.label = Status(), .tone = BuildBadgeTone(build_->state), .leadingIndicator = true};
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{12, 2});
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{0, 0});
        if (wide && !ImGui::BeginTable("##BuildSummary", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::PopStyleVar(2);
            return;
        }
        if (wide) {
            ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, Ui::BadgeWidth(badge, fonts) + 24);
            ImGui::TableSetupColumn("##target", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 44);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 11);
        }
        Ui::Badge(badge, fonts);
        if (wide) {
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        }
        ImGui::BeginGroup();
        Ui::FieldLabel(Text("build_release.target").c_str(), fonts);
        const auto target = request_.projectRoot.filename().string() + " · " + Text("build.compile_only");
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, target, Theme::Text(), fonts);
        ImGui::EndGroup();
        if (wide)
            ImGui::EndTable();
        ImGui::PopStyleVar(2);
    }

    void BuildModal::DrawStages() {
        using enum Application::GameplayBuildState;
        const std::array labels{Text("build_release.configuring").c_str(), Text("build.compile_only").c_str(),
                                Text("build_release.validating").c_str()};
        int active = BuildStageIndex(build_->state);
        if (!Running()) {
            for (auto record = outputSnapshot_.records.rbegin(); record != outputSnapshot_.records.rend(); ++record) {
                if (!build_->operationId || record->operationId != build_->operationId)
                    continue;
                if (record->stage == "configure" || record->stage == "build" || record->stage == "validate") {
                    active = OutputStageIndex(record->stage);
                    break;
                }
            }
        }
        const float width = std::max(1.0F, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (i)
                ImGui::SameLine();
            ImGui::BeginGroup();
            const auto pos = ImGui::GetCursorScreenPos();
            const bool current = static_cast<int>(i) == active;
            const bool completed = build_->state == Succeeded || static_cast<int>(i) < active;
            auto *drawList = ImGui::GetWindowDrawList();
            const auto color = BuildStageColor(completed, current, Running());
            drawList->AddRectFilled(pos, {pos.x + width, pos.y + 4}, Theme::U32(color), 2);
            if (current && Running()) {
                const float segment = width * 0.3F;
                const float phase =
                    Theme::GetActiveTokens().motion.reduceMotion ? 0.5F : static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1));
                const float offset = phase * (width + segment) - segment;
                drawList->AddRectFilled({pos.x + std::max(0.0F, offset), pos.y}, {pos.x + std::min(width, offset + segment), pos.y + 4},
                                        Theme::U32(Theme::Accent()), 2);
            }
            ImGui::Dummy({width, 4});
            float textWidth{};
            {
                Theme::ScopedTextStyle style{context_.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans};
                textWidth = ImGui::CalcTextSize(labels[i], nullptr, false, width).x;
            }
            const float inset = std::max(0.0F, (width - textWidth) * 0.5F);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inset);
            Ui::WrappedText(width - inset, labels[i], current && Running() ? Theme::Accent() : Theme::Muted(), context_.theme.fonts);
            ImGui::EndGroup();
        }
    }

    void BuildModal::DrawActivity() {
        const auto &fonts = context_.theme.fonts;
        if (build_->error)
            Ui::ErrorText(build_->error->message.c_str(), fonts);
        if (error_)
            Ui::ErrorText(error_->message.c_str(), fonts);
        bool records{};
        for (const auto &record : outputSnapshot_.records) {
            if (!build_->operationId || record.operationId != build_->operationId)
                continue;
            records = true;
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, record.message,
                            record.severity == DiagnosticSeverity::Error ? Theme::Err() : Theme::Text(), fonts);
        }
        if (!records)
            Ui::Hint(Status(), fonts);
    }

    bool BuildModal::DrawFooter() {
        const auto &fonts = context_.theme.fonts;
        const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        const bool running = !showDraft_ && Running();
        const char *secondary = nullptr;
        if (!showDraft_)
            secondary = Text(running ? "build_release.cancel" : "build.new_job").c_str();
        const char *primaryKey = "build_release.close";
        if (showDraft_)
            primaryKey = "build.title";
        else if (running)
            primaryKey = "build.background";
        const char *primary = Text(primaryKey).c_str();
        float width{};
        {
            Theme::ScopedTextStyle ts(fonts.sans, metrics.fontSize, Theme::FontPx::Sans);
            width = ImGui::CalcTextSize(primary).x + metrics.paddingX * 2;
            if (secondary)
                width += ImGui::CalcTextSize(secondary).x + metrics.paddingX * 2 + ImGui::GetStyle().ItemSpacing.x;
        }
        const bool stacked = width > ImGui::GetContentRegionAvail().x;
        if (!showDraft_) {
            const float statusWidth =
                stacked ? ImGui::GetContentRegionAvail().x : std::max(1.0F, ImGui::GetContentRegionAvail().x - width - 16);
            ImGui::BeginChild("##BuildFooterStatus", {statusWidth, metrics.minimumHeight}, false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const auto pos = ImGui::GetCursorScreenPos();
            const auto color = BuildStatusColor(running, build_ && build_->state == Application::GameplayBuildState::Succeeded);
            const ImVec2 center{pos.x + 8, pos.y + metrics.minimumHeight * 0.5F};
            if (running)
                Ui::DrawBusyIndicator(*ImGui::GetWindowDrawList(), center, 6, color);
            else
                ImGui::GetWindowDrawList()->AddCircleFilled(center, 3, Theme::U32(color));
            {
                Theme::ScopedTextStyle textStyle(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
                ImGui::GetWindowDrawList()->AddText({pos.x + 22, center.y - ImGui::GetTextLineHeight() * 0.5F}, Theme::U32(color),
                                                    Status());
            }
            ImGui::EndChild();
            if (!stacked)
                ImGui::SameLine(0, 16);
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - width));
        if (secondary) {
            if (Ui::Button({.label = secondary,
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = fonts.sans,
                            .style = {.width = stacked ? Ui::StyleWidth::FillAvailable : Ui::StyleWidth::FitContent}})) {
                if (running) {
                    (void)builds_->RequestCancel(build_->id);
                    Refresh();
                } else {
                    showDraft_ = true;
                    error_.reset();
                }
            }
            if (!stacked)
                ImGui::SameLine();
        }
        const bool ready =
            builds_ && !request_.projectRoot.empty() && !request_.environment.gameplaySdkPackage.empty() && Supported() && !Running();
        if (Ui::Button({.label = primary,
                        .enabled = !showDraft_ || ready,
                        .font = fonts.sans,
                        .style = {.width = stacked ? Ui::StyleWidth::FillAvailable : Ui::StyleWidth::FitContent}})) {
            if (!showDraft_)
                return true;
            constexpr const char *configurations[]{"Debug", "RelWithDebInfo", "Release"};
            request_.environment.configuration = configurations[configuration_];
            const auto started = builds_->Start(request_);
            if (started.HasValue()) {
                retainedBuild_ = started.Value();
                showDraft_ = false;
                error_.reset();
                Refresh();
            } else
                error_ = started.ErrorValue();
        }
        if (showDraft_ && !ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            Ui::ShowTooltip(Text("build.native_scope").c_str(), &fonts);
        return false;
    }

    ModalFrameResult BuildModal::Draw() {
        Ui::ScopedModalShell shell({.id = "BuildWorkflow",
                                    .title = Text("build.title").c_str(),
                                    .requestedSize = {820, 700},
                                    .viewportPadding = 32,
                                    .minimumWidth = 280,
                                    .minimumHeight = 240,
                                    .headerHeight = 38,
                                    .headerHorizontalPadding = 13,
                                    .footerHeight = ImGui::GetMainViewport()->WorkSize.x < 500 ? 136.0F : 60.0F,
                                    .foregroundBorder = true,
                                    .closeButtonVariant = Ui::CloseButtonVariant::Compact,
                                    .titleFontSize = Theme::TextPx::Label()},
                                   context_.theme.fonts);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
        ImGui::BeginChild("##BuildBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (showDraft_) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20, 12});
            ImGui::BeginChild("##BuildForm", {0, 0}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
            DrawForm();
            ImGui::EndChild();
            ImGui::PopStyleVar();
        } else if (build_) {
            const bool wide = ImGui::GetContentRegionAvail().x >= 500;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16, 6});
            ImGui::BeginChild("##BuildStages", {0, wide ? 44.0F : 62.0F}, false,
                              ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawStages();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20, 10});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
            ImGui::BeginChild("##BuildSummarySurface", {0, wide ? 64.0F : 110.0F}, false,
                              ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            DrawSummary();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16, 12});
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
            ImGui::BeginChild("##BuildLogArea", {0, 0}, true,
                              ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_AlwaysVerticalScrollbar);
            DrawActivity();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        shell.BeginFooter({20, 12});
        const bool close = DrawFooter();
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
