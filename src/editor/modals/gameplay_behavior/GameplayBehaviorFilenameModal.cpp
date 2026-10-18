#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Paths.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <imgui.h>
#include <system_error>
#include <utility>

namespace Horo::Editor {
    namespace {
        const char *ExtensionFor(const GameplayBehaviorKind kind) {
            return kind == GameplayBehaviorKind::Lua ? ".horo_script" : ".cpp";
        }
    }  // namespace

    GameplayBehaviorFilenameModal::GameplayBehaviorFilenameModal(const EditorGuiContext &context, const GameplayBehaviorKind kind,
                                                                 std::string destination, std::string baseName,
                                                                 std::filesystem::path projectRoot, BrowseCallback onBrowse,
                                                                 CreateCallback onCreate)
        : context_(context), state_(kind, std::move(destination), std::move(baseName)), projectRoot_(std::move(projectRoot)),
          luaDestination_(kind == GameplayBehaviorKind::Lua ? state_.Destination() : ProjectLayout::ScriptsRoot(projectRoot_)),
          onBrowse_(std::move(onBrowse)), onCreate_(std::move(onCreate)) {}

    ModalId GameplayBehaviorFilenameModal::Id() const {
        return ModalId{kModalId};
    }

    ModalPresentation GameplayBehaviorFilenameModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact, .dimWorkspace = true};
    }

    ModalClosePolicy GameplayBehaviorFilenameModal::ClosePolicy() const {
        return {.allowCloseButton = true, .allowEscape = true, .allowOutsideClick = false, .allowApplicationShutdown = true};
    }

    Result<void> GameplayBehaviorFilenameModal::OnOpen(EditorModalContext &) {
        RefreshAvailability();
        return Result<void>::Success();
    }

    const std::string &GameplayBehaviorFilenameModal::Text(const char *key) const {
        return context_.localization.Get("editor", key);
    }

    void GameplayBehaviorFilenameModal::RefreshAvailability() {
        nameAvailable_ = false;
        if (state_.Validation().HasError())
            return;
        std::error_code error;
        const std::filesystem::path source =
            std::filesystem::path{state_.Destination()} / (state_.BaseName() + ExtensionFor(state_.Kind()));
        nameAvailable_ = !std::filesystem::exists(source, error) && !error;
    }

    void GameplayBehaviorFilenameModal::DrawLanguage() {
        const auto &fonts = context_.theme.fonts;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text("workspace.gameplay_behavior.create.type"), Theme::Text(), fonts);
        const std::array languages{Text("workspace.gameplay_behavior.create.lua").c_str(),
                                   Text("workspace.gameplay_behavior.create.native").c_str()};
        int selected = state_.Kind() == GameplayBehaviorKind::Lua ? 0 : 1;
        ImGui::PushItemWidth(-1.0F);
        const bool changed = Ui::ComboControl("##GameplayBehaviorLanguage", &selected, languages.data(), static_cast<int>(languages.size()),
                                              fonts, {.height = 42.0F});
        ImGui::PopItemWidth();
        if (changed) {
            const GameplayBehaviorKind kind = selected == 0 ? GameplayBehaviorKind::Lua : GameplayBehaviorKind::Native;
            const std::filesystem::path destination =
                kind == GameplayBehaviorKind::Lua ? luaDestination_ : projectRoot_ / "source" / "gameplay";
            state_.SetKind(kind, destination.string());
            destinationError_ = false;
            RefreshAvailability();
        }
    }

    void GameplayBehaviorFilenameModal::DrawName() {
        const auto &fonts = context_.theme.fonts;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text("workspace.gameplay_behavior.create.filename"), Theme::Text(), fonts);
        const char *extension = ExtensionFor(state_.Kind());
        ImVec2 suffixTextSize;
        {
            Theme::ScopedTextStyle textStyle(fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            suffixTextSize = ImGui::CalcTextSize(extension);
        }
        const float suffixPadding = Ui::ScaledLayoutValue(12.0F);
        const float suffixWidth = suffixTextSize.x + 2.0F * suffixPadding;
        const float inputWidth =
            std::max(Ui::ScaledLayoutValue(120.0F), ImGui::GetContentRegionAvail().x - suffixWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginGroup();
        if (Ui::InputTextControl("##GameplayBehaviorFilename", state_.MutableBaseName(), 256, fonts,
                                 {.error = state_.Validation().HasError(),
                                  .width = inputWidth / Theme::GetActiveTokens().sizes.uiScale,
                                  .height = 42.0F})) {
            state_.SetBaseName(state_.BaseName());
            RefreshAvailability();
        }
        const float inputHeight = ImGui::GetItemRectSize().y;
        ImGui::SameLine();
        {
            Ui::ScopedCard suffix("##GameplayBehaviorExtension", {suffixWidth, inputHeight}, suffixPadding,
                                  std::max(0.0F, (inputHeight - suffixTextSize.y) * 0.5F), Theme::Bg3());
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, extension, Theme::Muted(), fonts);
        }
        ImGui::EndGroup();
        const char *statusKey = state_.Validation().HasError() ? "workspace.gameplay_behavior.create.invalid_filename"
                                : nameAvailable_               ? "workspace.gameplay_behavior.create.available"
                                                               : "workspace.gameplay_behavior.create.name_exists";
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(statusKey),
                        state_.Validation().HasValue() && nameAvailable_ ? Theme::Ok() : Theme::Err(), fonts);
    }

    void GameplayBehaviorFilenameModal::BrowseDestination() {
        if (!onBrowse_ || state_.Kind() != GameplayBehaviorKind::Lua)
            return;
        const std::optional<std::filesystem::path> chosen = onBrowse_(Text("workspace.gameplay_behavior.create.browse_title"));
        if (!chosen)
            return;
        std::error_code error;
        const std::filesystem::path scriptsRoot = std::filesystem::weakly_canonical(ProjectLayout::ScriptsRoot(projectRoot_), error);
        if (error) {
            destinationError_ = true;
            return;
        }
        const std::filesystem::path destination = std::filesystem::weakly_canonical(*chosen, error);
        if (error || !std::filesystem::is_directory(destination, error) || error ||
            !Foundation::Paths::HasPathPrefix(scriptsRoot, destination)) {
            destinationError_ = true;
            return;
        }
        destinationError_ = false;
        luaDestination_ = destination;
        state_.SetDestination(destination.string());
        RefreshAvailability();
    }

    void GameplayBehaviorFilenameModal::DrawDestination() {
        const auto &fonts = context_.theme.fonts;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text("workspace.gameplay_behavior.create.destination"), Theme::Text(), fonts);
        const float browseWidth = Ui::ScaledLayoutValue(124.0F);
        const float gap = Ui::ScaledLayoutValue(10.0F);
        const float pathWidth = std::max(Ui::ScaledLayoutValue(130.0F), ImGui::GetContentRegionAvail().x - browseWidth - gap);
        const std::filesystem::path relative = std::filesystem::path{state_.Destination()}.lexically_relative(projectRoot_);
        std::string displayPath = state_.Destination();
        if (!relative.empty()) {
            displayPath.clear();
            for (const auto &component : relative) {
                if (!displayPath.empty())
                    displayPath += " / ";
                displayPath += component.string();
            }
        }
        {
            Ui::ScopedCard path("##GameplayBehaviorDestination", {pathWidth, Ui::ScaledLayoutValue(42.0F)}, Ui::ScaledLayoutValue(12.0F),
                                Ui::ScaledLayoutValue(9.0F), Theme::Bg3());
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, displayPath, Theme::Text(), fonts);
        }
        ImGui::SameLine(0.0F, gap);
        if (Ui::Button({.label = Text("workspace.gameplay_behavior.create.browse").c_str(),
                        .size = {browseWidth / Theme::GetActiveTokens().sizes.uiScale, 42.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = state_.Kind() == GameplayBehaviorKind::Lua && static_cast<bool>(onBrowse_),
                        .font = fonts.sans}))
            BrowseDestination();
        if (destinationError_)
            Ui::ErrorText(Text("workspace.gameplay_behavior.create.invalid_destination").c_str(), fonts);
    }

    void GameplayBehaviorFilenameModal::DrawTemplate() {
        const auto &fonts = context_.theme.fonts;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text("workspace.gameplay_behavior.create.template"), Theme::Text(), fonts);
        const std::array templates{Text("workspace.gameplay_behavior.create.basic_template").c_str()};
        int selected = 0;
        ImGui::PushItemWidth(-1.0F);
        static_cast<void>(Ui::ComboControl("##GameplayBehaviorTemplate", &selected, templates.data(), 1, fonts, {.height = 42.0F}));
        ImGui::PopItemWidth();
    }

    ModalFrameResult GameplayBehaviorFilenameModal::Draw() {
        const std::string title = Text("workspace.gameplay_behavior.create.title");
        Ui::ScopedModalShell modal({.id = "GameplayBehaviorFilename",
                                    .title = title.c_str(),
                                    .requestedSize = {620.0F, 532.0F},
                                    .viewportPadding = 48.0F,
                                    .minimumWidth = 460.0F,
                                    .minimumHeight = 400.0F,
                                    .footerHeight = 68.0F,
                                    .showClose = true,
                                    .foregroundBorder = true,
                                    .titleFontSize = Theme::TextPx::Title()},
                                   context_.theme.fonts);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{Ui::ScaledLayoutValue(20.0F), Ui::ScaledLayoutValue(16.0F)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::BeginChild("##GameplayBehaviorBody", {0.0F, modal.BodyHeight()}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        DrawLanguage();
        ImGui::Dummy({0.0F, Ui::ScaledLayoutValue(4.0F)});
        DrawName();
        ImGui::Dummy({0.0F, Ui::ScaledLayoutValue(4.0F)});
        DrawDestination();
        ImGui::Dummy({0.0F, Ui::ScaledLayoutValue(4.0F)});
        DrawTemplate();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        modal.BeginFooter({Ui::ScaledLayoutValue(24.0F), Ui::ScaledLayoutValue(14.0F)});
        const std::string cancel = Text("workspace.gameplay_behavior.create.cancel");
        const std::string create = Text("workspace.gameplay_behavior.create.confirm");
        const float buttonWidth =
            std::max(Ui::ScaledLayoutValue(96.0F),
                     std::max(ImGui::CalcTextSize(cancel.c_str()).x, ImGui::CalcTextSize(create.c_str()).x) + Ui::ScaledLayoutValue(32.0F));
        const float gap = Ui::ScaledLayoutValue(12.0F);
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 2.0F * buttonWidth - gap));
        if (Ui::Button({.label = cancel.c_str(),
                        .size = {buttonWidth / Theme::GetActiveTokens().sizes.uiScale, 0.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context_.theme.fonts.sans}))
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        ImGui::SameLine(0.0F, gap);
        const bool confirm = Ui::Button({.label = create.c_str(),
                                         .size = {buttonWidth / Theme::GetActiveTokens().sizes.uiScale, 0.0F},
                                         .enabled = state_.Validation().HasValue() && nameAvailable_ && !destinationError_,
                                         .font = context_.theme.fonts.sans});
        modal.EndFooter();

        if (modal.CloseRequested())
            return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
        if (confirm) {
            RefreshAvailability();
            if (!nameAvailable_)
                return ModalFrameResult::None();
            if (const auto request = state_.Confirm(); request.has_value()) {
                if (onCreate_)
                    onCreate_(*request);
                return ModalFrameResult::RequestClose(ModalCloseReason::Completed);
            }
        }
        return ModalFrameResult::None();
    }

    CloseDecision GameplayBehaviorFilenameModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    const GameplayBehaviorFilenameModalState &GameplayBehaviorFilenameModal::State() const noexcept {
        return state_;
    }
}  // namespace Horo::Editor
