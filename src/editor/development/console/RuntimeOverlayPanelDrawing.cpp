#include "Horo/Editor/EditorUiComponents.h"
#include "RuntimeConsoleRecordList.h"
#include "RuntimeOverlayPanels.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        constexpr std::array<std::string_view, 7> SeverityKeys{"severity.trace", "severity.debug",    "severity.info", "severity.warn",
                                                               "severity.error", "severity.critical", "severity.off"};
        constexpr std::array<std::string_view, 7> StateKeys{"state.queued",    "state.running", "state.waiting",  "state.cancelling",
                                                            "state.succeeded", "state.failed",  "state.cancelled"};

        static_assert(SeverityKeys.size() == static_cast<std::size_t>(Log::Level::Off) + 1);
        static_assert(StateKeys.size() == static_cast<std::size_t>(OperationState::Cancelled) + 1);
    }  // namespace

    /** @copydoc RuntimeOverlayPanels::DrawPanel */
    void RuntimeOverlayPanels::DrawPanel(const Debug::OverlayPanelSnapshot &panel, const ILocalizationService &localization,
                                         const Theme::Fonts &fonts) {
        const bool logs = panel.source == Debug::OverlaySource::Logs;
        const auto *viewport = ImGui::GetMainViewport();
        const auto condition = ImGui::IsMouseDown(ImGuiMouseButton_Left) ? ImGuiCond_Appearing : ImGuiCond_Always;
        ImGui::SetNextWindowPos({viewport->WorkPos.x + panel.bounds.x * viewport->WorkSize.x,
                                 viewport->WorkPos.y + panel.bounds.y * viewport->WorkSize.y},
                                condition);
        ImGui::SetNextWindowSize({panel.bounds.width * viewport->WorkSize.x, panel.bounds.height * viewport->WorkSize.y}, condition);
        ImGui::SetNextWindowSizeConstraints({std::min(Ui::ScaledLayoutValue(240), viewport->WorkSize.x),
                                             std::min(Ui::ScaledLayoutValue(180), viewport->WorkSize.y)},
                                            viewport->WorkSize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg1());
        const bool scene = panel.source == Debug::OverlaySource::Scene;
        const bool network = panel.source == Debug::OverlaySource::Network;
        if (ImGui::Begin(network ? "##RuntimeNetworkOverlay"
                         : scene ? "##RuntimeSceneOverlay"
                         : logs  ? "##RuntimeLogsOverlay"
                                 : "##RuntimeJobsOverlay",
                         nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
            CaptureLayout(panel);
            Ui::Card card({.id = "runtime.overlay.panel"});
            const std::array actions{Ui::CardTitleBarAction{.id = "close",
                                                            .icon = Ui::UiIcon::VisibilityOff,
                                                            .title = LocalizedText(localization, "close_panel").c_str(),
                                                            .onInvoke = [this, source = panel.source] {
                RequestPanel(source, false);
            }}};
            card.DrawTitleBar({"panel.title",
                               LocalizedText(localization, network ? "source.network"
                                                           : scene ? "source.scene"
                                                           : logs  ? "source.logs"
                                                                   : "source.jobs")
                                   .c_str(),
                               fonts, actions});
            if (card.BeginBody() && requested_[static_cast<std::size_t>(panel.source)]) {
                if (network) {
                    DrawNetwork(localization, fonts);
                } else if (scene) {
                    DrawScene(localization, fonts);
                } else {
                    DrawStorePanel(logs, localization, fonts);
                }
            }
            closeRequested_ |= WantsClose();
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    /** @copydoc RuntimeOverlayPanels::DrawStorePanel */
    void RuntimeOverlayPanels::DrawStorePanel(const bool logs, const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (logs)
            DrawLogFilter(localization, fonts);
        else
            Ui::Hint(LocalizedText(localization, "job_scope").c_str(), fonts);
        std::array<std::string_view, 7> labels;
        const auto &keys = logs ? SeverityKeys : StateKeys;
        for (std::size_t index = 0; index < labels.size(); ++index)
            labels[index] = LocalizedText(localization, keys[index]);
        auto &selected = logs ? selectedLog_ : selectedJob_;
        const auto result =
            Ui::RuntimeStoreRecordList({stores_, logs ? Ui::RuntimeStoreRecordKind::Logs : Ui::RuntimeStoreRecordKind::Jobs, labels,
                                        "##records", LocalizedText(localization, logs ? "empty_logs" : "empty_jobs"), selected,
                                        std::max(Ui::ScaledLayoutValue(80), ImGui::GetWindowHeight() * 0.35F), true, logs},
                                       fonts);
        if (result.activated)
            selected = result.activated;
        DrawDetails(logs, localization, fonts);
        if (!errorCode_.empty())
            Ui::ErrorText(LocalizedText(localization, "refresh_failed").c_str(), fonts);
    }

    /** @copydoc RuntimeOverlayPanels::DrawLogFilter */
    void RuntimeOverlayPanels::DrawLogFilter(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        std::array<const char *, 7> levels;
        for (std::size_t index = 0; index < levels.size(); ++index)
            levels[index] = LocalizedText(localization, SeverityKeys[index]).c_str();
        Ui::FieldLabel(LocalizedText(localization, "severity").c_str(), fonts);
        bool changed = Ui::ComboControl("##severity", &severity_, levels.data(), static_cast<int>(levels.size()), fonts);
        changed |=
            Ui::InputTextControl("##category", category_.data(), category_.size(), fonts,
                                 {.hint = LocalizedText(localization, "category").c_str(), .componentSize = Ui::ComponentSize::Medium});
        changed |=
            Ui::InputTextControl("##filter", text_.data(), text_.size(), fonts,
                                 {.hint = LocalizedText(localization, "filter").c_str(), .componentSize = Ui::ComponentSize::Medium});
        const std::array<const char *, 2> matches{LocalizedText(localization, "match.exact").c_str(),
                                                  LocalizedText(localization, "match.namespace").c_str()};
        changed |= Ui::ComboControl("##match", &categoryMatch_, matches.data(), 2, fonts);
        Ui::FieldLabel(LocalizedText(localization, "collapse").c_str(), fonts);
        changed |= Ui::ToggleControl("##collapse", &collapse_, fonts, false);
        if (changed) {
            const auto applied =
                stores_.SetLogFilter({static_cast<Log::Level>(severity_), category_.data(),
                                      static_cast<Debug::ConsoleLogCategoryMatch>(categoryMatch_), text_.data(), collapse_});
            if (applied.HasError())
                errorCode_ = applied.ErrorValue().code.Value().substr(0, 256);
        }
    }

    /** @copydoc RuntimeOverlayPanels::DrawDetails */
    void RuntimeOverlayPanels::DrawDetails(const bool logs, const ILocalizationService &localization, const Theme::Fonts &fonts) {
        const auto selected = logs ? selectedLog_ : selectedJob_;
        const auto count = logs ? stores_.LogRowCount() : stores_.JobRowCount();
        if (!selected || *selected >= count)
            return;
        std::string_view title, message, phase, error;
        std::optional<OperationId> operation;
        bool truncated{};
        if (logs) {
            const auto &row = stores_.Logs(*selected, 1).front();
            title = row.category;
            message = row.message;
            truncated = row.truncated;
        } else {
            const auto &row = stores_.Jobs(*selected, 1).front();
            title = row.title;
            message = row.message;
            phase = row.phase;
            error = row.errorCode;
            operation = row.source->id;
            truncated = row.truncated;
        }
        if (ImGui::BeginChild("##details", {0, Ui::ScaledLayoutValue(120)}, ImGuiChildFlags_Borders)) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, title, Theme::Text(), fonts);
            if (operation) {
                Ui::FieldLabel(LocalizedText(localization, "operation_id").c_str(), fonts);
                std::array<char, 32> text;
                const auto number = std::to_chars(text.data(), text.data() + text.size(), *operation);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, {text.data(), static_cast<std::size_t>(number.ptr - text.data())},
                                Theme::Muted(), fonts);
            }
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, phase, Theme::Muted(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, message, Theme::Muted(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, error, Theme::Err(), fonts);
            if (truncated)
                Ui::Hint(LocalizedText(localization, "truncated").c_str(), fonts);
        }
        ImGui::EndChild();
        if (Ui::Button({.label = LocalizedText(localization, "copy").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = !message.empty(),
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const std::string copied(message);
            ImGui::SetClipboardText(copied.c_str());
        }
    }

}  // namespace Horo::Editor
