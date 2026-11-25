#include "EditorLocalAgentPanel.h"

#include "AgentBackgroundTurn.h"
#include "AgentLocalSceneTurn.h"
#include "AgentTurn.h"
#include "EditorCloudModelProfile.h"
#include "EditorLocalAgentContext.h"
#include "EditorLocalAgentHistory.h"
#include "EditorLocalAgentPatch.h"
#include "EditorLocalAgentPatchCheckpoint.h"
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
#include "EditorImmersiveListening.h"
#include "Horo/Editor/EditorSettingsService.h"
#endif
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/WorkspaceMcpControls.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Security/CredentialStore.h"
#include "editor/language/ClangdProvider.h"
#include "editor/language/HoroScriptSyntax.h"
#include "editor/source_editor/SourceSyntax.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Editor {
    namespace {
        using namespace Agent;

        struct ContextReview final {
            Result<EditorLocalAgentContext> outcome;
            const char *unavailable;
        };

        /** @brief Completion of a serialized user-profile operation. */
        struct CloudProfileReply final {
            Result<std::optional<EditorCloudModelProfile>> outcome;
            const char *status;
        };

        using Reply = std::variant<ProviderDiscovery, AgentTurnResult, ContextReview, CloudProfileReply>;
        using BackgroundTurn = AgentBackgroundTurn<Reply>;
        using PatchLoad = Result<std::optional<EditorLocalAgentPatchReview>>;
        using SceneReviewClock = std::chrono::steady_clock;

        /** @brief The exact disclosed destination and payload retained until a local decision. */
        struct CloudTurnReview final {
            ModelProviderConfig config;
            std::optional<ModelRequest> request;
        };

        /** @brief One preview tied to the exact project attachment and disclosed count. */
        struct SceneQueryReview final {
            std::size_t count;
            std::uint64_t projectGeneration;
            SceneReviewClock::time_point expires;
        };

        struct PatchPreview final {
            std::uint64_t turn;
            Result<EditorLocalAgentPatchReview> review;
        };

        /** @brief One read-only result for an exact immutable proposal plan and file. */
        struct PatchDiagnostics final {
            std::weak_ptr<const Proposals::WorkspaceEditPlan> plan;
            std::size_t file{};
            Result<Language::LanguageDiagnostics> diagnostics;
        };

        using PatchWorker = AgentBackgroundTurn<std::variant<Result<void>, PatchLoad, PatchPreview, PatchDiagnostics>>;

        /** @brief Resolves the localized operation label from the admitted file kind. */
        const char *PatchFileLabel(const EditorPatchFileKind kind) {
            constexpr std::array keys{"patch_file_create", "patch_file_edit", "patch_file_delete", "patch_file_rename"};
            return keys[static_cast<std::size_t>(kind)];
        }

        /** @brief Gives each planner conflict its owning user-facing status. */
        const char *PatchConflictLabel(const Proposals::PlanConflictReason reason) {
            constexpr auto unavailable = "patch_file_source_unavailable";
            constexpr std::array keys{unavailable,
                                      unavailable,
                                      unavailable,
                                      unavailable,
                                      "source_patch_stale",
                                      "patch_file_disk_changed",
                                      "patch_file_destination_occupied",
                                      "patch_file_overlap",
                                      "patch_file_dirty_delete",
                                      "patch_file_case_only_rename",
                                      "patch_file_parent_missing",
                                      "patch_file_redirect",
                                      "patch_file_destination_alias",
                                      "patch_file_destination_unavailable"};
            const auto index = static_cast<std::size_t>(reason);
            return index < keys.size() ? keys[index] : unavailable;
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

        /** @brief One worker publishes a receipt at most once. The editor reads the immutable value after acquire. */
        struct SceneQueryProgress final {
            void Publish(const LocalSceneAuditReference receipt) noexcept {
                audit = {{receipt.principal, receipt.session, receipt.generation}, receipt.operation};
                ready.store(true, std::memory_order_release);
            }

            [[nodiscard]] std::optional<WorkspaceMcpAuditReference> ReadAudit() const noexcept {
                if (!ready.load(std::memory_order_acquire))
                    return std::nullopt;
                return audit;
            }

            std::atomic<SceneQueryPhase> phase{SceneQueryPhase::Idle};

        private:
            WorkspaceMcpAuditReference audit;
            std::atomic_bool ready{};
        };

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
                    sceneQuery->phase.store(ScenePhase(activity), std::memory_order_relaxed);
                }, [&sceneQuery](const LocalSceneAuditReference receipt) {
                    sceneQuery->Publish(receipt);
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

        /** @brief Executes the approved cloud payload with no project-context or tool capability. */
        Reply RunCloudOperation(CloudTurnReview review, std::shared_ptr<Security::CredentialProvider> credentials, std::uint64_t turn,
                                std::stop_token stop) {
            auto provider = CreateOpenAIModelProviderWithCredentials(std::move(review.config), std::move(credentials));
            if (!review.request)
                return provider->Discover(stop);
            AgentTurnRuntime runtime;
            return runtime.Run(*provider, nullptr, turn, std::move(*review.request), {}, stop);
        }

        /** @brief Presents sanitized, localized remediation without displaying transport response bodies. */
        const char *FailureKey(const std::optional<ModelErrorCode> error, const bool cloud) {
            using enum ModelErrorCode;
            if (error == InvalidRequest)
                return "invalid";
            if (error == Cancelled)
                return "cancelled";
            if (error == UnsupportedCapability)
                return "tools_unsupported";
            if (cloud && (error == Authentication || error == CredentialUnavailable))
                return "cloud_auth_failed";
            if (cloud && error == RateLimited)
                return "cloud_rate_limited";
            if (error == Unavailable || error == Transport)
                return cloud ? "cloud_unavailable" : "unavailable";
            return "failed";
        }

        /** @brief Presents and reviews results from the editor-owned background operation. */
        class LocalAgentPanel final : public IWorkspacePanel {
        public:
            LocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files, std::shared_ptr<BackgroundTurn> background,
                            std::shared_ptr<EditorCloudModelProfileStore> cloudProfiles, IExternalProcessRunner *processes,
                            [[maybe_unused]] EditorImmersiveListening *listening)
                : IWorkspacePanel("workspace.local_agent"), cloudProfiles_(std::move(cloudProfiles)), background_(std::move(background)),
                  history_(jobs, files), files_(files), processes_(processes) {
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
                listening_ = listening;
#endif
            }

            ~LocalAgentPanel() override {
                CancelListening();
                RevokeContext();
            }

            std::string GetId() const override {
                return LocalAgentWorkspacePanelId;
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
                requestSourceReviewValidation_ = context.requestSourceReviewValidation;
                projectFiles_ = context.agentProjectFiles;
                projectReadIdentity_ = context.agentProjectReadIdentity;
                captureSceneSelection_ = context.captureSceneSelection;
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
                reviewAgentSceneReferences_ = context.reviewAgentSceneReferences;
                readAgentSceneContacts_ = context.readAgentSceneContacts;
                agentSceneContactsCurrent_ = context.agentSceneContactsCurrent;
#endif
                captureAgentViewport_ = context.captureAgentViewport;
                takeAgentViewportReview_ = context.takeAgentViewportReview;
                captureEditorDiagnostics_ = context.captureEditorDiagnostics;
                requestMcpAudit_ = context.requestMcpAudit;
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
                CancelListening();
                background_->Abandon();
                prompt_.clear();
                cloudPrompt_.clear();
                cloudProfileAdmitted_ = false;
                cloudProfileRead_ = false;
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
                CancelListening();
                textInput_.Reset();
                sceneQueryReview_.reset();
                cloudReview_.reset();
            }

            /** @brief Advances result publication and project storage after the workspace's runtime update. */
            void Poll() {
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (listening_)
                    listening_->Poll();
#endif
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
                DrawProviderSelection(locale);
                if (cloudMode_) {
                    CancelListening();
                    DrawCloud(locale, fonts);
                    UpdateTextInputFocus();
                    ImGui::EndChild();
                    return;
                }
                ReviewOfferedViewport();
                DrawProvider(locale, fonts);
                DrawOptionalListening(context);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "prompt"), Theme::Text(), fonts);
                ImGui::BeginDisabled(awaitingCheckpoint_ || ContextLocksComposer() || sceneQueryReview_);
                static_cast<void>(DrawTextField("##Prompt", prompt_, 4096, fonts));
                ImGui::EndDisabled();
                DrawSendActions(locale, fonts);
                const auto *cancel = sceneQuery_ ? "scene_query_revoke" : "stop";
                if (Busy() && Button(locale, cancel, background_->CanCancel()))
                    background_->Cancel();
                if (status_)
                    ImGui::TextWrapped("%s", LocalizedText(locale, status_).c_str());
                DrawSceneQueryActivity(locale, fonts, command);
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
            /** @brief Draws the native listening controls only when the runtime supplies that host. */
            void DrawOptionalListening([[maybe_unused]] const EditorGuiContext &context) {
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (listening_)
                    DrawListening(context);
#endif
            }

            /** @brief Retires an utterance before its panel or workspace leaves presentation. */
            void CancelListening() noexcept {
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
                if (listening_ && listening_->Status() != EditorImmersiveListening::State::Idle)
                    listening_->Cancel();
                ResetListeningInput();
                listeningReferences_.reset();
                requestedListeningReferences_.reset();
                reviewingSurfaces_ = false;
                ResetContactReview();
#endif
            }

#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
            /** @brief Requires a fresh choice after the device list changes. */
            void ResetListeningInput() noexcept {
                listeningInput_ = -1;
                listeningDevice_.reset();
            }

            /** @brief Presents an actionable reason for a capture that released its scope. */
            static const char *ListeningFailureKey(const SpeechTranscriptionStatus reason) noexcept {
                using enum SpeechTranscriptionStatus;
                switch (reason) {
                    case CaptureLimitExceeded:
                        return "xr_listening_limit";
                    case ProviderTimedOut:
                        return "xr_listening_provider_timeout";
                    case HypothesisLimitExceeded:
                        return "xr_listening_update_limit";
                    case ResourceUnavailable:
                        return "xr_listening_resources_unavailable";
                    case ProviderUnavailable:
                        return "xr_listening_provider_unavailable";
                    case LowConfidence:
                        return "xr_listening_low_confidence";
                    default:
                        return "xr_listening_interrupted";
                }
            }

            /** @brief Maps the local capture state to one localized status. */
            static const char *ListeningStatusKey(const EditorImmersiveListening &listening) noexcept {
                using enum EditorImmersiveListening::State;
                switch (listening.Status()) {
                    case Idle:
                        return "xr_listening_idle";
                    case Listening:
                        return "xr_listening_active";
                    case Finishing:
                        return "xr_listening_finishing";
                    case SpeechOnly:
                        return "xr_listening_speech_only";
                    case SpeechOnlySpatialRejected:
                        return "xr_listening_spatial_rejected";
                    case Spatial:
                        return "xr_listening_spatial";
                    case Interrupted:
                        return ListeningFailureKey(listening.FailureReason());
                }
                return "xr_listening_interrupted";
            }

            /** @brief Presents distinct owner permission requests without opening capture. */
            void DrawListeningPermissions(const ILocalizationService &locale) {
                if (listening_->NeedsMicrophonePermission() && Button(locale, "xr_listening_microphone_permission", true))
                    listening_->RequestMicrophone();
                if (listening_->NeedsSpeechPermission() && Button(locale, "xr_listening_speech_permission", true))
                    listening_->RequestSpeech();
            }

            /** @brief Presents explicit discovery and selected input controls. */
            void DrawListeningStart(const EditorGuiContext &context) {
                const auto &locale = context.localization;
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "xr_listening_privacy"), Theme::Muted(),
                                context.theme.fonts);
                DrawListeningPermissions(locale);
                if (Button(locale, "xr_listening_refresh", true)) {
                    ResetListeningInput();
                    listening_->RefreshInputs();
                }
                DrawListeningInput(context);
                const bool ready = listeningDevice_.has_value() && captureSceneSelection_ && listening_->CanListen();
                if (Button(locale, "xr_listening_start", ready)) {
                    const auto selection = captureSceneSelection_(AgentSceneSelectionPurpose::ImmersivePlay);
                    listening_->Start(*listeningDevice_, context.settings.settings.languageTag, contextGeneration_,
                                      selection.value_or(AgentSceneSelectionSnapshot{}));
                }
            }

            /** @brief Projects the current labels and checks that the chosen endpoint still owns its row. */
            void DrawListeningInput(const EditorGuiContext &context) {
                const auto inputs = listening_->Inputs();
                if (inputs.empty()) {
                    ResetListeningInput();
                    return;
                }
                const Ui::ComboItemSource source{.label = [inputs](int index) {
                    return inputs[static_cast<std::size_t>(index)].c_str();
                }};
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(context.localization, "xr_listening_input"), Theme::Text(),
                                context.theme.fonts);
                const bool changed = Ui::ComboControl("##xr-listening-input", &listeningInput_, static_cast<int>(inputs.size()), source,
                                                      context.theme.fonts);
                const auto input = listeningInput_ >= 0 ? listening_->InputAt(static_cast<std::size_t>(listeningInput_)) : std::nullopt;
                if (changed)
                    listeningDevice_ = input;
                if (listeningDevice_ != input)
                    ResetListeningInput();
            }

            /** @brief Shows the separate Play-only local listening workflow without feeding the model composer. */
            void DrawListening(const EditorGuiContext &context) {
                using enum EditorImmersiveListening::State;
                const auto state = listening_->Status();
                const auto &locale = context.localization;
                const auto &fonts = context.theme.fonts;
                const auto width = ImGui::GetContentRegionAvail().x;
                Ui::WrappedText(width, LocalizedText(locale, ListeningStatusKey(*listening_)), Theme::Text(), fonts);
                if (state == Idle || state == Interrupted) {
                    listeningReferences_.reset();
                    requestedListeningReferences_.reset();
                    reviewingSurfaces_ = false;
                    ResetContactReview();
                    DrawListeningStart(context);
                    return;
                }
                if (state == Listening && Button(locale, "xr_listening_finish", true))
                    listening_->Finish();
                const auto transcript = listening_->Transcript();
                const bool hasTranscript = !transcript.empty();
                if (Button(locale, hasTranscript ? "xr_listening_discard" : "xr_listening_cancel", true)) {
                    CancelListening();
                    return;
                }
                if (hasTranscript) {
                    Ui::WrappedText(width, transcript, Theme::Text(), fonts);
                    DrawListeningReferences(context);
                    DrawListeningContacts(context);
                } else if (const auto draft = listening_->PartialTranscript(); !draft.empty()) {
                    Ui::WrappedText(width, LocalizedText(locale, "xr_listening_draft"), Theme::Muted(), fonts);
                    Ui::WrappedText(width, draft, Theme::Text(), fonts);
                }
            }

            /** @brief Projects owner-issued reference states without selecting a candidate. */
            static const char *ReferenceStateKey(const AgentSceneReferenceState state) noexcept {
                using enum AgentSceneReferenceState;
                switch (state) {
                    case Available:
                        return "xr_reference_available";
                    case Hidden:
                        return "xr_reference_hidden";
                    case Locked:
                        return "xr_reference_locked";
                    case Deleted:
                        return "xr_reference_deleted";
                    case OutOfScope:
                        return "xr_reference_out_of_scope";
                }
                return "xr_reference_out_of_scope";
            }

            /** @brief Rechecks the captured selection on demand and displays its exact revision and every candidate. */
            void DrawListeningReferences(const EditorGuiContext &context) {
                const auto &locale = context.localization;
                const bool canReview = static_cast<bool>(reviewAgentSceneReferences_);
                using enum AgentSceneReferenceSource;
                if (Button(locale, "xr_reference_review", canReview))
                    LoadListeningReferences(Selection);
                if (Button(locale, "xr_pointing_review", canReview))
                    LoadListeningReferences(Pointing);
                if (Button(locale, "xr_location_review", canReview))
                    LoadListeningReferences(Location);
                if (Button(locale, "xr_surface_review", true)) {
                    requestedListeningReferences_.reset();
                    listeningReferences_.reset();
                    reviewingSurfaces_ = true;
                }
                if (reviewingSurfaces_)
                    DrawListeningSurfaces(context);
                if (!requestedListeningReferences_)
                    return;
                DrawListeningReferenceResult(context, *requestedListeningReferences_);
            }

            /** @brief Refreshes the requested review using the current scoped receipt. */
            void LoadListeningReferences(const AgentSceneReferenceSource source) {
                reviewingSurfaces_ = false;
                listeningReferences_ = listening_->ReviewReferences(reviewAgentSceneReferences_, source);
                requestedListeningReferences_ = source;
            }

            /** @brief Projects a complete review, an explicit empty result, or unavailable evidence. */
            void DrawListeningReferenceResult(const EditorGuiContext &context, const AgentSceneReferenceSource source) {
                const auto &locale = context.localization;
                const auto width = ImGui::GetContentRegionAvail().x;
                const auto &fonts = context.theme.fonts;
                const bool pointing = source != AgentSceneReferenceSource::Selection;
                if (!listeningReferences_) {
                    Ui::WrappedText(width, LocalizedText(locale, pointing ? "xr_pointing_unavailable" : "xr_reference_stale"),
                                    Theme::Muted(), fonts);
                    return;
                }
                const auto &review = *listeningReferences_;
                if (review.mounted)
                    Ui::WrappedText(width, LocalizedText(locale, "xr_reference_mounted"), Theme::Muted(), fonts);
                Ui::WrappedText(width, LocalizedText(locale, "xr_reference_revision") + " " + std::to_string(review.documentRevision),
                                Theme::Muted(), fonts);
                if (review.Candidates().empty())
                    Ui::WrappedText(width, LocalizedText(locale, pointing ? "xr_pointing_empty" : "xr_reference_empty"), Theme::Muted(),
                                    fonts);
                if (review.RequiresClarification())
                    Ui::WrappedText(width, LocalizedText(locale, "xr_reference_ambiguous"), Theme::Text(), fonts);
                for (const auto &candidate : review.Candidates()) {
                    Ui::WrappedText(width,
                                    LocalizedText(locale, ReferenceStateKey(candidate.state)) + " #" + std::to_string(candidate.object),
                                    Theme::Text(), fonts);
                    if (const auto &point = candidate.worldPosition)
                        DrawListeningCoordinates(context, width, *point);
                }
            }

            /** @brief Uses the same recorded World coordinate format for mesh and physical surface hits. */
            void DrawListeningCoordinates(const EditorGuiContext &context, const float width, const Math::Vec3 point) const {
                Ui::WrappedText(width,
                                std::format("{} X {} · Y {} · Z {}", LocalizedText(context.localization, "xr_location_coordinates"),
                                            point.x, point.y, point.z),
                                Theme::Muted(), context.theme.fonts);
            }

            /** @brief Maps portable plane classification to localized presentation. */
            static const char *SurfaceClassificationKey(const XR::XRPlaneClassification classification) noexcept {
                using enum XR::XRPlaneClassification;
                switch (classification) {
                    case Floor:
                        return "xr_surface_floor";
                    case Ceiling:
                        return "xr_surface_ceiling";
                    case Wall:
                        return "xr_surface_wall";
                    case Platform:
                        return "xr_surface_platform";
                    default:
                        return "xr_surface_unknown";
                }
            }

            /** @brief Projects live-authorized matches without caching any room coordinates in the panel. */
            void DrawListeningSurfaces(const EditorGuiContext &context) {
                const auto review = listening_->ReviewSurfaces();
                const auto width = ImGui::GetContentRegionAvail().x;
                const auto &fonts = context.theme.fonts;
                const auto &locale = context.localization;
                if (!review || review->count == 0) {
                    Ui::WrappedText(width, LocalizedText(locale, review ? "xr_surface_empty" : "xr_surface_unavailable"), Theme::Muted(),
                                    fonts);
                    return;
                }
                if (review->count > 1)
                    Ui::WrappedText(width, LocalizedText(locale, "xr_reference_ambiguous"), Theme::Text(), fonts);
                for (const auto &hit : std::span{review->hits}.first(review->count)) {
                    Ui::WrappedText(width, LocalizedText(locale, SurfaceClassificationKey(hit.classification)), Theme::Text(), fonts);
                    DrawListeningCoordinates(context, width, hit.worldPosition);
                }
            }

            /** @brief Clears the explicitly requested Physics snapshot. */
            void ResetContactReview() noexcept {
                contactReview_.reset();
                contactsRequested_ = false;
            }

            /** @brief Shows only local, selection-scoped contact starts and the tick that produced them. */
            void DrawListeningContacts(const EditorGuiContext &context) {
                const auto &locale = context.localization;
                if (Button(locale, "xr_contacts_review", readAgentSceneContacts_ && agentSceneContactsCurrent_)) {
                    contactReview_ = readAgentSceneContacts_();
                    contactsRequested_ = true;
                }
                if (!contactsRequested_)
                    return;
                const auto width = ImGui::GetContentRegionAvail().x;
                const auto &fonts = context.theme.fonts;
                if (!contactReview_ || !agentSceneContactsCurrent_ || !agentSceneContactsCurrent_(*contactReview_)) {
                    contactReview_.reset();
                    Ui::WrappedText(width, LocalizedText(locale, "xr_contacts_unavailable"), Theme::Muted(), fonts);
                    return;
                }
                const auto &review = *contactReview_;
                Ui::WrappedText(width,
                                std::format("{} {} · #{}", LocalizedText(locale, "xr_contacts_tick"), review.completedTick, review.object),
                                Theme::Muted(), fonts);
                if (review.count == 0)
                    Ui::WrappedText(width, LocalizedText(locale, "xr_contacts_empty"), Theme::Muted(), fonts);
                for (const auto &contact : std::span{review.contacts}.first(review.count)) {
                    const bool trigger = contact.kind == AgentSceneContactKind::TriggerEnter;
                    Ui::WrappedText(width,
                                    LocalizedText(locale, trigger ? "xr_contacts_trigger" : "xr_contacts_contact") + " #" +
                                        std::to_string(contact.otherObject),
                                    Theme::Text(), fonts);
                }
            }

            EditorImmersiveListening *listening_{};
            int listeningInput_{-1};
            std::optional<Audio::AudioCaptureDeviceId> listeningDevice_;
            std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> reviewAgentSceneReferences_;
            std::optional<AgentSceneReferenceReview> listeningReferences_;
            std::optional<AgentSceneReferenceSource> requestedListeningReferences_;
            bool reviewingSurfaces_{};
            std::function<std::optional<AgentSceneContactReview>()> readAgentSceneContacts_;
            std::function<bool(const AgentSceneContactReview &)> agentSceneContactsCurrent_;
            std::optional<AgentSceneContactReview> contactReview_;
            bool contactsRequested_{};
#endif

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
                sceneQueryReview_.reset();
                cloudReview_.reset();
                plainReplay_.reset();
                sceneQuery_.reset();
                activeSource_ = {};
                activeSourceRange_ = {};
                openSources_ = {};
                requestSourceReview_ = {};
                requestSourceReviewApply_ = {};
                requestSourceReviewUndo_ = {};
                sourceReviewTransaction_ = {};
                requestSourceReviewValidation_ = {};
                sourceApplyQueued_ = false;
                patchSource_ = {};
                patchReview_.reset();
                patchDiagnostics_.clear();
                patchDestination_.clear();
                patchCheckpoint_.Abandon();
                captureSceneSelection_ = {};
#if defined(HORO_EDITOR_MAC_MICROPHONE_PERMISSION) && defined(HORO_HAS_OPENXR_VULKAN_HOST)
                reviewAgentSceneReferences_ = {};
                readAgentSceneContacts_ = {};
                agentSceneContactsCurrent_ = {};
#endif
                captureAgentViewport_ = {};
                takeAgentViewportReview_ = {};
                captureEditorDiagnostics_ = {};
                requestMcpAudit_ = {};
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

            [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled) {
                const auto &visible = LocalizedText(locale, key);
                buttonLabel_.assign(visible).append("###").append(key);
                const bool clicked =
                    Ui::Button({.label = buttonLabel_.c_str(), .enabled = enabled, .style = {.width = Ui::StyleWidth::FillAvailable}});
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
            void DrawSceneQueryActivity(const ILocalizationService &locale, const Theme::Fonts &fonts,
                                        EditorWorkspaceViewCommandData &command) {
                if (!sceneQuery_)
                    return;
                if (const auto *key = ScenePhaseKey(sceneQuery_->phase.load(std::memory_order_relaxed)))
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, key), Theme::Muted(), fonts);
                if (const auto audit = sceneQuery_->ReadAudit(); audit && requestMcpAudit_ && Button(locale, "scene_query_audit", true))
                    requestMcpAudit_(*audit, command);
            }

            /** @brief Switches the selected provider only when the local composer holds no active review. */
            void DrawProviderSelection(const ILocalizationService &locale) {
                if (!cloudProfiles_)
                    return;
                const bool canSwitch = !Busy() && !awaitingCheckpoint_ && !history_.HasRecovery() && !ContextLocksComposer() &&
                                       !sceneQueryReview_ && !cloudReview_;
                if (!Button(locale, cloudMode_ ? "cloud_use_local" : "cloud_use", canSwitch))
                    return;
                cloudMode_ = !cloudMode_;
                cloudProfileRead_ = false;
                cloudProfileAdmitted_ = false;
                models_.clear();
                selection_ = -1;
                status_ = nullptr;
                response_.clear();
            }

            /** @brief Reports whether the available reference belongs to the exact configured endpoint. */
            bool CanReviewCloud() const {
                return cloudProfileAdmitted_ && cloudProfile_ && cloudEndpoint_ == cloudProfile_->endpoint;
            }

            /** @brief Serializes profile writes and credential resolution on the same worker lane. */
            template <typename Operation> void StartCloudProfile(Operation operation, const char *status) {
                try {
                    BackgroundTurn::Operation task{
                        [profiles = cloudProfiles_, operation = std::move(operation), status](std::stop_token) mutable {
                        return Reply{CloudProfileReply{operation(*profiles), status}};
                    }};
                    cloudProfileBusy_ = background_->Start(std::move(task));
                    status_ = cloudProfileBusy_ ? "cloud_profile_working" : "cloud_profile_failed";
                } catch (const std::bad_alloc &) {
                    status_ = "cloud_profile_failed";
                }
            }

            /** @brief Lifts a successful write into the same profile result used for loading. */
            static Result<std::optional<EditorCloudModelProfile>> UpdatedProfile(Result<EditorCloudModelProfile> profile) {
                using Outcome = Result<std::optional<EditorCloudModelProfile>>;
                if (profile.HasError())
                    return Outcome::Failure(profile.ErrorValue());
                return Outcome::Success(std::move(profile).Value());
            }

            /** @brief Enrolls only the explicitly requested clipboard value, using zeroizing application-owned memory. */
            void EnrollCloudCredential() {
                const auto value = CaptureClipboardText();
                if (value.empty() || std::ranges::any_of(value, [](const unsigned char byte) {
                    return byte <= 0x20 || byte >= 0x7f;
                })) {
                    status_ = "cloud_key_failed";
                    return;
                }
                try {
                    StartCloudProfile(
                        [expected = cloudProfile_, endpoint = cloudEndpoint_, model = cloudModel_,
                         key = Security::SecureBytes{std::as_bytes(std::span{value})}](EditorCloudModelProfileStore &store) mutable {
                        return UpdatedProfile(store.Enroll(expected, std::move(endpoint), std::move(model), std::move(key)));
                    },
                        "cloud_key_saved");
                } catch (const std::bad_alloc &) {
                    status_ = "cloud_key_failed";
                }
            }

            /** @brief Withdraws the key reference only after its exact backend entry has been revoked. */
            void RevokeCloudCredential() {
                StartCloudProfile([expected = *cloudProfile_](EditorCloudModelProfileStore &store) {
                    using Outcome = Result<std::optional<EditorCloudModelProfile>>;
                    const auto revoked = store.Revoke(expected);
                    return revoked.HasError() ? Outcome::Failure(revoked.ErrorValue()) : Outcome::Success(std::nullopt);
                }, "cloud_key_removed");
            }

            /** @brief Retains the selected model in the user profile without attempting a network request. */
            void RememberCloudModel() {
                StartCloudProfile([expected = *cloudProfile_, model = cloudModel_](EditorCloudModelProfileStore &store) mutable {
                    return UpdatedProfile(store.SelectModel(expected, std::move(model)));
                }, "cloud_model_saved");
            }

            /** @brief Pins the reviewed destination, reference, and payload before an explicit send decision. */
            void PrepareCloudReview(const bool discover) {
                if (!CanReviewCloud())
                    return;
                ModelProviderConfig config{.endpoint = cloudProfile_->endpoint,
                                           .credentialReference = std::string{cloudProfile_->credential.Value()}};
                std::optional<ModelRequest> request;
                if (!discover) {
                    request.emplace();
                    request->model = cloudModel_;
                    request->messages.push_back({.role = ModelRole::User, .text = cloudPrompt_});
                }
                cloudReview_.emplace(std::move(config), std::move(request));
                status_ = nullptr;
            }

            /** @brief Consumes the displayed review and transfers it to the existing cancellable worker owner. */
            void SendCloudReview() {
                auto reviewed = std::move(*cloudReview_);
                cloudReview_.reset();
                submittedPrompt_.reset();
                submittedReference_.reset();
                response_.clear();
                try {
                    BackgroundTurn::Operation task{[review = std::move(reviewed), credentials = cloudProfiles_->Provider(),
                                                    turn = ++turn_](std::stop_token stop) mutable {
                        return RunCloudOperation(std::move(review), std::move(credentials), turn, stop);
                    }};
                    status_ = background_->Start(std::move(task)) ? "cloud_working" : "failed";
                } catch (const std::bad_alloc &) {
                    status_ = "failed";
                }
            }

            /** @brief Shows the exact endpoint, operation, and prompt before any authenticated request is queued. */
            void DrawCloudReview(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                const auto show = [&](const std::string_view text) {
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, text, Theme::Text(), fonts);
                };
                show(LocalizedText(locale, cloudReview_->request ? "cloud_review_prompt" : "cloud_review_discovery"));
                show(cloudReview_->config.endpoint);
                if (cloudReview_->request) {
                    show(cloudReview_->request->model);
                    show(cloudReview_->request->messages.front().text);
                }
                if (Button(locale, "cloud_approve", !Busy()))
                    SendCloudReview();
                else if (Button(locale, "cloud_discard", true))
                    cloudReview_.reset();
            }

            /** @brief Presents the cloud-only composer and credentials without exposing local context or history actions. */
            void DrawCloud(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                const auto show = [&](const std::string_view text) {
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, text, Theme::Text(), fonts);
                };
                if (!cloudProfileRead_ && !Busy()) {
                    cloudProfileRead_ = true;
                    StartCloudProfile([](EditorCloudModelProfileStore &store) {
                        return store.Load();
                    }, nullptr);
                }
                ImGui::BeginDisabled(Busy() || cloudReview_ || !cloudProfileAdmitted_);
                show(LocalizedText(locale, "cloud_endpoint"));
                if (DrawTextField("##CloudEndpoint", cloudEndpoint_, 256, fonts)) {
                    models_.clear();
                    selection_ = -1;
                    response_.clear();
                    status_ = nullptr;
                }
                if (Button(locale, "cloud_key_enroll", ValidCloudModelEndpoint(cloudEndpoint_)))
                    EnrollCloudCredential();
                if (Button(locale, "cloud_key_revoke", cloudProfile_.has_value()))
                    RevokeCloudCredential();
                if (Button(locale, "discover", CanReviewCloud()))
                    PrepareCloudReview(true);
                show(LocalizedText(locale, "model"));
                if (DrawTextField("##CloudModel", cloudModel_, 256, fonts))
                    selection_ = -1;
                DrawAvailableModels(fonts, cloudModel_);
                if (Button(locale, "cloud_model_save", CanReviewCloud() && !cloudModel_.empty() && cloudProfile_->model != cloudModel_))
                    RememberCloudModel();
                show(LocalizedText(locale, "prompt"));
                static_cast<void>(DrawTextField("##CloudPrompt", cloudPrompt_, 4096, fonts));
                if (Button(locale, "cloud_review", CanReviewCloud() && !cloudModel_.empty() && !cloudPrompt_.empty()))
                    PrepareCloudReview(false);
                ImGui::EndDisabled();
                if (cloudReview_)
                    DrawCloudReview(locale, fonts);
                if (Busy() && !cloudProfileBusy_ && Button(locale, "stop", background_->CanCancel()))
                    background_->Cancel();
                if (status_)
                    show(LocalizedText(locale, status_));
                if (!response_.empty())
                    show(response_);
            }

            void DrawProvider(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "endpoint"), Theme::Text(), fonts);
                ImGui::BeginDisabled(Busy() || awaitingCheckpoint_ || history_.HasRecovery() || ContextLocksComposer() ||
                                     sceneQueryReview_);
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
                DrawAvailableModels(fonts, model_);
                ImGui::EndDisabled();
            }

            void DrawAvailableModels(const Theme::Fonts &fonts, std::string &destination) {
                if (models_.empty())
                    return;
                std::array<const char *, 64> labels{};
                for (std::size_t index = 0; index < models_.size(); ++index)
                    labels[index] = models_[index].id.c_str();
                if (Ui::ComboControl("##AvailableModels", &selection_, labels.data(), static_cast<int>(models_.size()), fonts) &&
                    selection_ >= 0 && static_cast<std::size_t>(selection_) < models_.size())
                    destination = models_[static_cast<std::size_t>(selection_)].id;
            }

            void DrawSendActions(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                if (sceneQueryReview_) {
                    DrawSceneQueryReview(locale, fonts);
                    return;
                }
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
                DrawSceneActions(locale, ready);
            }

            /** @brief Offers explicit Scene evidence or a separately scoped scene count tool. */
            void DrawSceneActions(const ILocalizationService &locale, const bool ready) {
                const bool contextReady = ready && contextLifetime_ && !contextProject_.empty();
                if (Button(locale, "context_scene_selection_offer", contextReady && captureSceneSelection_)) {
                    PrepareSceneSelectionReview(captureSceneSelection_(AgentSceneSelectionPurpose::SelectedObjects),
                                                "context_scene_selection_unavailable");
                    return;
                }
                if (captureAgentViewport_ && Button(locale, "context_viewport_offer", contextReady)) {
                    PrepareSceneSelectionReview(captureAgentViewport_(AgentViewportComponents::None), "context_viewport_unavailable");
                    return;
                }
                const auto count = sceneCount_ && !contextProject_.empty() ? sceneCount_() : std::nullopt;
                if (count && Button(locale, "scene_query_review", ready))
                    sceneQueryReview_ = SceneQueryReview{*count, contextGeneration_, SceneReviewClock::now() + std::chrono::minutes{1}};
            }

            /** @brief Displays only host-defined tool details and flushes a stale or denied preview before dispatch. */
            void DrawSceneQueryReview(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                const auto review = *sceneQueryReview_;
                if (review.expires <= SceneReviewClock::now() || review.projectGeneration != contextGeneration_ || !sceneCount_ ||
                    sceneCount_() != std::optional{review.count}) {
                    sceneQueryReview_.reset();
                    status_ = "context_stale";
                    return;
                }
                const auto width = ImGui::GetContentRegionAvail().x;
                Ui::WrappedText(width, LocalSceneCountTool, Theme::Text(), fonts);
                Ui::WrappedText(width, LocalizedText(locale, "scene_query_arguments"), Theme::Muted(), fonts);
                Ui::WrappedText(width, LocalizedText(locale, "scene_query_scope") + " " + contextProject_, Theme::Muted(), fonts);
                Ui::WrappedText(width, LocalizedText(locale, "scene_query_impact"), Theme::Muted(), fonts);
                Ui::WrappedText(width, LocalizedText(locale, "scene_count") + " " + std::to_string(review.count), Theme::Text(), fonts);
                if (Button(locale, "ask_with_scene_count", CanSendPrompt())) {
                    sceneQueryReview_.reset();
                    Send(prompt_, review.count);
                    return;
                }
                if (Button(locale, "scene_query_deny", true)) {
                    sceneQueryReview_.reset();
                    status_ = "scene_query_denied";
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
            void PrepareSceneSelectionReview(const std::optional<AgentSceneSelectionSnapshot> &snapshot, const char *unavailable) {
                if (!snapshot) {
                    status_ = unavailable;
                    return;
                }
                auto prepared = EditorLocalAgentContext::PrepareSceneSelection(*snapshot, contextProject_, contextGeneration_,
                                                                               contextLifetime_->Token(), turn_ + 1);
                if (prepared.HasError()) {
                    status_ = unavailable;
                    return;
                }
                contextPreview_.emplace(std::move(prepared).Value());
                status_ = nullptr;
            }

            /** @brief Lets an offered viewport preview coexist with a prompt composed after opening it. */
            bool ContextLocksComposer() const noexcept {
                return contextPreview_ && !contextPreview_->Viewport();
            }

            /** @brief Adopts an explicit viewport action on the editor lane, without sending a turn. */
            void ReviewOfferedViewport() {
                if (!takeAgentViewportReview_ || Busy() || contextPreview_ || sceneQueryReview_ || awaitingCheckpoint_)
                    return;
                if (auto offered = takeAgentViewportReview_())
                    PrepareSceneSelectionReview(offered, "context_viewport_unavailable");
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
                if (DrawViewportComponentChoice(locale))
                    return;
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

            /** @brief Rebuilds the preview only when the user changes the component disclosure scope. */
            bool DrawViewportComponentChoice(const ILocalizationService &locale) {
                const auto scope = contextPreview_->Viewport();
                if (!scope || !scope->hasSelection || !captureAgentViewport_)
                    return false;
                const bool hasTransforms = scope->components == AgentViewportComponents::LocalTransforms;
                const auto *label = hasTransforms ? "context_viewport_remove_transforms" : "context_viewport_add_transforms";
                if (!Button(locale, label, true))
                    return false;
                const auto components = hasTransforms ? AgentViewportComponents::None : AgentViewportComponents::LocalTransforms;
                PrepareSceneSelectionReview(captureAgentViewport_(components), "context_viewport_unavailable");
                return true;
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

            /** @brief Rechecks this exact reply with one explicitly named, read-only destination. */
            void ReviewPatchDestination() {
                auto path = SourceDocumentId::Parse(patchDestination_);
                if (path.HasError()) {
                    status_ = "source_patch_invalid";
                    return;
                }
                const auto started = patchCheckpoint_.Start(PatchWorker::Operation{
                    [root = buildRoot_, project = contextProject_, generation = contextGeneration_, path = std::move(path).Value(),
                     files = &files_, offered = patchSource_, reply = response_](const std::stop_token stop) -> PatchWorker::Reply {
                    const CancellationSource cancellation;
                    const std::stop_callback cancel{stop, [&] {
                        cancellation.RequestCancellation();
                    }};
                    const EditorPatchDestination destination{path, root, *files, cancellation.Token()};
                    return PatchPreview{offered.turn, PrepareEditorLocalPatchReview(reply, offered, project, generation, &destination)};
                }});
                status_ = started ? "working" : "source_patch_invalid";
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

            /** @brief Shows a bounded whole-scalar excerpt without opening or creating the destination. */
            void DrawCreatePreview(EditorLocalAgentPatchReview &review, const std::size_t index, const ILocalizationService &locale,
                                   const Theme::Fonts &fonts) {
                const auto *preview = std::get_if<Proposals::FilePreview>(&review.plan->entries[index]);
                if (!preview || !preview->after)
                    return;
                if (Button(locale, "source_patch_create_preview", true))
                    review.expandedFile = review.expandedFile == index ? std::nullopt : std::optional{index};
                if (review.expandedFile != index)
                    return;
                const auto &text = *preview->after;
                constexpr std::size_t MaximumPreviewBytes = 4096;
                auto boundary = std::min(text.size(), MaximumPreviewBytes);
                while (boundary < text.size() && (static_cast<unsigned char>(text[boundary]) & 0xc0U) == 0x80U)
                    --boundary;
                const auto width = ImGui::GetContentRegionAvail().x;
                Ui::WrappedText(width, std::string_view{text}.substr(0, boundary), Theme::Text(), fonts);
                if (boundary < text.size())
                    Ui::WrappedText(width, LocalizedText(locale, "source_patch_create_excerpt"), Theme::Muted(), fonts);
            }

            /** @brief Runs the selected language provider on immutable proposal bytes off the draw lane. */
            void StartPatchDiagnostics(const std::shared_ptr<const Proposals::WorkspaceEditPlan> &plan, std::size_t file,
                                       SourceDocumentId path, bool script) {
                const auto started = patchCheckpoint_.Start(
                    PatchWorker::Operation{[plan, file, path = std::move(path), script, root = buildRoot_,
                                            processes = processes_](const std::stop_token stop) -> PatchWorker::Reply {
                    using Check = Result<Language::LanguageDiagnostics>;
                    if (stop.stop_requested())
                        return PatchDiagnostics{plan, file, Check::Failure(MakeError(SourceDocumentErrors::Cancelled))};
                    const auto &preview = std::get<Proposals::FilePreview>(plan->entries[file]);
                    if (preview.before.Revision() && !preview.before.IsCurrent())
                        return PatchDiagnostics{plan, file, Check::Failure(MakeError(SourceDocumentErrors::Stale))};
                    if (script)
                        return PatchDiagnostics{plan, file, Language::CheckHoroScriptSyntaxText(*preview.after)};
                    return PatchDiagnostics{plan, file,
                                            Language::CheckClangdPreview(*processes, Language::CaptureClangdHostSettings(),
                                                                         {root, path, preview.before, *preview.after}, stop)};
                }});
                if (!started)
                    status_ = script ? "source_patch_syntax_unavailable" : "source_patch_cpp_unavailable";
            }

            /** @brief Presents bounded findings without forwarding provider output to the model or history. */
            void DrawPatchFindings(const Result<Language::LanguageDiagnostics> &result, const bool script,
                                   const ILocalizationService &locale, const Theme::Fonts &fonts) const {
                const auto width = ImGui::GetContentRegionAvail().x;
                if (result.HasError()) {
                    const auto &error = result.ErrorValue();
                    const auto *actionFailure = script ? "source_patch_syntax_unavailable" : "source_patch_cpp_unavailable";
                    const auto &staleBase = SourceDocumentErrors::Stale;
                    const bool stale = ErrorChainContains(error, staleBase.domain, staleBase.code);
                    Ui::WrappedText(width, LocalizedText(locale, stale ? "source_validation_stale" : actionFailure),
                                    Theme::ConsoleWarning(), fonts);
                    return;
                }
                const auto &items = result.Value().items;
                const auto *clean = script ? "source_patch_syntax_clean" : "source_patch_cpp_clean";
                const auto *findings = script ? "source_patch_syntax_invalid" : "source_patch_cpp_findings";
                const auto *label = items.empty() ? clean : findings;
                Ui::WrappedText(width, LocalizedText(locale, label), items.empty() ? Theme::Muted() : Theme::ConsoleWarning(), fonts);
                constexpr std::size_t MaximumPresentedDiagnostics = 8;
                for (const auto &item : std::span{items}.first(std::min(items.size(), MaximumPresentedDiagnostics)))
                    Ui::WrappedText(width, item.message, Theme::Muted(), fonts);
                if (items.size() > MaximumPresentedDiagnostics)
                    Ui::WrappedText(width, LocalizedText(locale, "source_patch_diagnostics_truncated"), Theme::Muted(), fonts);
            }

            /** @brief Offers a detached language check for the exact proposed file. */
            void DrawPatchDiagnostics(const EditorLocalAgentPatchReview &review, std::size_t index, const ILocalizationService &locale,
                                      const Theme::Fonts &fonts) {
                const auto &file = review.files[index];
                const auto &destination = file.destination ? *file.destination : file.path;
                const auto *preview = std::get_if<Proposals::FilePreview>(&review.plan->entries[index]);
                if (!preview || !preview->after)
                    return;
                const auto language = ClassifySourceLanguage(destination.Value());
                const bool script = language == SourceLanguage::HoroScript;
                if (!script && (language != SourceLanguage::Cpp || !processes_ || buildRoot_.empty()))
                    return;
                const auto *action = script ? "source_patch_check_syntax" : "source_patch_check_cpp";
                if (Button(locale, action, !patchCheckpoint_.Busy()))
                    StartPatchDiagnostics(review.plan, index, destination, script);
                const auto observed = std::ranges::find_if(patchDiagnostics_, [&review, index](const PatchDiagnostics &candidate) {
                    return candidate.file == index && candidate.plan.lock() == review.plan;
                });
                if (observed != patchDiagnostics_.end())
                    DrawPatchFindings(observed->diagnostics, script, locale, fonts);
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
                const bool canOpen = !rejected && !conflict;
                if (file.kind == EditorPatchFileKind::Create)
                    DrawCreatePreview(review, index, locale, fonts);
                else if (Button(locale, "source_patch_review", canOpen))
                    status_ = requestSourceReview_(review.proposal, *review.plan, index, command) ? nullptr : "source_patch_unavailable";
                if (canOpen)
                    DrawPatchDiagnostics(review, index, locale, fonts);
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
                const bool completed = state.feedback == Applied || state.feedback == Deleted || state.feedback == DeleteRecovery ||
                                       state.feedback == Renamed || state.feedback == RenameRecovery;
                if (state.feedback != Preparing && std::exchange(sourceApplyQueued_, false) && completed)
                    patchReview_.reset();
                constexpr std::array<const char *, 15> keys{nullptr,
                                                            "source_patch_applied",
                                                            "source_patch_reverted",
                                                            "source_patch_apply_failed",
                                                            "source_patch_revert_failed",
                                                            "source_patch_transaction_conflict",
                                                            "source_patch_transaction_cancelled",
                                                            "source_patch_transaction_preparing",
                                                            "source_patch_delete_complete",
                                                            "source_patch_delete_recovery",
                                                            "source_patch_rename_complete",
                                                            "source_patch_rename_recovery",
                                                            "source_patch_group_recovery",
                                                            "source_patch_cross_volume",
                                                            "source_patch_move_unsupported"};
                if (const auto *key = keys[static_cast<std::size_t>(state.feedback)])
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, key), Theme::Text(), fonts);
                if (state.audit)
                    DrawSourceAudit(*state.audit, state.disposition, locale, fonts);
                if (state.auditHistoryError)
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "source_patch_audit_history_error"),
                                    Theme::Text(), fonts);
                DrawSourceValidation(state, locale, fonts, command);
                if (requestSourceReviewUndo_ && contextLifetime_ && state.canUndo && Button(locale, "source_patch_revert", true))
                    status_ = requestSourceReviewUndo_(contextLifetime_->Token(), command) ? nullptr : "source_patch_revert_failed";
            }

            /** @brief Shows exact build/test outcome and queues only the clicked validation action. */
            void DrawSourceValidation(const Proposals::SourceTransactionStatus &state, const ILocalizationService &locale,
                                      const Theme::Fonts &fonts, EditorWorkspaceViewCommandData &command) {
                using enum Proposals::SourceReviewValidation;
                constexpr std::array<const char *, 10> labels{nullptr,
                                                              "source_validation_save",
                                                              nullptr,
                                                              "source_validation_building",
                                                              "source_validation_build_passed",
                                                              "source_validation_build_failed",
                                                              "source_validation_testing",
                                                              "source_validation_tests_passed",
                                                              "source_validation_tests_failed",
                                                              "source_validation_stale"};
                if (const auto *label = labels[static_cast<std::size_t>(state.validation)])
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, label), Theme::Text(), fonts);
                if (!contextLifetime_ || !state.audit || !requestSourceReviewValidation_)
                    return;
                if ((state.validation == Ready || state.validation == BuildFailed) &&
                    Button(locale, "source_validation_approve_build", true))
                    requestSourceReviewValidation_(SourceReviewValidationAction::Build, state, contextLifetime_->Token(), command);
                else if (state.canRunTests && Button(locale, "source_validation_approve_test", true))
                    requestSourceReviewValidation_(SourceReviewValidationAction::Test, state, contextLifetime_->Token(), command);
            }

            /** @brief Presents the content-free proposal identifier for the latest successful application. */
            void DrawSourceAudit(const Proposals::SourceReviewAudit &audit, const Proposals::SourceReviewDisposition disposition,
                                 const ILocalizationService &locale, const Theme::Fonts &fonts) {
                constexpr std::array keys{"source_patch_audit_historical", "source_patch_audit_applied", "source_patch_audit_saved",
                                          "source_patch_audit_reverted", "source_patch_audit_stale"};
                const auto label =
                    std::format("{} {} · {}", LocalizedText(locale, "source_patch_audit_proposal"), FormatSha256(audit.proposal.digest),
                                LocalizedText(locale, keys[static_cast<std::size_t>(disposition)]));
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
                    PatchWorker::Operation{[root = buildRoot_, project = contextProject_, files = &files_,
                                            sources = std::move(sources)](const std::stop_token stop) -> PatchWorker::Reply {
                    const CancellationSource cancellation;
                    const std::stop_callback cancel{stop, [&] {
                        cancellation.RequestCancellation();
                    }};
                    return LoadEditorPatchCheckpoint(root, project, sources, cancellation.Token(), files);
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

            /** @brief Publishes the destination observation as an inert review in the attached workspace. */
            void AdoptPatchCheckpoint(PatchPreview &preview) {
                if (preview.turn != patchSource_.turn)
                    return;
                if (preview.review.HasError()) {
                    status_ = "source_patch_invalid";
                    return;
                }
                patchReview_.emplace(std::move(preview.review).Value());
                status_ = nullptr;
            }

            /** @brief Drops results from superseded review plans before showing any provider text. */
            void AdoptPatchCheckpoint(PatchDiagnostics &result) {
                if (!patchReview_ || result.plan.lock() != patchReview_->plan)
                    return;
                std::erase_if(patchDiagnostics_, [this, &result](const PatchDiagnostics &previous) {
                    return previous.file == result.file || previous.plan.lock() != patchReview_->plan;
                });
                patchDiagnostics_.emplace_back(std::move(result));
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

            /** @brief Lets the user name the exact structural destination without reading any source from it. */
            void DrawPatchDestination(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                if (!patchSource_.turn || !requestSourceReview_ || buildRoot_.empty() || !IsEditorLocalPatchReply(response_))
                    return;
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, LocalizedText(locale, "source_patch_destination"), Theme::Text(), fonts);
                ImGui::BeginDisabled(!CanCheckpointPatchReview());
                static_cast<void>(DrawTextField("##PatchDestination", patchDestination_, MaximumSourceDocumentIdBytes, fonts));
                if (Button(locale, "source_patch_destination_review", !patchDestination_.empty()))
                    ReviewPatchDestination();
                ImGui::EndDisabled();
            }

            /** @brief Renders file summaries in proposal order and stages only the selected current operation. */
            void DrawPatchReview(const ILocalizationService &locale, const Theme::Fonts &fonts, EditorWorkspaceViewCommandData &command) {
                if (!patchReview_) {
                    DrawPatchDestination(locale, fonts);
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
                const auto kind = EditorPatchApplyKind(review);
                const auto *action = PatchApplyLabel(kind.value_or(EditorPatchFileKind::Edit), review.decisions.AcceptedFileCount() > 1);
                if (requestSourceReviewApply_ && Button(locale, action, kind.has_value() && !writingAudit))
                    QueueSourceApply(review, command);
                if (!buildRoot_.empty() && Button(locale, "source_patch_checkpoint_save", CanCheckpointPatchReview()))
                    SavePatchReview(review);
                ImGui::EndDisabled();
            }

            /** @brief Gives native file moves an explicit confirmation action. */
            static const char *PatchApplyLabel(const EditorPatchFileKind kind, const bool grouped) {
                switch (kind) {
                    case EditorPatchFileKind::Delete:
                        return grouped ? "source_patch_confirm_moves" : "source_patch_confirm_delete";
                    case EditorPatchFileKind::Rename:
                        return grouped ? "source_patch_confirm_moves" : "source_patch_confirm_rename";
                    default:
                        return "source_patch_apply";
                }
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
                    auto sceneQuery = sceneCount ? std::make_shared<SceneQueryProgress>() : nullptr;
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
                        patchDestination_.clear();
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
                cloudProfileBusy_ = false;
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

            /** @brief Installs the current durable binding without resolving its key. */
            void Adopt(CloudProfileReply &result) {
                if (result.outcome.HasError()) {
                    status_ = "cloud_profile_failed";
                    return;
                }
                cloudProfile_ = std::move(result.outcome).Value();
                cloudProfileAdmitted_ = true;
                if (cloudProfile_) {
                    cloudEndpoint_ = cloudProfile_->endpoint;
                    cloudModel_ = cloudProfile_->model;
                }
                status_ = result.status;
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
                    status_ = FailureKey(discovery.outcome.error->code, cloudMode_);
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
                    if (!cloudMode_) {
                        CapturePatchReview();
                        ReviewCompleted(result);
                    }
                    return;
                }
                const auto *timeout = cloudMode_ ? "cloud_timeout" : "timeout";
                status_ = result.status == AgentTurnStatus::TimedOut ? timeout : FailureKey(result.providerError, cloudMode_);
                if (cloudMode_)
                    return;
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
            std::string buttonLabel_; /**< Scratch for immediate-mode localized text and stable widget IDs. */
            std::shared_ptr<EditorCloudModelProfileStore> cloudProfiles_;
            bool cloudMode_{};
            bool cloudProfileBusy_{};
            bool cloudProfileRead_{};
            bool cloudProfileAdmitted_{};
            std::string cloudEndpoint_;
            std::string cloudModel_;
            std::string cloudPrompt_;
            std::optional<EditorCloudModelProfile> cloudProfile_;
            std::optional<CloudTurnReview> cloudReview_;
            std::vector<ModelDescriptor> models_;
            std::function<std::optional<std::size_t>()> sceneCount_;
            std::function<std::optional<AgentSceneSelectionSnapshot>(AgentSceneSelectionPurpose)> captureSceneSelection_;
            decltype(PanelContext::captureAgentViewport) captureAgentViewport_;
            decltype(PanelContext::takeAgentViewportReview) takeAgentViewportReview_;
            std::function<std::optional<AgentEditorDiagnosticsSnapshot>()> captureEditorDiagnostics_;
            std::function<bool(SourceDocumentSnapshot &)> activeSource_;
            std::function<bool(SourceDocumentSnapshot &, SourceContextRange &)> activeSourceRange_;
            std::function<std::span<const SourceDocumentSnapshot>()> openSources_;
            decltype(PanelContext::requestSourceReview) requestSourceReview_;
            decltype(PanelContext::requestSourceReviewApply) requestSourceReviewApply_;
            decltype(PanelContext::requestSourceReviewUndo) requestSourceReviewUndo_;
            decltype(PanelContext::sourceReviewTransaction) sourceReviewTransaction_;
            decltype(PanelContext::requestSourceReviewValidation) requestSourceReviewValidation_;
            decltype(PanelContext::requestMcpAudit) requestMcpAudit_;
            bool sourceApplyQueued_{};
            EditorLocalAgentPatchSource patchSource_;
            std::optional<EditorLocalAgentPatchReview> patchReview_;
            std::vector<PatchDiagnostics> patchDiagnostics_;
            std::string patchDestination_;
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
            std::optional<SceneQueryReview> sceneQueryReview_;
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
            IExternalProcessRunner *processes_{};
            PatchWorker patchCheckpoint_; /**< Joins before panel teardown while the host's file service is alive. */
        };
    }  // namespace

    /** @copydoc MakeEditorLocalAgentPanel */
    EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files,
                                                        std::shared_ptr<EditorCloudModelProfileStore> cloudProfiles,
                                                        IExternalProcessRunner *processes, EditorImmersiveListening *listening) {
        auto background = std::make_shared<BackgroundTurn>();
        auto panel = std::make_shared<LocalAgentPanel>(jobs, files, background, std::move(cloudProfiles), processes, listening);
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
