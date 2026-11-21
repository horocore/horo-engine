#include "EditorLocalAgentPanel.h"

#include "AgentBackgroundTurn.h"
#include "AgentLocalSceneTurn.h"
#include "AgentTurn.h"
#include "EditorLocalAgentContext.h"
#include "EditorLocalAgentHistory.h"
#include "EditorLocalAgentPatch.h"
#include "EditorLocalAgentPatchCheckpoint.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <stop_token>
#include <utility>
#include <variant>

namespace Horo::Editor {
    namespace {
        using namespace Agent;

        struct ContextReview final {
            Result<EditorLocalAgentContext> outcome;
            const char *unavailable;
        };

        using Reply = std::variant<ProviderDiscovery, AgentTurnResult, ContextReview>;
        using BackgroundTurn = AgentBackgroundTurn<Reply>;
        using PatchLoad = Result<std::optional<EditorLocalAgentPatchReview>>;
        using PatchWorker = AgentBackgroundTurn<std::variant<Result<void>, PatchLoad>>;

        /** @brief Resolves the localized operation label from the admitted file kind. */
        const char *PatchFileLabel(const EditorPatchFileKind kind) {
            constexpr std::array keys{"patch_file_create", "patch_file_edit", "patch_file_delete", "patch_file_rename"};
            return keys[static_cast<std::size_t>(kind)];
        }

        /** @brief Gives each planner conflict its owning user-facing status. */
        const char *PatchConflictLabel(const Proposals::PlanConflictReason reason) {
            using enum Proposals::PlanConflictReason;
            switch (reason) {
                case SourceStale:
                    return "source_patch_stale";
                case SourceMissing:
                case SourceMoved:
                case SourceUnavailable:
                case Unobserved:
                    return "patch_file_source_unavailable";
                case SourceChangedOnDisk:
                    return "patch_file_disk_changed";
                case DestinationOccupied:
                    return "patch_file_destination_occupied";
                case OverlappingPaths:
                    return "patch_file_overlap";
            }
            return "patch_file_source_unavailable";
        }

        /** @brief Resolves a canonical review state through the editor catalog. */
        const char *PatchDecisionLabel(const Proposals::ReviewState state) {
            constexpr std::array labels{"patch_file_pending", "patch_file_accepted", "patch_file_rejected", "patch_file_mixed"};
            return labels[static_cast<std::size_t>(state)];
        }

        enum class SceneQueryPhase {
            Idle,
            Running,
            Succeeded,
            Denied,
            Cancelled,
            TimedOut,
            Failed
        };
        using SceneQueryProgress = std::atomic<SceneQueryPhase>;

        /** @brief Projects only the settled tool status, with no provider content or MCP result bytes. */
        SceneQueryPhase ScenePhase(const AgentToolActivity &activity) {
            if (!activity.outcome)
                return SceneQueryPhase::Running;
            switch (*activity.outcome) {
                case AgentTurnStatus::Completed:
                    return SceneQueryPhase::Succeeded;
                case AgentTurnStatus::ToolDenied:
                    return SceneQueryPhase::Denied;
                case AgentTurnStatus::Cancelled:
                    return SceneQueryPhase::Cancelled;
                case AgentTurnStatus::TimedOut:
                    return SceneQueryPhase::TimedOut;
                default:
                    return SceneQueryPhase::Failed;
            }
        }

        /** @brief Resolves host-authored tool copy for the latest observed phase. */
        const char *ScenePhaseKey(const SceneQueryPhase phase) {
            using enum SceneQueryPhase;
            switch (phase) {
                case Idle:
                    return nullptr;
                case Running:
                    return "scene_query_running";
                case Succeeded:
                    return "scene_query_succeeded";
                case Denied:
                    return "scene_query_denied";
                case Cancelled:
                    return "scene_query_cancelled";
                case TimedOut:
                    return "scene_query_timeout";
                case Failed:
                    return "scene_query_failed";
            }
            return nullptr;
        }

        /** @brief Admits a complete bounded clipboard string after an explicit user action. */
        std::string_view CaptureClipboardText() {
            const auto *clipboard = ImGui::GetClipboardText();
            if (!clipboard)
                return {};
            constexpr auto Maximum = Security::AgentPolicyLimits{}.maximumItemBytes;
            std::size_t size{};
            while (size <= Maximum && clipboard[size] != '\0')
                ++size;
            return size <= Maximum ? std::string_view{clipboard, size} : std::string_view{};
        }

        /** @brief Runs one user-initiated discovery or inference on an isolated worker. */
        Reply RunLocalOperation(std::string endpoint, std::optional<ModelRequest> request, std::optional<std::size_t> sceneCount,
                                std::optional<EditorLocalAgentContext> source, std::uint64_t turn,
                                const std::shared_ptr<AgentHistoryLiveDraft> &draft, const std::shared_ptr<SceneQueryProgress> &sceneQuery,
                                std::stop_token stop) {
            ModelProviderConfig config{.endpoint = std::move(endpoint)};
            if (request && sceneCount.has_value())
                return RunApprovedLocalSceneTurn(std::move(config), std::move(*request), turn, *sceneCount,
                                                 Platform::CreateNativeSecureRandomSource(), stop,
                                                 [&sceneQuery](const AgentToolActivity activity) {
                    sceneQuery->store(ScenePhase(activity), std::memory_order_relaxed);
                });
            auto provider = CreateOllamaModelProvider(std::move(config));
            if (!request)
                return provider->Discover(stop);
            AgentTurnRuntime runtime;
            AgentTurnTextObserver observe;
            if (draft)
                observe = [&draft](std::string_view delta) {
                    draft->Append(delta);
                };
            if (source) {
                const auto dispatch = source->Dispatch(stop);
                return runtime.Run(*provider, nullptr, turn, std::move(*request), {}, stop, &dispatch, observe);
            }
            return runtime.Run(*provider, nullptr, turn, std::move(*request), {}, stop, nullptr, observe);
        }

        /** @brief Presents sanitized, localized remediation without displaying transport response bodies. */
        const char *FailureKey(const std::optional<ModelErrorCode> error) {
            using enum ModelErrorCode;
            if (error == InvalidRequest)
                return "invalid";
            if (error == Cancelled)
                return "cancelled";
            if (error == UnsupportedCapability)
                return "tools_unsupported";
            if (error == Unavailable || error == Transport)
                return "unavailable";
            return "failed";
        }

        /** @brief Presents and reviews results from the editor-owned background operation. */
        class LocalAgentPanel final : public IWorkspacePanel {
        public:
            LocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files, std::shared_ptr<BackgroundTurn> background)
                : IWorkspacePanel("workspace.local_agent"), background_(std::move(background)), history_(jobs, files), files_(files) {}

            ~LocalAgentPanel() override {
                RevokeContext();
            }

            std::string GetId() const override {
                return "horo.local_agent";
            }

            std::string GetDisplayName() const override {
                return "workspace.local_agent.title";
            }

            WorkspaceDockArea GetDefaultDockArea() const override {
                return WorkspaceDockArea::Right;
            }

            std::vector<std::string> GetObservedEventTypes() const override {
                return {};
            }

            void OnAttach(PanelContext &context) override {
                sceneCount_ = context.sceneObjectCount;
                RevokeContext();
                inputRouter_ = context.inputRouter;
                background_->Abandon();
                activeSource_ = context.activeSourceSnapshot;
                activeSourceRange_ = context.activeSourceRange;
                openSources_ = context.openSourceSnapshots;
                requestSourceReview_ = context.requestSourceReview;
                requestSourceReviewApply_ = context.requestSourceReviewApply;
                requestSourceReviewUndo_ = context.requestSourceReviewUndo;
                sourceReviewTransaction_ = context.sourceReviewTransaction;
                projectFiles_ = context.agentProjectFiles;
                projectReadIdentity_ = context.agentProjectReadIdentity;
                captureSceneSelection_ = context.captureSceneSelection;
                captureEditorDiagnostics_ = context.captureEditorDiagnostics;
                buildOutput_ = context.buildOutputQuery;
                gameplayBuilds_ = context.gameplayBuilds;
                if (context.projectFileRoot)
                    buildRoot_ = *context.projectFileRoot;
                contextProject_ = context.projectIdentity;
                contextLifetime_.emplace();
                ++contextGeneration_;
                if (context.projectFileRoot && !context.projectIdentity.empty())
                    history_.Attach({*context.projectFileRoot, std::string{context.projectIdentity}});
            }

            void OnDetach() override {
                background_->Abandon();
                prompt_.clear();
                response_.clear();
                sceneCount_ = {};
                RevokeContext();
                history_.Detach();
                liveDraft_.reset();
                awaitingCheckpoint_ = false;
                submittedPrompt_.reset();
                submittedReference_.reset();
                status_ = nullptr;
            }

            void OnPresentationHidden(EditorWorkspaceViewCommandData &) override {
                textInput_.Reset();
            }

            /** @brief Advances result publication and project storage after the workspace's runtime update. */
            void Poll() {
                if (history_.Poll()) {
                    if (std::exchange(awaitingCheckpoint_, false))
                        Send(prompt_, {}, true);
                    else
                        history_.FinishRecovery(AgentHistoryMessageState::Incomplete);
                } else if (awaitingCheckpoint_ && !history_.RecoveryStartPending()) {
                    awaitingCheckpoint_ = false;
                }
                PollReply();
                PollPatchCheckpoint();
                PollLiveResponse();
            }

            void DrawIcon(ImDrawList *draw, const ImVec2 &position, const ImVec2 &size, ImU32 color) override {
                const ImVec2 low{position.x + size.x * 0.15F, position.y + size.y * 0.2F};
                const ImVec2 high{position.x + size.x * 0.85F, position.y + size.y * 0.7F};
                draw->AddRect(low, high, color, size.x * 0.12F);
                draw->AddLine({low.x + size.x * 0.15F, high.y}, {low.x + size.x * 0.15F, high.y + size.y * 0.12F}, color);
            }

            void DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &, EditorWorkspaceViewCommandData &command,
                           const EditorGuiContext &context) override {
                const auto &locale = context.localization;
                const auto &fonts = context.theme.fonts;
                ImGui::BeginChild("##LocalAgent", size, false, ImGuiWindowFlags_NoSavedSettings);
                textFieldActive_ = false;
                DrawProvider(locale, fonts);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "prompt"), Theme::Text(), fonts);
                ImGui::BeginDisabled(awaitingCheckpoint_ || contextPreview_);
                static_cast<void>(DrawTextField("##Prompt", prompt_, 4096, fonts));
                ImGui::EndDisabled();
                DrawSendActions(locale, fonts);
                if (Busy() && Button(locale, "stop", background_->CanCancel()))
                    background_->Cancel();
                if (status_)
                    ImGui::TextWrapped("%s", LocalizedText(locale, status_).c_str());
                DrawSceneQueryActivity(locale, fonts);
                if (!response_.empty())
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, response_, Theme::Text(), fonts);
                DrawSourceTransaction(locale, fonts, command);
                DrawPatchReview(locale, fonts, command);
                if (liveDraft_ && history_.HasRecovery() && background_->CanCancel() && Button(locale, "history_draft_offer", true))
                    history_.ReviewLiveDraft(*liveDraft_);
                textFieldActive_ |= history_.Draw(context);
                UpdateTextInputFocus();
                ImGui::EndChild();
            }

        private:
            enum class BuildContextChoice {
                ActiveOutput,
                ActiveDiagnostics,
                CompletedOutput,
                CompletedDiagnostics
            };

            enum class SourcePickerMode {
                None,
                OpenTabs,
                SavedFile
            };

            struct PlainReplay final {
                std::string endpoint;
                std::string model;
                std::string prompt;
            };

            [[nodiscard]] bool Busy() const {
                return background_->Busy();
            }

            /** @brief Checks the common prerequisites for dispatch at the currently selected destination. */
            [[nodiscard]] bool CanRunTurn() const {
                return !Busy() && !patchCheckpoint_.Busy() && !awaitingCheckpoint_ && !history_.HasRecovery() && !endpoint_.empty() &&
                       !model_.empty();
            }

            /** @brief Enables composer actions only when a prompt has been entered. */
            [[nodiscard]] bool CanSendPrompt() const {
                return CanRunTurn() && !prompt_.empty();
            }

            void RevokeContext() noexcept {
                textInput_.Reset();
                inputRouter_ = nullptr;
                if (contextLifetime_)
                    contextLifetime_->RequestCancellation();
                contextLifetime_.reset();
                contextPreview_.reset();
                plainReplay_.reset();
                sceneQuery_.reset();
                activeSource_ = {};
                activeSourceRange_ = {};
                openSources_ = {};
                requestSourceReview_ = {};
                requestSourceReviewApply_ = {};
                requestSourceReviewUndo_ = {};
                sourceReviewTransaction_ = {};
                sourceApplyQueued_ = false;
                patchSource_ = {};
                patchReview_.reset();
                patchCheckpoint_.Abandon();
                captureSceneSelection_ = {};
                captureEditorDiagnostics_ = {};
                sourcePicker_ = SourcePickerMode::None;
                selectedOpenFiles_.clear();
                projectFiles_.reset();
                projectReadIdentity_.clear();
                savedFilePath_.clear();
                buildOutput_ = nullptr;
                gameplayBuilds_ = nullptr;
                buildRoot_.clear();
                contextProject_.clear();
            }

            [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled) const {
                const auto &visible = LocalizedText(locale, key);
                const auto label = visible + "###" + key;
                const bool clicked =
                    Ui::Button({.label = label.c_str(), .enabled = enabled, .style = {.width = Ui::StyleWidth::FillAvailable}});
                if ((ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) &&
                    ImGui::CalcTextSize(visible.c_str()).x + 2.0F * ImGui::GetStyle().FramePadding.x > ImGui::GetItemRectSize().x)
                    Ui::ShowTooltip(visible.c_str());
                return clicked;
            }

            /** @brief Tracks only the chat's own active text widgets. */
            bool DrawTextField(const char *id, std::string &value, const std::size_t capacity, const Theme::Fonts &fonts) {
                const bool changed = Ui::InputTextControl(id, value, capacity, fonts);
                textFieldActive_ |= ImGui::IsItemActive();
                return changed;
            }

            /** @brief Blocks workspace shortcuts only while the chat owns text input. */
            void UpdateTextInputFocus() {
                if (!textFieldActive_)
                    textInput_.Reset();
                else if (inputRouter_ && !textInput_.IsActive())
                    textInput_ = inputRouter_->PushContext(Input::InputContextId{"editor.local_agent.text"},
                                                           Input::InputContextKind::FocusedGuiWidget);
            }

            /** @brief Presents the actual admitted scene query's phase in the current response. */
            void DrawSceneQueryActivity(const ILocalizationService &locale, const Theme::Fonts &fonts) const {
                if (!sceneQuery_)
                    return;
                if (const auto *key = ScenePhaseKey(sceneQuery_->load(std::memory_order_relaxed)))
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, key), Theme::Muted(), fonts);
            }

            void DrawProvider(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "endpoint"), Theme::Text(), fonts);
                ImGui::BeginDisabled(Busy() || awaitingCheckpoint_ || history_.HasRecovery() || contextPreview_);
                if (DrawTextField("##Endpoint", endpoint_, 256, fonts)) {
                    models_.clear();
                    selection_ = -1;
                    response_.clear();
                    patchReview_.reset();
                    status_ = nullptr;
                }
                if (Button(locale, "discover", !Busy() && !endpoint_.empty()))
                    static_cast<void>(Start(std::nullopt));
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "model"), Theme::Text(), fonts);
                if (DrawTextField("##Model", model_, 256, fonts))
                    selection_ = -1;
                DrawAvailableModels(fonts);
                ImGui::EndDisabled();
            }

            void DrawAvailableModels(const Theme::Fonts &fonts) {
                if (models_.empty())
                    return;
                std::array<const char *, 64> labels{};
                for (std::size_t index = 0; index < models_.size(); ++index)
                    labels[index] = models_[index].id.c_str();
                if (Ui::ComboControl("##AvailableModels", &selection_, labels.data(), static_cast<int>(models_.size()), fonts) &&
                    selection_ >= 0 && static_cast<std::size_t>(selection_) < models_.size())
                    model_ = models_[static_cast<std::size_t>(selection_)].id;
            }

            void DrawSendActions(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                if (contextPreview_) {
                    DrawContextReview(locale);
                    return;
                }
                if (sourcePicker_ == SourcePickerMode::OpenTabs) {
                    DrawSourcePicker(locale);
                    return;
                }
                if (sourcePicker_ == SourcePickerMode::SavedFile) {
                    DrawSavedFilePicker(locale, fonts);
                    return;
                }
                const bool ready = CanSendPrompt();
                if (Button(locale, "ask", ready)) {
                    Send(prompt_);
                    return;
                }
                if (DrawRegenerate(locale)) {
                    return;
                }
                if (Button(locale, "history_recovery_offer", ready && history_.CanReviewRecovery())) {
                    awaitingCheckpoint_ = history_.ReviewRecoveryPrompt(prompt_);
                    return;
                }
                if (DrawSourceContextActions(locale, ready) || DrawBuildContextActions(locale, ready) ||
                    DrawClipboardActions(locale, ready) || DrawEditorDiagnosticsAction(locale, ready)) {
                    return;
                }
                const bool sceneAvailable = ready && captureSceneSelection_ && contextLifetime_ && !contextProject_.empty();
                if (Button(locale, "context_scene_selection_offer", sceneAvailable)) {
                    PrepareSceneSelectionReview();
                    return;
                }
                const auto count = sceneCount_ ? sceneCount_() : std::nullopt;
                if (count.has_value()) {
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "scene_count") + " " + std::to_string(*count),
                                    Theme::Text(), fonts);
                    if (Button(locale, "ask_with_scene_count", ready))
                        Send(prompt_, count);
                }
            }

            /** @brief Offers the last plain prompt again only at its exact prior destination. */
            bool DrawRegenerate(const ILocalizationService &locale) {
                if (!plainReplay_)
                    return false;
                const bool sameDestination = plainReplay_->endpoint == endpoint_ && plainReplay_->model == model_;
                if (!Button(locale, "regenerate", sameDestination && CanRunTurn()))
                    return false;
                Send(plainReplay_->prompt);
                return true;
            }

            /** @brief Opens a query for the active graph only after an explicit editor action. */
            bool DrawEditorDiagnosticsAction(const ILocalizationService &locale, const bool ready) {
                const bool enabled = ready && captureEditorDiagnostics_ && contextLifetime_ && !contextProject_.empty();
                if (!Button(locale, "context_editor_diagnostics_offer", enabled))
                    return false;
                StartEditorDiagnosticsReview();
                return true;
            }

            /** @brief Transfers only an owned Problems capture into the authenticated MCP worker. */
            void StartEditorDiagnosticsReview() {
                constexpr auto Unavailable = "context_editor_diagnostics_unavailable";
                try {
                    if (auto capture = captureEditorDiagnostics_()) {
                        Platform::ProjectReadContext project{contextProject_, contextGeneration_, contextLifetime_->Token(),
                                                             std::chrono::steady_clock::now() + std::chrono::minutes{5}};
                        BackgroundTurn::Operation task{[snapshot = std::move(*capture), project = std::move(project),
                                                        requestId = turn_ + 1](std::stop_token stop) mutable -> Reply {
                            return ContextReview{EditorLocalAgentContext::
                                                     PrepareEditorDiagnostics(std::move(snapshot), std::move(project), requestId,
                                                                              Platform::CreateNativeSecureRandomSource(), stop),
                                                 "context_editor_diagnostics_unavailable"};
                        }};
                        if (background_->Start(std::move(task))) {
                            status_ = "context_editor_diagnostics_reading";
                            return;
                        }
                    }
                } catch (const std::bad_alloc &) {
                    // Preparing the bounded worker inputs may allocate.
                }
                status_ = Unavailable;
            }

            /** @brief Keeps clipboard reads behind one of the two explicit attachment choices. */
            bool DrawClipboardActions(const ILocalizationService &locale, const bool ready) {
                const bool enabled = ready && contextLifetime_ && !contextProject_.empty();
                if (Button(locale, "context_clipboard_note", enabled)) {
                    PrepareClipboardReview(AgentContextAttachmentKind::Note);
                    return true;
                }
                if (Button(locale, "context_clipboard_report", enabled)) {
                    PrepareClipboardReview(AgentContextAttachmentKind::Report);
                    return true;
                }
                return false;
            }

            /** @brief Freezes the selected clipboard bytes for redaction and one-request consent on the editor lane. */
            void PrepareClipboardReview(const AgentContextAttachmentKind kind) {
                auto prepared =
                    EditorLocalAgentContext::PrepareClipboardAttachment(kind, CaptureClipboardText(), contextProject_, contextGeneration_,
                                                                        contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = "context_clipboard_unavailable";
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            /** @brief Requests only the current bounded scene selection from the workspace owner. */
            void PrepareSceneSelectionReview() {
                const auto snapshot = captureSceneSelection_();
                if (!snapshot) {
                    status_ = "context_scene_selection_unavailable";
                    return;
                }
                auto prepared = EditorLocalAgentContext::PrepareSceneSelection(*snapshot, contextProject_, contextGeneration_,
                                                                               contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = "context_scene_selection_unavailable";
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            /** @brief Offers the independent active-tab, active-range and open-file selections. */
            bool DrawSourceContextActions(const ILocalizationService &locale, const bool ready) {
                const bool enabled = ready && contextLifetime_ && !contextProject_.empty();
                if (Button(locale, "context_offer", enabled && activeSource_)) {
                    PrepareSourceReview(false);
                    return true;
                }
                if (Button(locale, "context_range_offer", enabled && activeSourceRange_)) {
                    PrepareSourceReview(true);
                    return true;
                }
                if (Button(locale, "context_file_offer", enabled && openSources_)) {
                    sourcePage_ = 0;
                    selectedOpenFiles_.clear();
                    sourcePicker_ = SourcePickerMode::OpenTabs;
                    return true;
                }
                if (Button(locale, "context_saved_file_offer", enabled && projectFiles_ && !projectReadIdentity_.empty())) {
                    sourcePicker_ = SourcePickerMode::SavedFile;
                    return true;
                }
                return false;
            }

            /** @brief Shows the exact project-relative path and selects by the current tab's stable identity. */
            void DrawOpenSourceRow(const ILocalizationService &locale, const SourceDocumentSnapshot &source) {
                const auto identity = source.Identity();
                const auto id = std::to_string(identity.instance.Value());
                const auto &path = identity.key.source.Value();
                ImGui::PushID(id.c_str());
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(path.data(), path.data() + path.size());
                ImGui::PopTextWrapPos();
                const auto selected = std::ranges::find_if(selectedOpenFiles_, [&](const auto &file) {
                    return file.Identity().instance == identity.instance;
                });
                if (selected != selectedOpenFiles_.end()) {
                    if (Button(locale, "context_file_remove", true))
                        selectedOpenFiles_.erase(selected);
                } else if (Button(locale, "context_file_add",
                                  selectedOpenFiles_.size() < Security::AgentPolicyLimits{}.maximumContextItems)) {
                    selectedOpenFiles_.push_back(source);
                }
                ImGui::PopID();
            }

            /** @brief Browses existing workspace snapshots without opening files or changing the active tab. */
            void DrawSourcePicker(const ILocalizationService &locale) {
                constexpr std::size_t PageSize = 5;
                const auto sources = openSources_();
                if (sources.empty()) {
                    ImGui::TextWrapped("%s", LocalizedText(locale, "context_file_empty").c_str());
                } else {
                    const auto lastPage = (sources.size() - 1) / PageSize;
                    sourcePage_ = std::min(sourcePage_, lastPage);
                    const auto remaining = sources.subspan(sourcePage_ * PageSize);
                    for (const auto &source : remaining.first(std::min(remaining.size(), PageSize))) {
                        if (source.IsCurrent())
                            DrawOpenSourceRow(locale, source);
                    }
                    if (Button(locale, "context_file_previous", sourcePage_ > 0))
                        --sourcePage_;
                    if (Button(locale, "context_file_next", sourcePage_ < lastPage))
                        ++sourcePage_;
                }
                if (Button(locale, "context_files_preview", !selectedOpenFiles_.empty() && CanSendPrompt())) {
                    PrepareOpenFileReview();
                    return;
                }
                if (Button(locale, "context_cancel", true)) {
                    selectedOpenFiles_.clear();
                    sourcePicker_ = SourcePickerMode::None;
                }
            }

            /** @brief Offers one inseparable consent review for the explicitly selected file snapshots. */
            void PrepareOpenFileReview() {
                sourcePicker_ = SourcePickerMode::None;
                auto prepared = EditorLocalAgentContext::PrepareOpenFiles(std::exchange(selectedOpenFiles_, {}), contextProject_,
                                                                          contextGeneration_, contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = "context_file_unavailable";
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                reviewingOpenSource_ = true;
                status_ = nullptr;
            }

            /** @brief Selects one saved relative path without inspecting or opening any editor tab. */
            void DrawSavedFilePicker(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                ImGui::TextWrapped("%s", LocalizedText(locale, "context_saved_file_path").c_str());
                static_cast<void>(DrawTextField("##SavedProjectFile", savedFilePath_, 256, fonts));
                if (Button(locale, "context_file_select", !savedFilePath_.empty() && CanSendPrompt())) {
                    sourcePicker_ = SourcePickerMode::None;
                    StartSavedFileReview();
                    return;
                }
                if (Button(locale, "context_cancel", true))
                    sourcePicker_ = SourcePickerMode::None;
            }

            /** @brief Reads only after explicit selection, using the same cancelled-on-detach worker slot as inference. */
            void StartSavedFileReview() {
                try {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{5};
                    Platform::ProjectReadContext project{projectReadIdentity_, 1, contextLifetime_->Token(), deadline};
                    BackgroundTurn::Operation task{[files = projectFiles_, path = savedFilePath_, project = std::move(project),
                                                    requestId = turn_ + 1](std::stop_token stop) mutable -> Reply {
                        return ContextReview{EditorLocalAgentContext::PrepareSavedFile(std::move(files), path, std::move(project),
                                                                                       requestId, stop),
                                             "context_saved_file_unavailable"};
                    }};
                    if (background_->Start(std::move(task))) {
                        status_ = "context_saved_file_reading";
                        return;
                    }
                } catch (const std::bad_alloc &) {
                    // Copying the owned worker inputs may allocate.
                }
                status_ = "context_saved_file_unavailable";
            }

            void PrepareSourceReview(const bool selectedRange) {
                SourceDocumentSnapshot active;
                SourceContextRange range;
                const bool available = selectedRange ? activeSourceRange_(active, range) : activeSource_(active);
                if (!available) {
                    status_ = selectedRange ? "context_range_unavailable" : "context_unavailable";
                    return;
                }
                PrepareSourceSnapshot(std::move(active), selectedRange ? std::optional{range} : std::nullopt);
            }

            void PrepareSourceSnapshot(SourceDocumentSnapshot source, const std::optional<SourceContextRange> range) {
                auto prepared = EditorLocalAgentContext::Prepare(std::move(source), contextProject_, contextGeneration_,
                                                                 contextLifetime_->Token(), turn_ + 1, range);
                if (prepared.HasError()) {
                    status_ = "context_unavailable";
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                reviewingOpenSource_ = false;
                status_ = nullptr;
            }

            /** @brief Offers distinct explicit selections over the same committed project. */
            bool DrawBuildContextActions(const ILocalizationService &locale, const bool ready) {
                const bool enabled =
                    ready && buildOutput_ && gameplayBuilds_ && !buildRoot_.empty() && contextLifetime_ && !contextProject_.empty();
                if (Button(locale, "context_build_offer", enabled)) {
                    PrepareBuildReview(BuildContextChoice::ActiveOutput, "context_build_unavailable");
                    return true;
                }
                if (Button(locale, "context_active_build_diagnostics_offer", enabled)) {
                    PrepareBuildReview(BuildContextChoice::ActiveDiagnostics, "context_active_build_diagnostics_unavailable");
                    return true;
                }
                if (Button(locale, "context_completed_build_offer", enabled)) {
                    PrepareBuildReview(BuildContextChoice::CompletedOutput, "context_completed_build_unavailable");
                    return true;
                }
                if (Button(locale, "context_build_diagnostics_offer", enabled)) {
                    PrepareBuildReview(BuildContextChoice::CompletedDiagnostics, "context_build_diagnostics_unavailable");
                    return true;
                }
                return false;
            }

            /** @brief Resolves the separately selected active or terminal operation within the committed project. */
            std::optional<OperationId> BuildOperation(const BuildContextChoice choice) const {
                if (choice == BuildContextChoice::CompletedOutput || choice == BuildContextChoice::CompletedDiagnostics)
                    return gameplayBuilds_->QueryLatestCompletedOutput(buildRoot_);
                const auto build = gameplayBuilds_->QueryActiveProject(buildRoot_);
                return build ? build->operationId : std::nullopt;
            }

            void PrepareBuildReview(const BuildContextChoice choice, const char *unavailable) {
                constexpr std::size_t Records = 8;
                constexpr std::size_t Bytes = (3U << 10U) - Records;  // Leave room for framing each selected in-memory message.
                const auto operation = BuildOperation(choice);
                if (!operation) {
                    status_ = unavailable;
                    return;
                }
                const bool diagnostics =
                    choice == BuildContextChoice::ActiveDiagnostics || choice == BuildContextChoice::CompletedDiagnostics;
                auto tail = diagnostics ? buildOutput_->CaptureOperationDiagnostics(*operation, Records, Bytes)
                                        : buildOutput_->CaptureOperationTail(*operation, Records, Bytes);
                if (!tail) {
                    status_ = unavailable;
                    return;
                }
                buildContextChoice_ = choice;
                if (diagnostics) {
                    StartDiagnosticReview(std::move(*tail), unavailable);
                    return;
                }
                auto prepared = EditorLocalAgentContext::PrepareBuildOutput(std::move(*tail), contextProject_, contextGeneration_,
                                                                            contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = unavailable;
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            /** @brief Transfers only the selected owned capture into the authenticated MCP worker. */
            void StartDiagnosticReview(BuildOutputOperationTail tail, const char *unavailable) {
                try {
                    Platform::ProjectReadContext project{contextProject_, contextGeneration_, contextLifetime_->Token(),
                                                         std::chrono::steady_clock::now() + std::chrono::minutes{5}};
                    BackgroundTurn::Operation task{[tail = std::move(tail), project = std::move(project), requestId = turn_ + 1,
                                                    unavailable](std::stop_token stop) mutable -> Reply {
                        return ContextReview{EditorLocalAgentContext::PrepareBuildDiagnostics(std::move(tail), std::move(project),
                                                                                              requestId,
                                                                                              Platform::CreateNativeSecureRandomSource(),
                                                                                              stop),
                                             unavailable};
                    }};
                    if (background_->Start(std::move(task))) {
                        status_ = "context_build_diagnostics_reading";
                        return;
                    }
                } catch (const std::bad_alloc &) {
                    // Preparing the bounded worker inputs may allocate.
                }
                status_ = unavailable;
            }

            /** @brief Rechecks widget byte offsets only on approval, avoiding whole-document scans during preview drawing. */
            bool ReviewSelectionCurrent() const {
                if (const auto operation = contextPreview_->BuildOperation())
                    return BuildOperation(buildContextChoice_) == operation;
                const auto expected = contextPreview_->Selection();
                if (!expected)
                    return true;
                SourceDocumentSnapshot active;
                SourceContextRange range;
                return activeSourceRange_(active, range) && contextPreview_->Matches(active) && range == *expected;
            }

            /** @brief Validates the active tab for source previews or the exact operation's witness for output. */
            bool ContextReviewCurrent() const {
                if (!contextPreview_->RequiresOpenSource())
                    return contextPreview_->IsCurrent();
                if (reviewingOpenSource_)
                    return contextPreview_->MatchesOpenSource(openSources_());
                SourceDocumentSnapshot active;
                return activeSource_(active) && contextPreview_->Matches(active);
            }

            void DrawContextReview(const ILocalizationService &locale) {
                if (!ContextReviewCurrent()) {
                    contextPreview_.reset();
                    status_ = "context_stale";
                    return;
                }
                ImGui::TextWrapped("%s", LocalizedText(locale, "context_review").c_str());
                ImGui::PushTextWrapPos(0);
                for (const auto &item : contextPreview_->Items()) {
                    const auto &payload = item.candidate.text;
                    ImGui::TextUnformatted(payload.data(), payload.data() + payload.size());
                }
                ImGui::PopTextWrapPos();
                if (Button(locale, "context_approve", CanSendPrompt())) {
                    const bool selected = ReviewSelectionCurrent();
                    auto approved = std::exchange(contextPreview_, std::nullopt);
                    if (selected && approved->Approve())
                        Send(prompt_, {}, false, std::move(approved));
                    else
                        status_ = "context_stale";
                    return;
                }
                if (Button(locale, "context_cancel", true))
                    contextPreview_.reset();
            }

            /** @brief Prepares only complete model edits backed by the exact offered source. */
            void CapturePatchReview() {
                if (!patchSource_.turn || !requestSourceReview_ || !IsEditorLocalPatchReply(response_))
                    return;
                auto review = PrepareEditorLocalPatchReview(response_, patchSource_, contextProject_, contextGeneration_);
                if (review.HasError()) {
                    status_ = review.ErrorValue().code.Value() == Proposals::Errors::StaleBase.code.Value() ? "source_patch_stale"
                                                                                                            : "source_patch_invalid";
                    return;
                }
                patchReview_.emplace(std::move(review).Value());
            }

            /** @brief Renders one immutable file summary and checks that its source review remains available. */
            const char *DrawPatchFile(const EditorPatchFileCard &file,
                                      const std::variant<Proposals::FilePreview, Proposals::PlanConflict> &plan,
                                      const ILocalizationService &locale, const Theme::Fonts &fonts) const {
                const auto width = ImGui::GetContentRegionAvail().x;
                Ui::WrappedText(width, LocalizedText(locale, PatchFileLabel(file.kind)), Theme::Text(), fonts);
                Ui::WrappedText(width, file.path.Value(), Theme::Muted(), fonts);
                if (file.destination)
                    Ui::WrappedText(width, file.destination->Value(), Theme::Muted(), fonts);
                const auto counts =
                    std::format("+{} / -{} {} · {} {}", file.addedBytes, file.removedBytes, LocalizedText(locale, "patch_file_bytes"),
                                file.hunks, LocalizedText(locale, "patch_file_ranges"));
                Ui::WrappedText(width, counts, Theme::Muted(), fonts);
                if (const auto *conflict = std::get_if<Proposals::PlanConflict>(&plan))
                    return PatchConflictLabel(conflict->reason);
                const auto &source = std::get<Proposals::FilePreview>(plan).before;
                return source.Revision() && !source.IsCurrent() ? "source_patch_stale" : nullptr;
            }

            /** @brief Records an explicit review choice against the current plan. */
            void DecidePatch(EditorLocalAgentPatchReview &review, const Proposals::ReviewTarget target,
                             const Proposals::ReviewChoice choice) {
                const auto result = review.decisions.Decide(*review.plan, target, choice);
                status_ = result.HasError() ? "source_patch_stale" : nullptr;
            }

            /** @brief Draws review decisions for one canonical file or hunk. */
            void DrawUnitDecisions(EditorLocalAgentPatchReview &review, const Proposals::ReviewTarget target,
                                   const Proposals::ReviewState state, const bool conflict, const ILocalizationService &locale) {
                const auto *accept = target.hunk ? "source_patch_accept_hunk" : "source_patch_accept_file";
                const auto *reject = target.hunk ? "source_patch_reject_hunk" : "source_patch_reject_file";
                if (Button(locale, accept, !conflict && state != Proposals::ReviewState::Accepted))
                    DecidePatch(review, target, Proposals::ReviewChoice::Accept);
                if (Button(locale, reject, state != Proposals::ReviewState::Rejected))
                    DecidePatch(review, target, Proposals::ReviewChoice::Reject);
            }

            /** @brief Navigates the bounded list while retaining the exact proposal hunk index. */
            void DrawHunkNavigation(EditorLocalAgentPatchReview &review, const std::size_t count, const ILocalizationService &locale) {
                if (Button(locale, "source_patch_previous_hunk", review.displayedHunk != 0))
                    --review.displayedHunk;
                if (Button(locale, "source_patch_next_hunk", review.displayedHunk + 1 < count))
                    ++review.displayedHunk;
            }

            /** @brief Displays one hunk's original byte range without editing or retaining its text in the view. */
            void DrawHunkReview(EditorLocalAgentPatchReview &review, const std::size_t file, const bool conflict,
                                const ILocalizationService &locale, const Theme::Fonts &fonts) {
                const auto *edit = std::get_if<Proposals::EditFile>(&review.proposal.Files()[file]);
                if (!edit)
                    return;
                if (Button(locale, "source_patch_hunks", true)) {
                    review.expandedFile = review.expandedFile == file ? std::nullopt : std::optional{file};
                    review.displayedHunk = 0;
                }
                if (review.expandedFile != file)
                    return;
                DrawHunkNavigation(review, edit->hunks.size(), locale);
                const auto index = review.displayedHunk;
                const auto &hunk = edit->hunks[index];
                const auto range =
                    std::format("{} {}/{} · {} {}–{}", LocalizedText(locale, "source_patch_range"), index + 1, edit->hunks.size(),
                                LocalizedText(locale, "source_patch_offset"), hunk.offset, hunk.offset + hunk.before.size());
                const auto state = review.decisions.State({file, index}).Value();
                const auto width = ImGui::GetContentRegionAvail().x;
                Ui::WrappedText(width, range, Theme::Text(), fonts);
                Ui::WrappedText(width, LocalizedText(locale, PatchDecisionLabel(state)), Theme::Muted(), fonts);
                DrawUnitDecisions(review, {file, index}, state, conflict, locale);
            }

            /** @brief Draws one file's review state and stages its operation when available. */
            void DrawPatchCard(EditorLocalAgentPatchReview &review, const std::size_t index, const ILocalizationService &locale,
                               const Theme::Fonts &fonts, EditorWorkspaceViewCommandData &command) {
                const auto &file = review.files[index];
                const auto *conflict = DrawPatchFile(file, review.plan->entries[index], locale, fonts);
                using enum Proposals::ReviewState;
                const auto state = review.decisions.State({index}).Value();
                const bool rejected = state == Rejected;
                const auto *status = conflict && !rejected ? conflict : PatchDecisionLabel(state);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, status),
                                conflict && !rejected ? Theme::ConsoleWarning() : Theme::Muted(), fonts);
                const bool canOpen = !rejected && !conflict && file.kind != EditorPatchFileKind::Create;
                if (Button(locale, "source_patch_review", canOpen))
                    status_ = requestSourceReview_(review.proposal, *review.plan, index, command) ? nullptr : "source_patch_unavailable";
                DrawUnitDecisions(review, {index}, state, conflict != nullptr, locale);
                DrawHunkReview(review, index, conflict != nullptr, locale, fonts);
            }

            /** @brief Shows atomic proposal actions independently of per-file decisions. */
            void DrawPatchDecisions(EditorLocalAgentPatchReview &review, const ILocalizationService &locale) {
                using enum Proposals::ReviewState;
                const auto state = review.decisions.State().Value();
                if (Button(locale, "source_patch_accept_all", state != Accepted))
                    DecidePatch(review, {}, Proposals::ReviewChoice::Accept);
                if (Button(locale, "source_patch_reject", state != Rejected))
                    DecidePatch(review, {}, Proposals::ReviewChoice::Reject);
                if (Button(locale, "source_patch_undo", review.decisions.CanUndo()))
                    status_ = review.decisions.Undo(*review.plan).HasError() ? "source_patch_stale" : nullptr;
            }

            /** @brief Rebuilds the exact offered-file policy before staging the explicit application. */
            void QueueSourceApply(const EditorLocalAgentPatchReview &review, EditorWorkspaceViewCommandData &command) {
                try {
                    std::vector<Proposals::PathRule> rules;
                    rules.reserve(review.offeredPaths.size());
                    for (const auto &path : review.offeredPaths)
                        rules.push_back({path, false});
                    const auto policy =
                        Proposals::ProjectPolicy::Capture(contextProject_, review.proposal.PolicyRevision(), std::move(rules));
                    sourceApplyQueued_ =
                        policy.HasValue() && contextLifetime_ &&
                        requestSourceReviewApply_(review.decisions, policy.Value(), review.plan, contextLifetime_->Token(), command);
                } catch (const std::bad_alloc &) {
                    sourceApplyQueued_ = false;
                }
                status_ = sourceApplyQueued_ ? nullptr : "source_patch_apply_failed";
            }

            /** @brief Presents the workspace-owned result and a single action to reverse the applied group. */
            void DrawSourceTransaction(const ILocalizationService &locale, const Theme::Fonts &fonts,
                                       EditorWorkspaceViewCommandData &command) {
                if (!sourceReviewTransaction_)
                    return;
                const auto state = sourceReviewTransaction_();
                using enum Proposals::SourceTransactionFeedback;
                if (state.feedback != Preparing && std::exchange(sourceApplyQueued_, false) && state.feedback == Applied)
                    patchReview_.reset();
                constexpr std::array<const char *, 8> keys{nullptr,
                                                           "source_patch_applied",
                                                           "source_patch_reverted",
                                                           "source_patch_apply_failed",
                                                           "source_patch_revert_failed",
                                                           "source_patch_transaction_conflict",
                                                           "source_patch_transaction_cancelled",
                                                           "source_patch_transaction_preparing"};
                if (const auto *key = keys[static_cast<std::size_t>(state.feedback)])
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, key), Theme::Text(), fonts);
                if (state.audit)
                    DrawSourceAudit(*state.audit, locale, fonts);
                if (state.auditHistoryError)
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "source_patch_audit_history_error"),
                                    Theme::Text(), fonts);
                if (requestSourceReviewUndo_ && contextLifetime_ && state.canUndo && Button(locale, "source_patch_revert", true))
                    status_ = requestSourceReviewUndo_(contextLifetime_->Token(), command) ? nullptr : "source_patch_revert_failed";
            }

            /** @brief Presents the content-free proposal identifier for the latest successful application. */
            void DrawSourceAudit(const Proposals::SourceReviewAudit &audit, const ILocalizationService &locale, const Theme::Fonts &fonts) {
                const auto label =
                    std::format("{} {}", LocalizedText(locale, "source_patch_audit_proposal"), FormatSha256(audit.proposal.digest));
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, label, Theme::Muted(), fonts);
            }

            /** @brief Explicitly checkpoints the exact inert proposal and current unit decisions on the storage worker. */
            void SavePatchReview(const EditorLocalAgentPatchReview &review) {
                EditorPatchCheckpoint checkpoint{review.proposal, review.decisions, review.offeredPaths};
                const auto started =
                    patchCheckpoint_.Start(PatchWorker::Operation{[root = buildRoot_, files = &files_, checkpoint = std::move(checkpoint)](
                                                                      const std::stop_token stop) -> PatchWorker::Reply {
                    const CancellationSource cancellation;
                    const std::stop_callback cancel{stop, [&] {
                        cancellation.RequestCancellation();
                    }};
                    return SaveEditorPatchCheckpoint(*files, root, checkpoint, cancellation.Token());
                }});
                status_ = started ? "working" : "source_patch_checkpoint_error";
            }

            /** @brief Captures current immutable source evidence before restoring the last explicit checkpoint. */
            void LoadPatchReview() {
                const auto snapshots = openSources_();
                std::vector<SourceDocumentSnapshot> sources{snapshots.begin(), snapshots.end()};
                const auto started = patchCheckpoint_.Start(
                    PatchWorker::Operation{[root = buildRoot_, project = contextProject_,
                                            sources = std::move(sources)](const std::stop_token stop) -> PatchWorker::Reply {
                    const CancellationSource cancellation;
                    const std::stop_callback cancel{stop, [&] {
                        cancellation.RequestCancellation();
                    }};
                    return LoadEditorPatchCheckpoint(root, project, sources, cancellation.Token());
                }});
                status_ = started ? "working" : "source_patch_checkpoint_error";
            }

            /** @brief Adopts storage completion only for the project incarnation that initiated it. */
            void PollPatchCheckpoint() {
                using State = PatchWorker::CompletionState;
                auto result = patchCheckpoint_.Poll();
                if (!result || result->state == State::Abandoned)
                    return;
                if (result->state != State::Ready) {
                    status_ = "source_patch_checkpoint_error";
                    return;
                }
                std::visit([this](auto &reply) {
                    AdoptPatchCheckpoint(reply);
                }, *result->reply);
            }

            /** @brief Reports whether the explicitly selected proposal and decisions reached durable storage. */
            void AdoptPatchCheckpoint(const Result<void> &saved) {
                status_ = saved.HasValue() ? "source_patch_checkpoint_saved" : "source_patch_checkpoint_error";
            }

            /** @brief Displays revalidated decisions without dispatching a model turn or editing source. */
            void AdoptPatchCheckpoint(PatchLoad &loaded) {
                if (loaded.HasError()) {
                    status_ = loaded.ErrorValue().code.Value() == Proposals::Errors::StaleBase.code.Value()
                                  ? "source_patch_checkpoint_stale"
                                  : "source_patch_checkpoint_error";
                    return;
                }
                if (!loaded.Value()) {
                    status_ = "source_patch_checkpoint_missing";
                    return;
                }
                patchReview_ = std::move(loaded).Value();
                response_.clear();
                status_ = "source_patch_checkpoint_loaded";
            }

            /** @brief Requires an attached project and an idle review lane for explicit checkpoint actions. */
            bool CanCheckpointPatchReview() const {
                return !Busy() && !patchCheckpoint_.Busy() && !awaitingCheckpoint_ && !contextPreview_ && !buildRoot_.empty() &&
                       !contextProject_.empty();
            }

            /** @brief Offers restoration when there is an attached project and no active source review. */
            void DrawPatchRestore(const ILocalizationService &locale) {
                if (openSources_ && requestSourceReview_ && !buildRoot_.empty() &&
                    Button(locale, "source_patch_checkpoint_load", CanCheckpointPatchReview()))
                    LoadPatchReview();
            }

            /** @brief Renders file summaries in proposal order and stages only the selected current operation. */
            void DrawPatchReview(const ILocalizationService &locale, const Theme::Fonts &fonts, EditorWorkspaceViewCommandData &command) {
                if (!patchReview_) {
                    DrawPatchRestore(locale);
                    return;
                }
                ImGui::BeginDisabled(patchCheckpoint_.Busy() || sourceApplyQueued_);
                auto &review = *patchReview_;
                for (std::size_t index = 0; index < review.files.size(); ++index) {
                    if (review.files.size() > 1)
                        ImGui::PushID(static_cast<int>(index));
                    DrawPatchCard(review, index, locale, fonts, command);
                    if (review.files.size() > 1)
                        ImGui::PopID();
                }
                DrawPatchDecisions(review, locale);
                const bool writingAudit = sourceReviewTransaction_ && sourceReviewTransaction_().writingAudit;
                if (requestSourceReviewApply_ &&
                    Button(locale, "source_patch_apply", !writingAudit && review.decisions.HasAcceptedChanges()))
                    QueueSourceApply(review, command);
                if (!buildRoot_.empty() && Button(locale, "source_patch_checkpoint_save", CanCheckpointPatchReview()))
                    SavePatchReview(review);
                ImGui::EndDisabled();
            }

            void Send(std::string_view prompt, std::optional<std::size_t> sceneCount = {}, bool recoverable = false,
                      std::optional<EditorLocalAgentContext> source = {}) {
                ModelRequest request;
                request.model = model_;
                request.messages.push_back({.role = ModelRole::User, .text = std::string{prompt}});
                history_.ClearReview();
                const bool plain = !recoverable && !sceneCount && !source;
                auto replay = plain ? std::optional{PlainReplay{endpoint_, model_, request.messages.front().text}} : std::nullopt;
                submittedPrompt_ = plain ? std::optional{request.messages.front().text} : std::nullopt;
                submittedReference_.reset();
                if (source)
                    submittedReference_ = source->HistoryReference();
                else if (sceneCount)
                    submittedReference_ = AgentHistoryReference{AgentHistoryReferenceKind::ToolCall, turn_ + 1, contextGeneration_};
                submittedProject_ = history_.Project();
                if (Start(std::move(request), sceneCount, std::move(source)))
                    plainReplay_ = std::move(replay);
                else if (recoverable)
                    history_.FinishRecovery(AgentHistoryMessageState::Failed);
            }

            [[nodiscard]] bool Start(std::optional<ModelRequest> request, std::optional<std::size_t> sceneCount = {},
                                     std::optional<EditorLocalAgentContext> source = {}) {
                try {
                    auto offeredSources = source ? source->PatchSources() : std::vector<SourceDocumentSnapshot>{};
                    auto offeredModel = request && !offeredSources.empty() ? request->model : std::string{};
                    auto draft = request && !sceneCount ? std::make_shared<AgentHistoryLiveDraft>() : nullptr;
                    auto sceneQuery = sceneCount ? std::make_shared<SceneQueryProgress>(SceneQueryPhase::Idle) : nullptr;
                    BackgroundTurn::Operation task{[endpoint = endpoint_, request = std::move(request), sceneCount,
                                                    source = std::move(source), turn = ++turn_, draft,
                                                    sceneQuery](std::stop_token stop) mutable {
                        return RunLocalOperation(std::move(endpoint), std::move(request), sceneCount, std::move(source), turn, draft,
                                                 sceneQuery, stop);
                    }};
                    if (background_->Start(std::move(task))) {
                        liveDraft_ = std::move(draft);
                        sceneQuery_ = std::move(sceneQuery);
                        displayedDraftBytes_ = 0;
                        status_ = "working";
                        response_.clear();
                        patchSource_ = {std::move(offeredSources), std::move(offeredModel), turn_};
                        if (patchSource_.sources.empty())
                            patchSource_.turn = 0;
                        patchReview_.reset();
                        return true;
                    }
                } catch (const std::bad_alloc &) {
                    // Preparing the value-only worker request may allocate.
                }
                status_ = "failed";
                submittedPrompt_.reset();
                submittedReference_.reset();
                return false;
            }

            /** @brief Keeps the latest safe prefix when a transient display allocation fails. */
            void PollLiveResponse() {
                if (!liveDraft_)
                    return;
                try {
                    if (auto updated = liveDraft_->DisplayIfChanged(displayedDraftBytes_))
                        response_ = std::move(*updated);
                } catch (const std::bad_alloc &) {
                    // Live display is optional while the worker still owns the terminal response.
                }
            }

            void PollReply() {
                auto completion = background_->Poll();
                if (!completion)
                    return;
                liveDraft_.reset();
                if (completion->state != BackgroundTurn::CompletionState::Abandoned) {
                    try {
                        AdoptCompletion(*completion);
                    } catch (const std::exception &) {
                        FailTurn();
                    }
                }
                submittedPrompt_.reset();
                submittedReference_.reset();
            }

            void AdoptCompletion(BackgroundTurn::Completion &completion) {
                using State = BackgroundTurn::CompletionState;
                if (completion.state == State::Cancelled) {
                    status_ = "cancelled";
                    history_.FinishRecovery(AgentHistoryMessageState::Cancelled);
                    return;
                }
                if (completion.state == State::Failed) {
                    FailTurn();
                    return;
                }
                std::visit([this](auto &result) {
                    Adopt(result);
                }, *completion.reply);
            }

            void FailTurn() {
                status_ = "failed";
                history_.FinishRecovery(AgentHistoryMessageState::Failed);
            }

            void Adopt(ProviderDiscovery &discovery) {
                if (!discovery.outcome.Succeeded()) {
                    models_.clear();
                    status_ = FailureKey(discovery.outcome.error->code);
                    return;
                }
                std::erase_if(discovery.models, [](const ModelDescriptor &model) {
                    return model.id.empty() || model.id.size() > 256;
                });
                discovery.models.resize(std::min(discovery.models.size(), std::size_t{64}));
                models_ = std::move(discovery.models);
                selection_ = -1;
                status_ = models_.empty() ? "no_models" : nullptr;
            }

            void Adopt(AgentTurnResult &result) {
                if (result.status == AgentTurnStatus::ContextDenied) {
                    status_ = result.contextError == Security::AgentPolicyReason::StaleAuthority ? "context_stale" : "context_unavailable";
                    return;
                }
                if (result.status == AgentTurnStatus::Completed && !result.messages.empty()) {
                    response_ = std::move(result.messages.back().text);
                    status_ = response_.empty() ? "empty_response" : nullptr;
                    CapturePatchReview();
                    ReviewCompleted(result);
                    return;
                }
                status_ = result.status == AgentTurnStatus::TimedOut ? "timeout" : FailureKey(result.providerError);
                history_.FinishRecovery(result.status == AgentTurnStatus::Cancelled ? AgentHistoryMessageState::Cancelled
                                                                                    : AgentHistoryMessageState::Failed);
            }

            /** @brief Publishes the completed worker preview for explicit consent on the editor lane. */
            void Adopt(ContextReview &review) {
                if (review.outcome.HasError()) {
                    status_ = review.unavailable;
                    return;
                }
                contextPreview_.emplace(std::move(review.outcome).Value());
                status_ = nullptr;
            }

            /** @brief Keeps context-bearing replies ephemeral and proposes only host-issued reference metadata. */
            void ReviewCompleted(const AgentTurnResult &result) {
                if (submittedReference_) {
                    if (submittedReference_->kind != AgentHistoryReferenceKind::ToolCall ||
                        std::ranges::any_of(result.messages, [](const ModelMessage &message) {
                        return message.role == ModelRole::Tool;
                    }))
                        history_.ReviewReference(submittedProject_, *submittedReference_);
                    return;
                }
                const auto reported = result.finalUsage.value_or(ModelUsage{});
                const auto usage = AgentHistoryUsage::FromReported(reported.inputTokens, reported.outputTokens);
                if (history_.HasRecovery())
                    history_.ReviewRecoveryAnswer(response_, usage);
                else if (!response_.empty() && submittedPrompt_)
                    history_.Review(submittedProject_, *submittedPrompt_, response_, usage);
            }

            std::string endpoint_;
            std::string model_;
            std::string prompt_;
            std::string response_;
            std::vector<ModelDescriptor> models_;
            std::function<std::optional<std::size_t>()> sceneCount_;
            std::function<std::optional<AgentSceneSelectionSnapshot>()> captureSceneSelection_;
            std::function<std::optional<AgentEditorDiagnosticsSnapshot>()> captureEditorDiagnostics_;
            std::function<bool(SourceDocumentSnapshot &)> activeSource_;
            std::function<bool(SourceDocumentSnapshot &, SourceContextRange &)> activeSourceRange_;
            std::function<std::span<const SourceDocumentSnapshot>()> openSources_;
            decltype(PanelContext::requestSourceReview) requestSourceReview_;
            decltype(PanelContext::requestSourceReviewApply) requestSourceReviewApply_;
            decltype(PanelContext::requestSourceReviewUndo) requestSourceReviewUndo_;
            decltype(PanelContext::sourceReviewTransaction) sourceReviewTransaction_;
            bool sourceApplyQueued_{};
            EditorLocalAgentPatchSource patchSource_;
            std::optional<EditorLocalAgentPatchReview> patchReview_;
            std::shared_ptr<const Platform::IProjectReadFiles> projectFiles_;
            std::string projectReadIdentity_;
            std::string savedFilePath_;
            const IBuildOutputQuery *buildOutput_{};
            const Application::GameplayBuildService *gameplayBuilds_{};
            std::filesystem::path buildRoot_;
            std::string contextProject_;
            std::uint64_t contextGeneration_{};
            std::optional<CancellationSource> contextLifetime_;
            std::optional<EditorLocalAgentContext> contextPreview_;
            std::shared_ptr<BackgroundTurn> background_;
            EditorLocalAgentHistory history_;
            std::shared_ptr<AgentHistoryLiveDraft> liveDraft_;
            std::shared_ptr<SceneQueryProgress> sceneQuery_;
            std::size_t displayedDraftBytes_{};
            std::optional<PlainReplay> plainReplay_;
            Input::InputRouter *inputRouter_{};
            Input::InputContextToken textInput_;
            bool textFieldActive_{};
            std::optional<std::string> submittedPrompt_;
            std::optional<AgentHistoryReference> submittedReference_;
            AgentHistoryProjectRevision submittedProject_;
            int selection_{-1};
            const char *status_{};
            std::uint64_t turn_{};
            bool awaitingCheckpoint_{};
            BuildContextChoice buildContextChoice_{BuildContextChoice::ActiveOutput};
            SourcePickerMode sourcePicker_{SourcePickerMode::None};
            bool reviewingOpenSource_{};
            std::size_t sourcePage_{};
            std::vector<SourceDocumentSnapshot> selectedOpenFiles_;
            DurableFileSystem &files_;
            PatchWorker patchCheckpoint_; /**< Joins before panel teardown while the host's file service is alive. */
        };
    }  // namespace

    /** @copydoc MakeEditorLocalAgentPanel */
    EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files) {
        auto background = std::make_shared<BackgroundTurn>();
        auto panel = std::make_shared<LocalAgentPanel>(jobs, files, background);
        return {panel, [background = std::move(background), owner = std::weak_ptr{panel}] {
            if (const auto current = owner.lock()) {
                current->Poll();
            } else {
                background->Abandon();
                static_cast<void>(background->Poll());
            }
        }};
    }
}  // namespace Horo::Editor
