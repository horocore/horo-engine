#include "EditorLocalAgentPanel.h"

#include "AgentBackgroundTurn.h"
#include "AgentLocalSceneTurn.h"
#include "AgentTurn.h"
#include "EditorLocalAgentContext.h"
#include "EditorLocalAgentHistory.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stop_token>
#include <utility>
#include <variant>

namespace Horo::Editor {
    namespace {
        using namespace Agent;
        using Reply = std::variant<ProviderDiscovery, AgentTurnResult>;
        using BackgroundTurn = AgentBackgroundTurn<Reply>;

        /** @brief Runs one user-initiated discovery or inference on an isolated worker. */
        Reply RunLocalOperation(std::string endpoint, std::optional<ModelRequest> request, std::optional<std::size_t> sceneCount,
                                std::optional<EditorLocalAgentContext> source, std::uint64_t turn,
                                const std::shared_ptr<AgentHistoryLiveDraft> &draft, std::stop_token stop) {
            ModelProviderConfig config{.endpoint = std::move(endpoint)};
            if (request && sceneCount.has_value())
                return RunApprovedLocalSceneTurn(std::move(config), std::move(*request), turn, *sceneCount,
                                                 Platform::CreateNativeSecureRandomSource(), stop);
            auto provider = CreateOllamaModelProvider(std::move(config));
            if (!request)
                return provider->Discover(stop);
            AgentTurnRuntime runtime;
            if (source) {
                const auto dispatch = source->Dispatch();
                return runtime.Run(*provider, nullptr, turn, std::move(*request), {}, stop, &dispatch);
            }
            if (draft)
                return runtime.Run(*provider, nullptr, turn, std::move(*request), {}, stop, nullptr, [&draft](std::string_view delta) {
                    draft->Append(delta);
                });
            return runtime.Run(*provider, nullptr, turn, std::move(*request), {}, stop);
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
                : IWorkspacePanel("workspace.local_agent"), background_(std::move(background)), history_(jobs, files) {}

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
                background_->Abandon();
                activeSource_ = context.activeSourceSnapshot;
                activeSourceRange_ = context.activeSourceRange;
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

            /** @brief Advances result publication and project storage after the workspace's runtime update. */
            void Poll() {
                if (history_.Poll()) {
                    if (std::exchange(awaitingCheckpoint_, false))
                        Send({}, true);
                    else
                        history_.FinishRecovery(AgentHistoryMessageState::Incomplete);
                } else if (awaitingCheckpoint_ && !history_.RecoveryStartPending()) {
                    awaitingCheckpoint_ = false;
                }
                PollReply();
            }

            void DrawIcon(ImDrawList *draw, const ImVec2 &position, const ImVec2 &size, ImU32 color) override {
                const ImVec2 low{position.x + size.x * 0.15F, position.y + size.y * 0.2F};
                const ImVec2 high{position.x + size.x * 0.85F, position.y + size.y * 0.7F};
                draw->AddRect(low, high, color, size.x * 0.12F);
                draw->AddLine({low.x + size.x * 0.15F, high.y}, {low.x + size.x * 0.15F, high.y + size.y * 0.12F}, color);
            }

            void DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &, EditorWorkspaceViewCommandData &,
                           const EditorGuiContext &context) override {
                const auto &locale = context.localization;
                const auto &fonts = context.theme.fonts;
                ImGui::BeginChild("##LocalAgent", size, false, ImGuiWindowFlags_NoSavedSettings);
                DrawProvider(locale, fonts);
                ImGui::TextUnformatted(LocalizedText(locale, "prompt").c_str());
                ImGui::BeginDisabled(awaitingCheckpoint_ || contextPreview_);
                static_cast<void>(Ui::InputTextControl("##Prompt", prompt_, 4096, fonts));
                ImGui::EndDisabled();
                DrawSendActions(locale);
                if (Busy() && Button(locale, "stop", background_->CanCancel()))
                    background_->Cancel();
                if (status_)
                    ImGui::TextWrapped("%s", LocalizedText(locale, status_).c_str());
                if (!response_.empty())
                    ImGui::TextWrapped("%s", response_.c_str());
                if (liveDraft_ && background_->CanCancel() && Button(locale, "history_draft_offer", true))
                    history_.ReviewLiveDraft(*liveDraft_);
                history_.Draw(context);
                ImGui::EndChild();
            }

        private:
            [[nodiscard]] bool Busy() const {
                return background_->Busy();
            }

            void RevokeContext() noexcept {
                if (contextLifetime_)
                    contextLifetime_->RequestCancellation();
                contextLifetime_.reset();
                contextPreview_.reset();
                activeSource_ = {};
                activeSourceRange_ = {};
                buildOutput_ = nullptr;
                gameplayBuilds_ = nullptr;
                buildRoot_.clear();
                contextProject_.clear();
            }

            [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled) const {
                const auto label = LocalizedText(locale, key) + "##" + key;
                return Ui::Button({.label = label.c_str(), .enabled = enabled});
            }

            void DrawProvider(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                ImGui::TextUnformatted(LocalizedText(locale, "endpoint").c_str());
                ImGui::BeginDisabled(Busy() || awaitingCheckpoint_ || history_.HasRecovery() || contextPreview_);
                if (Ui::InputTextControl("##Endpoint", endpoint_, 256, fonts)) {
                    models_.clear();
                    selection_ = -1;
                    response_.clear();
                    status_ = nullptr;
                }
                if (Button(locale, "discover", !Busy() && !endpoint_.empty()))
                    static_cast<void>(Start(std::nullopt));
                ImGui::TextUnformatted(LocalizedText(locale, "model").c_str());
                if (Ui::InputTextControl("##Model", model_, 256, fonts))
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

            void DrawSendActions(const ILocalizationService &locale) {
                if (contextPreview_) {
                    DrawContextReview(locale);
                    return;
                }
                const bool ready =
                    !Busy() && !awaitingCheckpoint_ && !history_.HasRecovery() && !endpoint_.empty() && !model_.empty() && !prompt_.empty();
                if (Button(locale, "ask", ready)) {
                    Send({});
                    return;
                }
                if (Button(locale, "history_recovery_offer", ready && history_.CanReviewRecovery())) {
                    awaitingCheckpoint_ = history_.ReviewRecoveryPrompt(prompt_);
                    return;
                }
                if (Button(locale, "context_offer", ready && activeSource_ && contextLifetime_ && !contextProject_.empty())) {
                    PrepareSourceReview(false);
                    return;
                }
                if (Button(locale, "context_range_offer", ready && activeSourceRange_ && contextLifetime_ && !contextProject_.empty())) {
                    PrepareSourceReview(true);
                    return;
                }
                if (Button(locale, "context_build_offer",
                           ready && buildOutput_ && gameplayBuilds_ && !buildRoot_.empty() && contextLifetime_ &&
                               !contextProject_.empty())) {
                    PrepareBuildReview();
                    return;
                }
                const auto count = sceneCount_ ? sceneCount_() : std::nullopt;
                if (count.has_value()) {
                    ImGui::Text("%s %zu", LocalizedText(locale, "scene_count").c_str(), *count);
                    if (Button(locale, "ask_with_scene_count", ready))
                        Send(count);
                }
            }

            void PrepareSourceReview(const bool selectedRange) {
                SourceDocumentSnapshot active;
                SourceContextRange range;
                const bool available = selectedRange ? activeSourceRange_(active, range) : activeSource_(active);
                if (!available) {
                    status_ = selectedRange ? "context_range_unavailable" : "context_unavailable";
                    return;
                }
                auto prepared =
                    EditorLocalAgentContext::Prepare(std::move(active), contextProject_, contextGeneration_, contextLifetime_->Token(),
                                                     turn_ + 1, selectedRange ? std::optional{range} : std::nullopt);
                if (prepared.HasError()) {
                    status_ = "context_unavailable";
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            /** @brief Uses only the existing gameplay service's exact active-project operation. */
            std::optional<OperationId> ActiveBuildOperation() const {
                const auto build = gameplayBuilds_->QueryActiveProject(buildRoot_);
                return build ? build->operationId : std::nullopt;
            }

            void PrepareBuildReview() {
                constexpr std::size_t Records = 8;
                constexpr std::size_t Bytes = (3U << 10U) - Records;  // Leave room for framing each selected in-memory message.
                const auto operation = ActiveBuildOperation();
                auto tail = operation ? buildOutput_->CaptureOperationTail(*operation, Records, Bytes) : std::nullopt;
                if (!tail) {
                    status_ = "context_build_unavailable";
                    return;
                }
                auto prepared = EditorLocalAgentContext::PrepareBuildOutput(std::move(*tail), contextProject_, contextGeneration_,
                                                                            contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = "context_build_unavailable";
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            /** @brief Rechecks widget byte offsets only on approval, avoiding whole-document scans during preview drawing. */
            bool ReviewSelectionCurrent() const {
                if (const auto operation = contextPreview_->BuildOperation())
                    return ActiveBuildOperation() == operation;
                const auto expected = contextPreview_->Selection();
                if (!expected)
                    return true;
                SourceDocumentSnapshot active;
                SourceContextRange range;
                return activeSourceRange_(active, range) && contextPreview_->Matches(active) && range == *expected;
            }

            /** @brief Validates the active tab for source previews or the exact operation's witness for output. */
            bool ContextReviewCurrent() const {
                if (contextPreview_->BuildOperation())
                    return contextPreview_->IsCurrent();
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
                if (Button(locale, "context_approve", !Busy())) {
                    const bool selected = ReviewSelectionCurrent();
                    auto approved = std::exchange(contextPreview_, std::nullopt);
                    if (selected && approved->Approve())
                        Send({}, false, std::move(approved));
                    else
                        status_ = "context_stale";
                    return;
                }
                ImGui::SameLine();
                if (Button(locale, "context_cancel", true))
                    contextPreview_.reset();
            }

            void Send(std::optional<std::size_t> sceneCount, bool recoverable = false, std::optional<EditorLocalAgentContext> source = {}) {
                ModelRequest request;
                request.model = model_;
                request.messages.push_back({.role = ModelRole::User, .text = prompt_});
                history_.ClearReview();
                submittedPrompt_ = recoverable || sceneCount.has_value() || source ? std::nullopt : std::optional{prompt_};
                submittedReference_.reset();
                if (source)
                    submittedReference_ = source->HistoryReference();
                else if (sceneCount)
                    submittedReference_ = AgentHistoryReference{AgentHistoryReferenceKind::ToolCall, turn_ + 1, contextGeneration_};
                submittedProject_ = history_.Project();
                if (!Start(std::move(request), sceneCount, std::move(source), recoverable) && recoverable)
                    history_.FinishRecovery(AgentHistoryMessageState::Failed);
            }

            [[nodiscard]] bool Start(std::optional<ModelRequest> request, std::optional<std::size_t> sceneCount = {},
                                     std::optional<EditorLocalAgentContext> source = {}, const bool recoverable = false) {
                try {
                    auto draft = recoverable ? std::make_shared<AgentHistoryLiveDraft>() : nullptr;
                    BackgroundTurn::Operation task{[endpoint = endpoint_, request = std::move(request), sceneCount,
                                                    source = std::move(source), turn = ++turn_, draft](std::stop_token stop) mutable {
                        return RunLocalOperation(std::move(endpoint), std::move(request), sceneCount, std::move(source), turn, draft, stop);
                    }};
                    if (background_->Start(std::move(task))) {
                        liveDraft_ = std::move(draft);
                        status_ = "working";
                        response_.clear();
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
                    ReviewCompleted(result);
                    return;
                }
                status_ = result.status == AgentTurnStatus::TimedOut ? "timeout" : FailureKey(result.providerError);
                history_.FinishRecovery(result.status == AgentTurnStatus::Cancelled ? AgentHistoryMessageState::Cancelled
                                                                                    : AgentHistoryMessageState::Failed);
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
            std::function<bool(SourceDocumentSnapshot &)> activeSource_;
            std::function<bool(SourceDocumentSnapshot &, SourceContextRange &)> activeSourceRange_;
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
            std::optional<std::string> submittedPrompt_;
            std::optional<AgentHistoryReference> submittedReference_;
            AgentHistoryProjectRevision submittedProject_;
            int selection_{-1};
            const char *status_{};
            std::uint64_t turn_{};
            bool awaitingCheckpoint_{};
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
