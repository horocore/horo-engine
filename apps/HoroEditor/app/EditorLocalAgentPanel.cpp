#include "EditorLocalAgentPanel.h"

#include "AgentLocalSceneTurn.h"
#include "AgentTurn.h"
#include "EditorLocalAgentContext.h"
#include "EditorLocalAgentHistory.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>

namespace Horo::Editor {
    namespace {
        using namespace Agent;
        using Reply = std::variant<ProviderDiscovery, AgentTurnResult>;

        /** @brief Runs one user-initiated discovery or inference on an isolated worker. */
        Reply RunLocalOperation(std::string endpoint, std::optional<ModelRequest> request, std::optional<std::size_t> sceneCount,
                                std::optional<EditorLocalAgentContext> source, std::uint64_t turn, std::stop_token stop) {
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

        /** @brief Owns one optional asynchronous request; workspace callbacks remain on the editor lane. */
        class LocalAgentPanel final : public IWorkspacePanel {
        public:
            LocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files)
                : IWorkspacePanel("workspace.local_agent"), history_(jobs, files) {}

            ~LocalAgentPanel() override {
                RevokeSourceContext();
                worker_.request_stop();
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
                RevokeSourceContext();
                activeSource_ = context.activeSourceSnapshot;
                contextProject_ = context.projectIdentity;
                contextLifetime_.emplace();
                ++contextGeneration_;
                if (context.projectFileRoot && !context.projectIdentity.empty())
                    history_.Attach({*context.projectFileRoot, std::string{context.projectIdentity}});
            }

            void OnDetach() override {
                worker_.request_stop();
                discard_ = true;
                prompt_.clear();
                response_.clear();
                sceneCount_ = {};
                RevokeSourceContext();
                history_.Detach();
                awaitingCheckpoint_ = false;
                submittedPrompt_.reset();
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
                ImGui::BeginDisabled(awaitingCheckpoint_ || sourcePreview_);
                static_cast<void>(Ui::InputTextControl("##Prompt", prompt_, 4096, fonts));
                ImGui::EndDisabled();
                DrawSendActions(locale);
                if (Busy() && Button(locale, "stop", !worker_.get_stop_token().stop_requested()))
                    worker_.request_stop();
                if (status_)
                    ImGui::TextWrapped("%s", LocalizedText(locale, status_).c_str());
                if (!response_.empty())
                    ImGui::TextWrapped("%s", response_.c_str());
                history_.Draw(context);
                ImGui::EndChild();
            }

        private:
            [[nodiscard]] bool Busy() const {
                return reply_.valid();
            }

            void RevokeSourceContext() noexcept {
                if (contextLifetime_)
                    contextLifetime_->RequestCancellation();
                contextLifetime_.reset();
                sourcePreview_.reset();
                activeSource_ = {};
                contextProject_.clear();
            }

            [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled) const {
                const auto label = LocalizedText(locale, key) + "##" + key;
                return Ui::Button({.label = label.c_str(), .enabled = enabled});
            }

            void DrawProvider(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                ImGui::TextUnformatted(LocalizedText(locale, "endpoint").c_str());
                ImGui::BeginDisabled(Busy() || awaitingCheckpoint_ || history_.HasRecovery() || sourcePreview_);
                if (Ui::InputTextControl("##Endpoint", endpoint_, 256, fonts)) {
                    models_.clear();
                    selection_ = -1;
                    response_.clear();
                    status_ = nullptr;
                }
                if (Button(locale, "discover", !Busy() && !endpoint_.empty()))
                    Start(std::nullopt);
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
                if (sourcePreview_) {
                    DrawSourceReview(locale);
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
                    PrepareSourceReview();
                    return;
                }
                const auto count = sceneCount_ ? sceneCount_() : std::nullopt;
                if (count.has_value()) {
                    ImGui::Text("%s %zu", LocalizedText(locale, "scene_count").c_str(), *count);
                    if (Button(locale, "ask_with_scene_count", ready))
                        Send(count);
                }
            }

            void PrepareSourceReview() {
                SourceDocumentSnapshot active;
                if (!activeSource_(active)) {
                    status_ = "context_unavailable";
                    return;
                }
                auto prepared = EditorLocalAgentContext::Prepare(std::move(active), contextProject_, contextGeneration_,
                                                                 contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = "context_unavailable";
                    return;
                }
                sourcePreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            void DrawSourceReview(const ILocalizationService &locale) {
                SourceDocumentSnapshot active;
                if (!activeSource_(active) || !sourcePreview_->Matches(active)) {
                    sourcePreview_.reset();
                    status_ = "context_stale";
                    return;
                }
                ImGui::TextWrapped("%s", LocalizedText(locale, "context_review").c_str());
                ImGui::PushTextWrapPos(0);
                for (const auto &item : sourcePreview_->Items()) {
                    const auto &payload = item.candidate.text;
                    ImGui::TextUnformatted(payload.data(), payload.data() + payload.size());
                }
                ImGui::PopTextWrapPos();
                if (Button(locale, "context_approve", !Busy())) {
                    auto approved = std::exchange(sourcePreview_, std::nullopt);
                    if (approved->Approve())
                        Send({}, false, std::move(approved));
                    else
                        status_ = "context_stale";
                    return;
                }
                ImGui::SameLine();
                if (Button(locale, "context_cancel", true))
                    sourcePreview_.reset();
            }

            void Send(std::optional<std::size_t> sceneCount, bool recoverable = false, std::optional<EditorLocalAgentContext> source = {}) {
                ModelRequest request;
                request.model = model_;
                request.messages.push_back({.role = ModelRole::User, .text = prompt_});
                history_.ClearReview();
                submittedPrompt_ = recoverable || sceneCount.has_value() || source ? std::nullopt : std::optional{prompt_};
                submittedProject_ = history_.Project();
                Start(std::move(request), sceneCount, std::move(source));
                if (recoverable && !Busy())
                    history_.FinishRecovery(AgentHistoryMessageState::Failed);
            }

            void Start(std::optional<ModelRequest> request, std::optional<std::size_t> sceneCount = {},
                       std::optional<EditorLocalAgentContext> source = {}) {
                try {
                    std::packaged_task<Reply(std::stop_token)> task{[endpoint = endpoint_, request = std::move(request), sceneCount,
                                                                     source = std::move(source),
                                                                     turn = ++turn_](std::stop_token stop) mutable {
                        return RunLocalOperation(std::move(endpoint), std::move(request), sceneCount, std::move(source), turn, stop);
                    }};
                    auto pending = task.get_future();
                    worker_ = std::jthread{[task = std::move(task)](std::stop_token stop) mutable {
                        task(stop);
                    }};
                    reply_ = std::move(pending);
                    discard_ = false;
                    status_ = "working";
                    response_.clear();
                } catch (const std::bad_alloc &) {
                    status_ = "failed";
                } catch (const std::system_error &) {
                    status_ = "failed";
                }
            }

            void PollReply() {
                if (!Busy() || reply_.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
                    return;
                try {
                    auto reply = reply_.get();
                    if (discard_)
                        status_ = nullptr;
                    else if (worker_.get_stop_token().stop_requested()) {
                        status_ = "cancelled";
                        history_.FinishRecovery(AgentHistoryMessageState::Cancelled);
                    } else
                        std::visit([this](auto &result) {
                            Adopt(result);
                        }, reply);
                } catch (const std::exception &) {
                    status_ = discard_ ? nullptr : "failed";
                    history_.FinishRecovery(AgentHistoryMessageState::Failed);
                }
                submittedPrompt_.reset();
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
                    if (history_.HasRecovery())
                        history_.ReviewRecoveryAnswer(response_);
                    else if (!response_.empty() && submittedPrompt_)
                        history_.Review(submittedProject_, *submittedPrompt_, response_);
                    return;
                }
                status_ = result.status == AgentTurnStatus::TimedOut ? "timeout" : FailureKey(result.providerError);
                history_.FinishRecovery(result.status == AgentTurnStatus::Cancelled ? AgentHistoryMessageState::Cancelled
                                                                                    : AgentHistoryMessageState::Failed);
            }

            std::string endpoint_;
            std::string model_;
            std::string prompt_;
            std::string response_;
            std::vector<ModelDescriptor> models_;
            std::function<std::optional<std::size_t>()> sceneCount_;
            std::function<bool(SourceDocumentSnapshot &)> activeSource_;
            std::string contextProject_;
            std::uint64_t contextGeneration_{};
            std::optional<CancellationSource> contextLifetime_;
            std::optional<EditorLocalAgentContext> sourcePreview_;
            EditorLocalAgentHistory history_;
            std::optional<std::string> submittedPrompt_;
            AgentHistoryProjectRevision submittedProject_;
            int selection_{-1};
            const char *status_{};
            std::uint64_t turn_{};
            bool discard_{};
            bool awaitingCheckpoint_{};
            std::jthread worker_;
            std::future<Reply> reply_;
        };
    }  // namespace

    /** @copydoc MakeEditorLocalAgentPanel */
    EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files) {
        auto panel = std::make_shared<LocalAgentPanel>(jobs, files);
        return {panel, [owner = std::weak_ptr{panel}] {
            if (const auto current = owner.lock())
                current->Poll();
        }};
    }
}  // namespace Horo::Editor
