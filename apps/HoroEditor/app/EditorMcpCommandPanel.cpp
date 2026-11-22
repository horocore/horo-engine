#include "EditorMcpCommandPanel.h"

#include "EditorBuildMcpTools.h"
#include "EditorDebugCaptureMcpTools.h"
#include "EditorInspectionMcpTools.h"
#include "EditorMcpWorkspaceHost.h"
#include "EditorSourceMcpTools.h"
#include "EditorTestMcpTools.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Runtime/Input.h"

#include <array>
#include <optional>
#include <utility>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;

        /** @brief Exact choices displayed at local client admission. */
        struct CommandPack {
            const char *label;
            std::array<std::string_view, 2> grants;
        };

        constexpr std::array Packs{
            CommandPack{"open_source", {EditorSourcePresentationGrant, {}}},
            CommandPack{"open_build", {EditorBuildExecutionGrant, EditorBuildObservationGrant}},
            CommandPack{"open_tests", {EditorTestExecutionGrant, EditorTestObservationGrant}},
            CommandPack{"open_inspection", {EditorSceneInspectionGrant, {}}},
            CommandPack{"open_scene_proposals", {EditorSceneInspectionGrant, EditorSceneProposalGrant}},
            CommandPack{"open_debug_capture", {EditorDebugCaptureGrant, {}}},
        };

        /** @brief Keeps approved bytes independent of the editor input widgets. */
        struct Review {
            Mcp::McpRequest request;
            std::uint64_t challenge{};
        };

        /** @brief Authenticated, editor-local client and explicit per-request approval surface. */
        class CommandPanel final : public IWorkspacePanel {
        public:
            explicit CommandPanel(EditorMcpWorkspaceHost &host) : IWorkspacePanel("workspace.mcp_commands"), host_(host) {}

            std::string GetId() const override {
                return "horo.mcp_commands";
            }

            std::string GetDisplayName() const override {
                return "workspace.mcp_commands.title";
            }

            WorkspaceDockArea GetDefaultDockArea() const override {
                return WorkspaceDockArea::Right;
            }

            std::vector<std::string> GetObservedEventTypes() const override {
                return {};
            }

            void OnAttach(PanelContext &context) override {
                OnDetach();
                project_ = context.projectIdentity;
                input_ = context.inputRouter;
            }

            void OnDetach() override {
                CloseClient();
                project_.clear();
                input_ = nullptr;
            }

            void OnPresentationHidden(EditorWorkspaceViewCommandData &) override {
                inputFocus_.Reset();
                Decide(false);
            }

            void DrawIcon(ImDrawList *draw, const ImVec2 &p, const ImVec2 &size, const ImU32 color) override {
                const auto point = [&](const float x, const float y) {
                    return ImVec2{p.x + x * size.x, p.y + y * size.y};
                };
                draw->AddRect(point(0.12F, 0.22F), point(0.88F, 0.78F), color, size.x * 0.1F);
                draw->AddLine(point(0.3F, 0.39F), point(0.43F, 0.5F), color);
                draw->AddLine(point(0.43F, 0.5F), point(0.3F, 0.61F), color);
                draw->AddLine(point(0.53F, 0.61F), point(0.71F, 0.61F), color);
            }

            void DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &, EditorWorkspaceViewCommandData &,
                           const EditorGuiContext &context) override {
                ImGui::BeginChild("##McpCommands", size, false, ImGuiWindowFlags_NoSavedSettings);
                const auto &locale = context.localization;
                if (client_ && !client_->Session().IsValid())
                    CloseClient();
                ImGui::TextUnformatted(LocalizedText(locale, "project").c_str());
                Describe(context, project_);
                if (client_) {
                    PollOperation();
                    DrawSession(context);
                } else {
                    for (const auto &pack : Packs)
                        if (Button(locale, pack.label, !project_.empty()))
                            Open(pack);
                }
                if (message_)
                    Describe(context, LocalizedText(locale, message_));
                if (!result_.empty())
                    Describe(context, result_);
                ImGui::EndChild();
            }

        private:
            /** @brief Presents locale-independent protocol values without format-string interpretation. */
            static void Describe(const EditorGuiContext &context, const std::string &text) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, text, Theme::Text(), context.theme.fonts);
            }

            /** @brief Draws a stable, translated action using the shared editor control. */
            bool Button(const ILocalizationService &locale, const char *key, const bool enabled = true) const {
                const auto label = LocalizedText(locale, key) + "###" + key;
                return Ui::Button({.label = label.c_str(), .enabled = enabled, .style = {.width = Ui::StyleWidth::FillAvailable}});
            }

            /** @brief Opens only the capability group explicitly chosen by the operator. */
            void Open(const CommandPack &pack) {
                std::vector<std::string> grants;
                for (const auto grant : pack.grants)
                    if (!grant.empty())
                        grants.emplace_back(grant);
                auto opened = host_.Open("editor-command-panel", std::move(grants));
                if (opened.HasError()) {
                    message_ = "unavailable";
                    return;
                }
                client_ = std::move(opened).Value();
                auto listed = client_->Call({.id = "list", .method = "tools/list"});
                if (listed.HasError()) {
                    CloseClient();
                    message_ = "unavailable";
                    return;
                }
                for (const auto &entry : listed.Value().at("tools"))
                    tools_.push_back(entry.at("name").get<std::string>());
                message_ = nullptr;
            }

            /** @brief Withdraws exact approval and closes only the active panel client. */
            void CloseClient() {
                Decide(false);
                inputFocus_.Reset();
                if (client_ && client_->Session().IsValid())
                    static_cast<void>(host_.CloseSession());
                client_.reset();
                tools_.clear();
                selected_.clear();
                arguments_.clear();
                operation_.reset();
                result_.clear();
                message_ = nullptr;
            }

            /** @brief Renders either the exact pending request or the editable command draft. */
            void DrawSession(const EditorGuiContext &context) {
                const auto &locale = context.localization;
                if (Button(locale, "close")) {
                    CloseClient();
                    return;
                }
                if (review_) {
                    Describe(context, LocalizedText(locale, "approve_hint"));
                    Describe(context, review_->request.params.dump());
                    if (Button(locale, "approve")) {
                        Decide(true);
                    } else if (Button(locale, "deny")) {
                        Decide(false);
                    }
                    return;
                }
                for (const auto &tool : tools_)
                    if (Ui::Button({.label = tool.c_str(), .style = {.width = Ui::StyleWidth::FillAvailable}}))
                        selected_ = tool;
                if (!selected_.empty()) {
                    Describe(context, selected_);
                    DrawArguments(context);
                }
            }

            /** @brief Captures bounded JSON and holds the text keyboard context only while the widget is active. */
            void DrawArguments(const EditorGuiContext &context) {
                const auto &locale = context.localization;
                ImGui::TextUnformatted(LocalizedText(locale, "arguments").c_str());
                static_cast<void>(Ui::InputTextControl("##CommandArguments", arguments_, 4096, context.theme.fonts));
                if (!ImGui::IsItemActive())
                    inputFocus_.Reset();
                else if (input_ && !inputFocus_.IsActive())
                    inputFocus_ =
                        input_->PushContext(Input::InputContextId{"editor.mcp_commands.text"}, Input::InputContextKind::FocusedGuiWidget);
                if (Button(locale, "review", !operation_))
                    BeginRequest();
            }

            /** @brief Validates the selected tool and stages exact approval for side effects. */
            void BeginRequest() {
                auto *session = host_.ActiveSession();
                if (!session)
                    return;
                auto args = arguments_.empty() ? Json::object() : Json::parse(arguments_, nullptr, false);
                if (!args.is_object()) {
                    message_ = "invalid";
                    return;
                }
                const auto effect = session->ToolEffect(selected_);
                if (effect.HasError()) {
                    message_ = "unavailable";
                    return;
                }
                Mcp::McpRequest request{.id = ++requestId_,
                                        .method = "tools/call",
                                        .params = {{"name", selected_}, {"arguments", std::move(args)}}};
                result_.clear();
                message_ = nullptr;
                inputFocus_.Reset();
                if (effect.Value() == Mcp::McpToolEffect::Query) {
                    Submit(request);
                    return;
                }
                const auto challenge = session->RequestApproval(request);
                if (challenge.HasError()) {
                    message_ = "unavailable";
                    return;
                }
                review_ = Review{std::move(request), challenge.Value()};
            }

            /** @brief Submits the exact approved request and tracks its controller result across frames. */
            void Submit(const Mcp::McpRequest &request) {
                const auto accepted = client_->Call(request);
                if (accepted.HasError()) {
                    message_ = "rejected";
                    return;
                }
                operation_ = accepted.Value().at("operationId").get<std::uint64_t>();
                message_ = "queued";
            }

            /** @brief Consumes this panel's visible review once and drops it on denial or departure. */
            void Decide(const bool approved) {
                auto review = std::exchange(review_, std::nullopt);
                if (!review)
                    return;
                auto *session = host_.ActiveSession();
                if (!session || !client_ || !client_->Session().IsValid() || session->Decide(review->challenge, approved).HasError()) {
                    message_ = "rejected";
                    return;
                }
                if (approved) {
                    Submit(review->request);
                } else {
                    message_ = "denied";
                }
            }

            /** @brief Polls one bounded controller result; native work remains owned by its application service. */
            void PollOperation() {
                if (!operation_)
                    return;
                const auto status = client_->Call({.id = "read", .method = "operations/get", .params = {{"operationId", *operation_}}});
                if (status.HasError()) {
                    message_ = "rejected";
                    operation_.reset();
                    return;
                }
                const auto &view = status.Value();
                const auto &state = view.at("status").get_ref<const std::string &>();
                if (state == "queued" || state == "running")
                    return;
                message_ = state == "succeeded" ? "completed" : "rejected";
                result_ = (state == "succeeded" ? view.at("result") : view.at("error")).dump();
                operation_.reset();
            }

            EditorMcpWorkspaceHost &host_;
            Input::InputRouter *input_{};
            Input::InputContextToken inputFocus_;
            std::shared_ptr<const Mcp::McpInProcessAdapter> client_;
            std::string project_;
            std::vector<std::string> tools_;
            std::string selected_;
            std::string arguments_;
            std::string result_;
            std::optional<Review> review_;
            std::optional<std::uint64_t> operation_;
            std::uint64_t requestId_{};
            const char *message_{};
        };
    }  // namespace

    /** @copydoc MakeEditorMcpCommandPanel */
    std::shared_ptr<IWorkspacePanel> MakeEditorMcpCommandPanel(EditorMcpWorkspaceHost &host) {
        return std::make_shared<CommandPanel>(host);
    }
}  // namespace Horo::Editor
