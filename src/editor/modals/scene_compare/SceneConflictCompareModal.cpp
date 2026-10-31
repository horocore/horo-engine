#include "editor/modals/scene_compare/SceneConflictCompareModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <format>
#include <imgui.h>
#include <mutex>
#include <string>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Ui::BadgeTone ToneFor(const SceneObjectComparisonKind kind) noexcept {
            using enum Ui::BadgeTone;
            using enum SceneObjectComparisonKind;
            switch (kind) {
                case AddedOnDisk:
                    return Success;
                case RemovedFromDisk:
                    return Error;
                case Modified:
                    return Warning;
            }
            return Neutral;
        }

        [[nodiscard]] const std::string &KindLabel(const EditorGuiContext &context, const SceneObjectComparisonKind kind) {
            using enum SceneObjectComparisonKind;
            switch (kind) {
                case AddedOnDisk:
                    return SceneConflictCompareModal::kTextScope.Text(context.localization, "added");
                case RemovedFromDisk:
                    return SceneConflictCompareModal::kTextScope.Text(context.localization, "removed");
                case Modified:
                    return SceneConflictCompareModal::kTextScope.Text(context.localization, "modified");
            }
            return SceneConflictCompareModal::kTextScope.Text(context.localization, "modified");
        }

        [[nodiscard]] std::string DifferenceLabel(const SceneObjectComparison &object, const EditorGuiContext &context) {
            std::string label;
            const auto append = [&label](const std::string_view field) {
                if (!label.empty())
                    label += ", ";
                label += field;
            };
            if (object.fields.name)
                append(SceneConflictCompareModal::kTextScope.Text(context.localization, "field.name"));
            if (object.fields.parent)
                append(SceneConflictCompareModal::kTextScope.Text(context.localization, "field.parent"));
            if (object.fields.transform)
                append(SceneConflictCompareModal::kTextScope.Text(context.localization, "field.transform"));
            if (object.fields.primitive || object.fields.asset)
                append(SceneConflictCompareModal::kTextScope.Text(context.localization, "field.primitive"));
            if (object.fields.components)
                append(SceneConflictCompareModal::kTextScope.Text(context.localization, "field.components"));
            return label;
        }

        void DrawSummaryBadge(const std::size_t count, const char *label, const Ui::BadgeTone tone, const Theme::Fonts &fonts) {
            const std::string text = std::format("{} {}", count, label != nullptr ? label : "");
            Ui::Badge(
                {
                    .label = text.c_str(),
                    .tone = tone,
                    .size = Ui::BadgeSize::Medium,
                },
                fonts);
        }

        [[nodiscard]] std::string ResolveDisplayName(const SceneObjectComparison &object) {
            if (object.kind == SceneObjectComparisonKind::AddedOnDisk) {
                return object.diskName;
            }
            if (object.kind == SceneObjectComparisonKind::RemovedFromDisk) {
                return object.documentName;
            }
            if (object.documentName == object.diskName) {
                return object.documentName;
            }
            return object.documentName + "  ->  " + object.diskName;
        }

        void DrawComparisonRow(const SceneObjectComparison &object, const EditorGuiContext &context) {
            const std::string &kindLabel = KindLabel(context, object.kind);
            const std::string displayName = ResolveDisplayName(object);
            const std::string differences = DifferenceLabel(object, context);

            const std::string rowId = std::to_string(object.id.value);
            ImGui::PushID(rowId.c_str());
            Ui::ScopedCard card("##SceneDifference", {0.0F, 0.0F}, 16.0F, 12.0F, Theme::Bg2(), true);
            if (ImGui::BeginTable("##DifferenceColumns", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 150.0F);
                ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                Ui::Badge(
                    {
                        .label = kindLabel.c_str(),
                        .tone = ToneFor(object.kind),
                        .size = Ui::BadgeSize::Small,
                    },
                    context.theme.fonts);

                ImGui::TableSetColumnIndex(1);
                {
                    Theme::ScopedTextStyle style(context.theme.fonts.sansEmphasis, Theme::TextPx::Title(), Theme::FontPx::SansEmphasis);
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
                    ImGui::TextUnformatted(displayName.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
                ImGui::Text("%s %llu", SceneConflictCompareModal::kTextScope.Text(context.localization, "field.id").c_str(),
                            static_cast<unsigned long long>(object.id.value));
                if (!differences.empty())
                    ImGui::TextWrapped("%s", differences.c_str());
                ImGui::PopStyleColor();
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    }  // namespace

    struct SceneConflictCompareModal::Completion {
        std::mutex mutex;
        std::optional<SceneDocumentComparison> comparison;
        std::optional<Error> error;
    };

    /** @copydoc SceneConflictCompareModal::SceneConflictCompareModal */
    SceneConflictCompareModal::SceneConflictCompareModal(const EditorGuiContext &context, JobSystem &jobs,
                                                         SceneDocumentComparisonRequest request)
        : EditorModal(kTextScope), context_(context), jobs_(jobs), request_(std::move(request)),
          absoluteScenePath_(request_.absoluteScenePath.string()), completion_(std::make_shared<Completion>()) {}

    /** @copydoc SceneConflictCompareModal::~SceneConflictCompareModal */
    SceneConflictCompareModal::~SceneConflictCompareModal() {
        cancellation_.RequestCancellation();
        if (job_.has_value()) {
            static_cast<void>(jobs_.RequestCancel(job_->Id()));
            static_cast<void>(job_->Wait());
        }
    }

    /** @copydoc SceneConflictCompareModal::Id */
    ModalId SceneConflictCompareModal::Id() const {
        return ModalId{kModalId};
    }

    /** @copydoc SceneConflictCompareModal::Presentation */
    ModalPresentation SceneConflictCompareModal::Presentation() const {
        return {
            .size = ModalSizePolicy::Medium,
            .dimWorkspace = true,
        };
    }

    /** @copydoc SceneConflictCompareModal::ClosePolicy */
    ModalClosePolicy SceneConflictCompareModal::ClosePolicy() const {
        return {};
    }

    /** @copydoc SceneConflictCompareModal::OnOpen */
    Result<void> SceneConflictCompareModal::OnOpen(EditorModalContext &) {
        const std::shared_ptr<Completion> completion = completion_;
        Result<JobHandle> submitted =
            jobs_.SubmitResult({.parentCancellation = cancellation_.Token()},
                               [request = std::move(request_), completion](const CancellationToken &cancellation) mutable {
            if (cancellation.IsCancellationRequested())
                return JobCancelled();
            Result<SceneDocumentComparison> compared = LoadSceneDocumentComparison(request);
            if (cancellation.IsCancellationRequested())
                return JobCancelled();

            std::lock_guard lock(completion->mutex);
            if (compared.HasError())
                completion->error = compared.ErrorValue();
            else
                completion->comparison = std::move(compared).Value();
            return Result<void>::Success();
        });
        if (submitted.HasError())
            return Result<void>::Failure(submitted.ErrorValue());
        job_ = std::move(submitted).Value();
        return Result<void>::Success();
    }

    /** @copydoc SceneConflictCompareModal::OnUpdate */
    void SceneConflictCompareModal::OnUpdate(float) {
        if (job_.has_value()) {
            using enum JobState;
            const JobSnapshot snapshot = jobs_.Query(job_->Id());
            if (snapshot.state == Succeeded || snapshot.state == Failed || snapshot.state == Cancelled) {
                if (Result<void> completed = job_->Wait(); completed.HasError())
                    error_ = completed.ErrorValue();
                job_.reset();
            }
        }

        std::lock_guard lock(completion_->mutex);
        if (completion_->comparison.has_value()) {
            comparison_ = std::move(completion_->comparison);
            completion_->comparison.reset();
        }
        if (completion_->error.has_value()) {
            error_ = std::move(completion_->error);
            completion_->error.reset();
        }
    }

    /** @copydoc SceneConflictCompareModal::Draw */
    ModalFrameResult SceneConflictCompareModal::Draw() {
        const std::string &title = SceneConflictCompareModal::kTextScope.Text(context_.localization, "title");
        Ui::ScopedModalShell modal(
            {
                .id = "SceneConflictCompare",
                .title = title.c_str(),
                .requestedSize = {900.0F, 620.0F},
                .viewportPadding = 48.0F,
                .minimumWidth = 520.0F,
                .minimumHeight = 420.0F,
                .footerHeight = 0.0F,
                .showClose = true,
                .titleFontSize = Theme::TextPx::Title(),
            },
            context_.theme.fonts);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{24.0F, 20.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::BeginChild("##SceneCompareBody", {0.0F, modal.BodyHeight()}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);

        const std::string &description = SceneConflictCompareModal::kTextScope.Text(context_.localization, "description");
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
        ImGui::TextWrapped("%s", description.c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
        ImGui::TextWrapped("%s", absoluteScenePath_.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy({0.0F, 8.0F});

        if (error_.has_value()) {
            const std::string &failed = SceneConflictCompareModal::kTextScope.Text(context_.localization, "failed");
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Err());
            ImGui::TextWrapped("%s", failed.c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
            ImGui::TextWrapped("%s", error_->message.c_str());
            ImGui::PopStyleColor();
        } else if (!comparison_.has_value()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
            ImGui::TextUnformatted(SceneConflictCompareModal::kTextScope.Text(context_.localization, "loading").c_str());
            ImGui::PopStyleColor();
        } else {
            const SceneDocumentComparison &comparison = *comparison_;
            DrawSummaryBadge(comparison.addedOnDisk,
                             SceneConflictCompareModal::kTextScope.Text(context_.localization, "summary.added").c_str(),
                             Ui::BadgeTone::Success, context_.theme.fonts);
            ImGui::SameLine(0.0F, 8.0F);
            DrawSummaryBadge(comparison.removedFromDisk,
                             SceneConflictCompareModal::kTextScope.Text(context_.localization, "summary.removed").c_str(),
                             Ui::BadgeTone::Error, context_.theme.fonts);
            ImGui::SameLine(0.0F, 8.0F);
            DrawSummaryBadge(comparison.modified,
                             SceneConflictCompareModal::kTextScope.Text(context_.localization, "summary.modified").c_str(),
                             Ui::BadgeTone::Warning, context_.theme.fonts);
            ImGui::Dummy({0.0F, 10.0F});
            ImGui::Separator();
            ImGui::Dummy({0.0F, 8.0F});

            if (!comparison.HasDifferences()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
                ImGui::TextWrapped("%s", SceneConflictCompareModal::kTextScope.Text(context_.localization, "empty").c_str());
                ImGui::PopStyleColor();
            } else {
                for (const SceneObjectComparison &object : comparison.objects) {
                    DrawComparisonRow(object, context_);
                    ImGui::Dummy({0.0F, 8.0F});
                }
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return modal.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }

    /** @copydoc SceneConflictCompareModal::CanClose */
    CloseDecision SceneConflictCompareModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }
}  // namespace Horo::Editor
