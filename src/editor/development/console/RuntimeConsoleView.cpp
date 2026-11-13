#include "RuntimeConsoleView.h"

#include "Horo/Debug/ConsoleVariableSet.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "RuntimeConsoleRecordList.h"
#include "RuntimeOverlayPanels.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects redacted/oversized recall instead of inserting a changed executable command. */
        bool Recallable(std::string_view command, std::size_t capacity) {
            return command.size() < capacity && command.find("[REDACTED]") == std::string_view::npos;
        }

        /** @brief Finds the shared prefix of validated ASCII descriptor names, without constructing display text. */
        std::string_view CommonPrefix(const std::string_view left, const std::string_view right) {
            std::size_t count{};
            while (count < left.size() && count < right.size() && left[count] == right[count])
                ++count;
            return left.substr(0, count);
        }
    }  // namespace

    /** @copydoc RuntimeConsoleView::RuntimeConsoleView */
    RuntimeConsoleView::RuntimeConsoleView(const RuntimeConsoleViewPorts &ports, const RuntimeConsoleViewPresentation &presentation)
        : LocalizedGui("runtime_console"), input_(ports.input), history_(ports.history), registry_(ports.registry),
          variables_(ports.variables), principal_(ports.principal),
          terminal_(ports.input, ports.executor, ports.history, ports.operations, true, ports.configuration),
          panels_(std::make_unique<RuntimeOverlayPanels>(ports.logs, ports.operations, ports.principal, presentation.overlays,
                                                         presentation.panels, ports.sceneInspection, ports.network)),
          capacity_(std::min(line_.size(), ports.registry.Limits().inputBytes + 1)), revision_(ports.history.Revision()),
          visible_(presentation.console), requestFocus_(presentation.console) {}

    /** @copydoc RuntimeConsoleView::~RuntimeConsoleView */
    RuntimeConsoleView::~RuntimeConsoleView() {
        terminal_.Close();
    }

    /** @copydoc RuntimeConsoleView::PublishScene */
    Result<void> RuntimeConsoleView::PublishScene(const Runtime::RuntimeSceneView *scene) {
        return panels_->PublishScene(scene);
    }

    /** @copydoc RuntimeConsoleView::SetVisible */
    void RuntimeConsoleView::SetVisible(const bool visible) {
        visibilityRequest_.reset();
        if (visible_ == visible)
            return;
        visible_ = visible;
        requestFocus_ = visible;
        if (visible)
            return;
        if (!panels_->AnyVisible()) {
            terminal_.Close();
            lease_ = false;
        }
        line_.fill(0);
        draft_.fill(0);
        replacement_.fill(0);
        search_.fill(0);
        selected_.reset();
        recallOffset_.reset();
        errorCode_.clear();
        completion_.clear();
        searchVisible_ = false;
    }

    /** @copydoc RuntimeConsoleView::RequestVisible */
    void RuntimeConsoleView::RequestVisible(const bool visible) {
        if (visible != visible_)
            visibilityRequest_ = visible;
    }

    /** @copydoc RuntimeConsoleView::TakeVisibilityRequest */
    std::optional<bool> RuntimeConsoleView::TakeVisibilityRequest() noexcept {
        return std::exchange(visibilityRequest_, std::nullopt);
    }

    /** @copydoc RuntimeConsoleView::ReportRequestFailure */
    void RuntimeConsoleView::ReportRequestFailure(const Error &error) {
        errorCode_ = error.code.Value().substr(0, 256);
    }

    /** @copydoc RuntimeConsoleView::ReportPanelRequestFailure */
    void RuntimeConsoleView::ReportPanelRequestFailure(const Error &error) {
        panels_->ReportRequestFailure(error);
    }

    /** @copydoc RuntimeConsoleView::SetPanelVisibility */
    Result<void> RuntimeConsoleView::SetPanelVisibility(const Debug::OverlaySource source, const bool visible) {
        return panels_->SetPanelVisibility(source, visible);
    }

    /** @copydoc RuntimeConsoleView::TakePanelVisibilityRequest */
    std::optional<Debug::OverlayVisibilityIntent> RuntimeConsoleView::TakePanelVisibilityRequest() noexcept {
        return panels_->TakeVisibilityRequest();
    }

    /** @copydoc RuntimeConsoleView::SetOverlaysVisible */
    void RuntimeConsoleView::SetOverlaysVisible(const bool visible) {
        panels_->SetVisible(visible);
        if (!AnyVisible()) {
            terminal_.Close();
            lease_ = false;
        }
    }

    /** @copydoc RuntimeConsoleView::OverlaysVisible */
    bool RuntimeConsoleView::OverlaysVisible() const noexcept {
        return panels_->Visible();
    }

    /** @copydoc RuntimeConsoleView::AnyVisible */
    bool RuntimeConsoleView::AnyVisible() const {
        return visible_ || panels_->AnyVisible();
    }

    /** @copydoc RuntimeConsoleView::SynchronizeFocus */
    void RuntimeConsoleView::SynchronizeFocus(const bool blocked) {
        if (!AnyVisible() || blocked) {
            terminal_.Close();
            lease_ = false;
            panels_->Suspend();
            return;
        }
        if (lease_) {
            if (!terminal_.IsOpen())
                panels_->Suspend();
            return;
        }
        if (input_.HasHigherPriorityContext(Input::InputContextKind::FocusedGuiWidget)) {
            panels_->Suspend();
            return;
        }
        const auto opened = terminal_.Open();
        if (opened.HasError()) {
            errorCode_ = opened.ErrorValue().code.Value().substr(0, 256);
            return;
        }
        lease_ = true;
        requestFocus_ = true;
        errorCode_.clear();
    }

    /** @copydoc RuntimeConsoleView::Visible */
    bool RuntimeConsoleView::Visible() const noexcept {
        return visible_;
    }

    /** @copydoc RuntimeConsoleView::Interactive */
    bool RuntimeConsoleView::Interactive() const noexcept {
        return AnyVisible() && terminal_.IsOpen();
    }

    /** @copydoc RuntimeConsoleView::Recall */
    std::optional<std::string_view> RuntimeConsoleView::Recall(const std::string_view command) {
        if (!Recallable(command, capacity_))
            return {};
        std::copy(command.begin(), command.end(), replacement_.begin());
        replacement_[command.size()] = '\0';
        return std::string_view(replacement_.data(), command.size());
    }

    /** @copydoc RuntimeConsoleView::Complete */
    std::optional<std::string_view> RuntimeConsoleView::Complete(const std::string_view text, const std::size_t cursor) {
        completion_.clear();
        recallOffset_.reset();
        if (cursor != text.size() || text.find_first_of(" \t\r\n") != std::string_view::npos)
            return {};
        const auto matches = registry_.Discover(text, principal_);
        std::string_view common;
        std::size_t count{};
        const auto append = [&](const std::string_view name) {
            if (!name.starts_with(text))
                return;
            if (completion_.size() + name.size() + 2 <= 1024) {
                if (!completion_.empty())
                    completion_ += ", ";
                completion_ += name;
            }
            if (count++ == 0)
                common = name;
            else
                common = CommonPrefix(common, name);
        };
        for (const auto &descriptor : matches)
            append(descriptor.name);
        const auto variableMatches =
            variables_ ? variables_->Discover(text, principal_, true) : std::vector<Debug::ConsoleVariableDescriptor>{};
        for (const auto &descriptor : variableMatches)
            append(descriptor.name);
        if (count == 0 || common.size() < text.size())
            return {};
        return Recall(common);
    }

    /** @copydoc RuntimeConsoleView::Navigate */
    std::optional<std::string_view> RuntimeConsoleView::Navigate(void *context, const Ui::InputTextNavigationAction action,
                                                                 const std::string_view text, const std::size_t cursor) {
        auto &view = *static_cast<RuntimeConsoleView *>(context);
        view.navigating_ = true;
        view.errorCode_.clear();
        switch (action) {
            case Ui::InputTextNavigationAction::Complete:
                return view.Complete(text, cursor);
            case Ui::InputTextNavigationAction::Previous:
                return view.Previous(text);
            case Ui::InputTextNavigationAction::Next:
                return view.Next();
        }
        return {};
    }

    /** @copydoc RuntimeConsoleView::Previous */
    std::optional<std::string_view> RuntimeConsoleView::Previous(const std::string_view text) {
        if (!recallOffset_) {
            if (text.size() >= draft_.size())
                return {};
            std::copy(text.begin(), text.end(), draft_.begin());
            draft_[text.size()] = '\0';
        }
        const auto rows = history_.Entries();
        const auto start = recallOffset_ ? *recallOffset_ + 1 : 0;
        for (std::size_t offset = start; offset < rows.size(); ++offset) {
            if (auto recalled = Recall(rows[rows.size() - 1 - offset].command)) {
                recallOffset_ = offset;
                return recalled;
            }
        }
        return {};
    }

    /** @copydoc RuntimeConsoleView::Next */
    std::optional<std::string_view> RuntimeConsoleView::Next() {
        if (!recallOffset_)
            return {};
        const auto rows = history_.Entries();
        for (auto offset = *recallOffset_; offset > 0;) {
            --offset;
            if (offset < rows.size()) {
                if (auto recalled = Recall(rows[rows.size() - 1 - offset].command)) {
                    recallOffset_ = offset;
                    return recalled;
                }
            }
        }
        recallOffset_.reset();
        return std::string_view(draft_.data());
    }

    /** @copydoc RuntimeConsoleView::Submit */
    void RuntimeConsoleView::Submit() {
        if (!visible_ || !Interactive() || line_[0] == '\0')
            return;
        const auto submitted = terminal_.Submit(line_.data(), principal_);
        if (submitted.HasError()) {
            errorCode_ = submitted.ErrorValue().code.Value().substr(0, 256);
            return;
        }
        line_.fill(0);
        draft_.fill(0);
        recallOffset_.reset();
        completion_.clear();
        errorCode_.clear();
        requestFocus_ = true;
    }

    /** @copydoc RuntimeConsoleView::DrawBackdrop */
    void RuntimeConsoleView::DrawBackdrop() {
        const auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowBgAlpha(0.58F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0, 0, 0, 1});
        ImGui::Begin("##RuntimeConsoleBackdrop", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::End();
        ImGui::PopStyleColor();
    }

    /** @copydoc RuntimeConsoleView::DrawSearch */
    void RuntimeConsoleView::DrawSearch(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (!searchVisible_)
            return;
        static_cast<void>(Ui::InputTextControl("##runtime.history.search", search_.data(), search_.size(), fonts,
                                               {.hint = LocalizedText(localization, "history_hint").c_str(),
                                                .componentSize = Ui::ComponentSize::Medium}));
        const auto *match = history_.ReverseSearch(search_.data());
        if (match)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, match->command, Theme::Muted(), fonts);
        if (Ui::Button({.label = LocalizedText(localization, "restore").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = match && Recallable(match->command, capacity_),
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const auto recalled = Recall(match->command);
            if (recalled) {
                std::copy(recalled->begin(), recalled->end(), line_.begin());
                line_[recalled->size()] = '\0';
                requestFocus_ = true;
                recallOffset_.reset();
                errorCode_.clear();
                completion_.clear();
            }
        }
        if (match && !Recallable(match->command, capacity_))
            Ui::Hint(LocalizedText(localization, "sensitive").c_str(), fonts);
    }

    /** @copydoc RuntimeConsoleView::DrawDetails */
    void RuntimeConsoleView::DrawDetails(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (!selected_ || *selected_ >= history_.Entries().size())
            return;
        const auto &entry = history_.Entries()[*selected_];
        std::string_view output = entry.output;
        if (entry.operationUnavailable)
            output = LocalizedText(localization, "unavailable");
        else if (entry.HasPendingResult())
            output = LocalizedText(localization, "pending");
        Ui::SectionTitle(LocalizedText(localization, "details").c_str(), fonts);
        if (ImGui::BeginChild("##runtime.result.details", {0, Ui::ScaledLayoutValue(120)}, ImGuiChildFlags_Borders)) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, entry.command, Theme::Text(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, output, Theme::Muted(), fonts);
        }
        ImGui::EndChild();
        if (Ui::Button({.label = LocalizedText(localization, "copy").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = !entry.output.empty() && !entry.HasPendingResult() && !entry.operationUnavailable,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            ImGui::SetClipboardText(entry.output.c_str());
    }

    /** @copydoc RuntimeConsoleView::DrawContents */
    void RuntimeConsoleView::DrawContents(const ILocalizationService &localization, const Theme::Fonts &fonts) {
        if (history_.Revision() != revision_) {
            revision_ = history_.Revision();
            selected_.reset();
            recallOffset_.reset();
        }
        const std::array actions{Ui::CardTitleBarAction{.id = "runtime.close",
                                                        .icon = Ui::UiIcon::VisibilityOff,
                                                        .title = LocalizedText(localization, "close").c_str(),
                                                        .onInvoke = [this] {
            RequestVisible(false);
        }}};
        Ui::Card card({.id = "runtime.console.card"});
        card.DrawTitleBar({"runtime.console.title", LocalizedText(localization, "title").c_str(), fonts, actions});
        if (!card.BeginBody() || !visible_)
            return;
        const auto list =
            Ui::RuntimeConsoleRecordList({.rows = history_.Entries(),
                                          .revision = history_.Revision(),
                                          .empty = LocalizedText(localization, "empty"),
                                          .pending = LocalizedText(localization, "pending"),
                                          .unavailable = LocalizedText(localization, "unavailable"),
                                          .selected = selected_,
                                          .height = std::max(Ui::ScaledLayoutValue(80), ImGui::GetMainViewport()->WorkSize.y * 0.25F)},
                                         fonts);
        if (list.activated)
            selected_ = list.activated;
        DrawDetails(localization, fonts);
        Ui::InputTextNavigation navigation{this, Navigate};
        if (requestFocus_) {
            ImGui::SetKeyboardFocusHere();
            requestFocus_ = false;
        }
        navigating_ = false;
        const bool entered = Ui::InputTextControl("##runtime.command", line_.data(), capacity_, fonts,
                                                  {.error = !errorCode_.empty(),
                                                   .hint = LocalizedText(localization, "hint").c_str(),
                                                   .componentSize = Ui::ComponentSize::Medium,
                                                   .submitOnEnter = true,
                                                   .navigation = &navigation});
        if (ImGui::IsItemEdited() && !navigating_) {
            recallOffset_.reset();
            completion_.clear();
            errorCode_.clear();
        }
        const bool clicked = Ui::Button({.label = LocalizedText(localization, "send").c_str(),
                                         .enabled = line_[0] != '\0',
                                         .style = {.width = Ui::StyleWidth::FillAvailable}});
        if (entered || clicked)
            Submit();
        if (!errorCode_.empty()) {
            Ui::ErrorText(LocalizedText(localization, "failed").c_str(), fonts);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, errorCode_, Theme::Err(), fonts);
        }
        if (!completion_.empty())
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, completion_, Theme::Muted(), fonts);
        if (Ui::Button({.label = LocalizedText(localization, "history").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .style = {.width = Ui::StyleWidth::FillAvailable}}))
            searchVisible_ = !searchVisible_;
        DrawSearch(localization, fonts);
        Ui::Hint(LocalizedText(localization, "shortcuts").c_str(), fonts);
    }

    /** @copydoc RuntimeConsoleView::Draw */
    void RuntimeConsoleView::Draw(const ILocalizationService &localization, const Theme::Fonts &fonts, const bool blocked,
                                  const std::chrono::steady_clock::time_point now) {
        SynchronizeFocus(blocked);
        if (!Interactive())
            return;
        static_cast<void>(terminal_.TakeText());
        DrawBackdrop();
        if (panels_->Draw(localization, fonts, now)) {
            RequestVisible(false);
            SetOverlaysVisible(false);
        }
        SynchronizeFocus(blocked);
        if (!visible_ || !Interactive())
            return;
        const auto *viewport = ImGui::GetMainViewport();
        const ImVec2 size{viewport->WorkSize.x * 0.85F, viewport->WorkSize.y * 0.75F};
        ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x * 0.5F, viewport->WorkPos.y + viewport->WorkSize.y},
                                ImGuiCond_FirstUseEver, {0.5F, 1});
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints({std::min(Ui::ScaledLayoutValue(320), viewport->WorkSize.x),
                                             std::min(Ui::ScaledLayoutValue(240), viewport->WorkSize.y)},
                                            viewport->WorkSize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg1());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        if (ImGui::Begin("##RuntimeConsoleHost", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
            DrawContents(localization, fonts);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
                RequestVisible(false);
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
    }
}  // namespace Horo::Editor
