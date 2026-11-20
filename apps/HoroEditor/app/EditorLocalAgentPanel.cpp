#include "EditorLocalAgentPanel.h"

#include "AgentTurn.h"
#include "Horo/Agent/ReferenceModelProviders.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/IWorkspacePanel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <optional>
#include <stop_token>
#include <variant>

namespace Horo::Editor {
    namespace {
        using namespace Agent;
        using Reply = std::variant<ProviderDiscovery, AgentTurnResult>;

        /** @brief Runs one user-initiated discovery or text-only inference on an isolated worker. */
        Reply RunLocalOperation(std::string endpoint, std::optional<ModelRequest> request, std::uint64_t turn, std::stop_token stop) {
            auto provider = CreateOllamaModelProvider({.endpoint = std::move(endpoint)});
            if (!request)
                return provider->Discover(stop);
            AgentTurnRuntime runtime;
            return runtime.Run(*provider, nullptr, turn, std::move(*request), {}, stop);
        }

        /** @brief Presents sanitized, localized remediation without displaying transport response bodies. */
        const char *FailureKey(const std::optional<ModelErrorCode> error) {
            using enum ModelErrorCode;
            if (error == InvalidRequest)
                return "invalid";
            if (error == Cancelled)
                return "cancelled";
            if (error == Unavailable || error == Transport)
                return "unavailable";
            return "failed";
        }

        /** @brief Owns one optional asynchronous request; workspace callbacks remain on the editor lane. */
        class LocalAgentPanel final : public IWorkspacePanel {
        public:
            LocalAgentPanel() : IWorkspacePanel("workspace.local_agent") {}

            ~LocalAgentPanel() override {
                stop_.request_stop();
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

            void OnAttach(PanelContext &) override {}

            void OnDetach() override {
                stop_.request_stop();
                discard_ = true;
                prompt_.clear();
                response_.clear();
                status_ = nullptr;
            }

            void DrawIcon(ImDrawList *draw, const ImVec2 &position, const ImVec2 &size, ImU32 color) override {
                const ImVec2 low{position.x + size.x * 0.15F, position.y + size.y * 0.2F};
                const ImVec2 high{position.x + size.x * 0.85F, position.y + size.y * 0.7F};
                draw->AddRect(low, high, color, size.x * 0.12F);
                draw->AddLine({low.x + size.x * 0.15F, high.y}, {low.x + size.x * 0.15F, high.y + size.y * 0.12F}, color);
            }

            void DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &, EditorWorkspaceViewCommandData &,
                           const EditorGuiContext &context) override {
                Poll();
                const auto &locale = context.localization;
                const auto &fonts = context.theme.fonts;
                ImGui::BeginChild("##LocalAgent", size, false, ImGuiWindowFlags_NoSavedSettings);
                DrawProvider(locale, fonts);
                ImGui::TextUnformatted(LocalizedText(locale, "prompt").c_str());
                static_cast<void>(Ui::InputTextControl("##Prompt", prompt_, 4096, fonts));
                if (Button(locale, "ask", !Busy() && !endpoint_.empty() && !model_.empty() && !prompt_.empty())) {
                    ModelRequest request;
                    request.model = model_;
                    request.messages.push_back({.role = ModelRole::User, .text = prompt_});
                    Start(std::move(request));
                }
                if (Busy() && Button(locale, "stop", !stop_.stop_requested()))
                    stop_.request_stop();
                if (status_)
                    ImGui::TextWrapped("%s", LocalizedText(locale, status_).c_str());
                if (!response_.empty())
                    ImGui::TextWrapped("%s", response_.c_str());
                ImGui::EndChild();
            }

        private:
            [[nodiscard]] bool Busy() const {
                return reply_.valid();
            }

            [[nodiscard]] bool Button(const ILocalizationService &locale, const char *key, bool enabled) const {
                const auto label = LocalizedText(locale, key) + "##" + key;
                return Ui::Button({.label = label.c_str(), .enabled = enabled});
            }

            void DrawProvider(const ILocalizationService &locale, const Theme::Fonts &fonts) {
                ImGui::TextUnformatted(LocalizedText(locale, "endpoint").c_str());
                ImGui::BeginDisabled(Busy());
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

            void Start(std::optional<ModelRequest> request) {
                try {
                    stop_ = std::stop_source{};
                    reply_ = std::async(std::launch::async, RunLocalOperation, endpoint_, std::move(request), ++turn_, stop_.get_token());
                    discard_ = false;
                    status_ = "working";
                    response_.clear();
                } catch (const std::exception &) {
                    status_ = "failed";
                }
            }

            void Poll() {
                if (!Busy() || reply_.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
                    return;
                try {
                    auto reply = reply_.get();
                    if (discard_)
                        status_ = nullptr;
                    else if (stop_.stop_requested())
                        status_ = "cancelled";
                    else
                        std::visit([this](auto &result) {
                            Adopt(result);
                        }, reply);
                } catch (const std::exception &) {
                    status_ = discard_ ? nullptr : "failed";
                }
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
                if (result.status == AgentTurnStatus::Completed && !result.messages.empty()) {
                    response_ = std::move(result.messages.back().text);
                    status_ = response_.empty() ? "empty_response" : nullptr;
                    return;
                }
                status_ = result.status == AgentTurnStatus::TimedOut ? "timeout" : FailureKey(result.providerError);
            }

            std::string endpoint_;
            std::string model_;
            std::string prompt_;
            std::string response_;
            std::vector<ModelDescriptor> models_;
            int selection_{-1};
            const char *status_{};
            std::uint64_t turn_{};
            bool discard_{};
            std::stop_source stop_;
            std::future<Reply> reply_;
        };
    }  // namespace

    /** @copydoc MakeEditorLocalAgentPanel */
    std::shared_ptr<IWorkspacePanel> MakeEditorLocalAgentPanel() {
        return std::make_shared<LocalAgentPanel>();
    }
}  // namespace Horo::Editor
