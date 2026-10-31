#include "editor/modals/play_topology/PlayTopologyModal.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <charconv>

namespace Horo::Editor {
    namespace {
        /** @brief Decode inert numeric references; never resolve a provider or credential from presentation. */
        std::optional<std::uint64_t> Identity(const std::string_view value) {
            if (value.empty())
                return std::uint64_t{0};
            std::uint64_t number{};
            if (const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return std::nullopt;
            return number;
        }

        /** @brief Translate bounded network controls into one owned draft before issuing an application command. */
        bool ApplyNetworkDraft(Application::PlayTopologyProfile &profile, const int clients, const int port,
                               const std::string &providerText, const std::string &presetText) {
            if (clients < 0 || clients > 8 || port < 1 || port > 65535)
                return false;
            const auto provider = Identity(providerText);
            const auto preset = Identity(presetText);
            if (!provider.has_value() || !preset.has_value() || *provider == 0)
                return false;
            profile.serverCount = 1;
            profile.clientCount = static_cast<std::uint8_t>(clients);
            profile.port = static_cast<std::uint16_t>(port);
            profile.transport = GuiLive::HostSymbol<&Network::NetworkTransportProviderId::Create>()(*provider).Value();
            profile.simulationPreset = *preset;
            return true;
        }

        /** @brief Map declared errors to localized UI copy without rendering private filesystem diagnostics. */
        const char *ErrorKey(const Error &error) {
            if (error.code.Value() == GuiLive::HostSymbol<&Application::PlayTopologyErrors::Stale>().code.Value())
                return "stale";
            if (error.code.Value() == GuiLive::HostSymbol<&Application::PlayTopologyErrors::Invalid>().code.Value())
                return "invalid";
            return "storage";
        }
    }  // namespace

    /** @copydoc PlayTopologyModal::PlayTopologyModal */
    PlayTopologyModal::PlayTopologyModal(const EditorGuiContext &context, std::unique_ptr<Application::PlayTopologyStore> store)
        : EditorModal(kTextScope), context_(context), store_(std::move(store)) {}

    ModalId PlayTopologyModal::Id() const {
        return ModalId{0x504C4159544F504FULL};
    }

    ModalPresentation PlayTopologyModal::Presentation() const {
        return {.size = ModalSizePolicy::Large};
    }

    ModalClosePolicy PlayTopologyModal::ClosePolicy() const {
        return {};
    }

    Result<void> PlayTopologyModal::OnOpen(EditorModalContext &) {
        if (!store_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&Application::PlayTopologyErrors::Disabled>()));
        auto loaded = store_->Reload();
        loaded_ = loaded.HasValue();
        if (loaded_) {
            projection_ = store_->Project();
            Select(projection_.profiles.empty() ? -1 : 0);
        } else
            errorKey_ = ErrorKey(loaded.ErrorValue());
        return Result<void>::Success();
    }

    CloseDecision PlayTopologyModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @brief Fetch one complete localized string; dynamic suffixes come only from a closed compiled vocabulary. */
    const std::string &PlayTopologyModal::Text(const char *suffix) const {
        return LocalizedText(context_.localization, suffix);
    }

    /** @brief Replace the transient draft from one immutable projection; cancellation never persists it. */
    void PlayTopologyModal::Select(const int index) {
        selected_ = index;
        draft_ = {};
        if (index >= 0 && static_cast<std::size_t>(index) < projection_.profiles.size())
            draft_ = projection_.profiles[static_cast<std::size_t>(index)];
        else {
            draft_.id = 1;
            while (std::ranges::find(projection_.profiles, draft_.id, &Application::PlayTopologyProfile::id) != projection_.profiles.end())
                ++draft_.id;
        }
        kind_ = static_cast<int>(draft_.kind);
        clients_ = draft_.clientCount;
        port_ = draft_.port == 0 ? 7777 : draft_.port;
        transport_ = draft_.transport.IsValid() ? std::to_string(draft_.transport.Value()) : "";
        simulation_ = draft_.simulationPreset == 0 ? "" : std::to_string(draft_.simulationPreset);
        errorKey_.clear();
    }

    /** @brief Build a complete validated typed command; reject negative/narrowing input before conversion. */
    std::optional<Application::PlayTopologyProfile> PlayTopologyModal::Draft() const {
        if (!loaded_ || kind_ < 0 || kind_ >= static_cast<int>(Application::PlayTopologyKind::Count))
            return std::nullopt;
        auto profile = draft_;
        profile.kind = static_cast<Application::PlayTopologyKind>(kind_);
        if (profile.kind == Application::PlayTopologyKind::Standalone) {
            profile.serverCount = 0;
            profile.clientCount = 0;
            profile.port = 0;
            profile.transport = {};
            profile.simulationPreset = 0;
        } else if (!ApplyNetworkDraft(profile, clients_, port_, transport_, simulation_))
            return std::nullopt;
        return Application::ValidatePlayTopology(profile).HasValue() ? std::optional{std::move(profile)} : std::nullopt;
    }

    namespace {
        /** @brief Uses equal field columns when both can retain readable labels; narrow forms stack in source order. */
        template <typename Left, typename Right> void FieldPair(const char *id, Left &&left, Right &&right) {
            if (ImGui::GetContentRegionAvail().x < 500.0F) {
                left();
                right();
                return;
            }
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{8, 4});
            if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                left();
                ImGui::TableNextColumn();
                // The previous cell's input baseline must not shift this cell's label.
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ImGui::GetStyle().ItemSpacing.x, 0});
                ImGui::Dummy({0, 0});
                ImGui::PopStyleVar();
                right();
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }
    }  // namespace

    /** @brief Renders bounded portable fields in responsive pairs without selecting transport implementations. */
    void PlayTopologyModal::DrawFields() {
        const auto &fonts = context_.theme.fonts;
        FieldPair("##TopologyIdentity", [&] {
            Ui::FieldLabel(Text("name").c_str(), fonts);
            (void)Ui::InputTextControl("##TopologyName", draft_.name, 97, fonts);
        }, [&] {
            Ui::FieldLabel(Text("map").c_str(), fonts);
            (void)Ui::InputTextControl("##TopologyMap", draft_.map, 257, fonts);
        });
        FieldPair("##TopologyModeClients", [&] {
            const std::array kinds{Text("standalone").c_str(), Text("listen").c_str(), Text("dedicated").c_str()};
            Ui::FieldLabel(Text("kind").c_str(), fonts);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            (void)Ui::ComboControl("##TopologyKind", &kind_, kinds.data(), static_cast<int>(kinds.size()), fonts);
        }, [&] {
            ImGui::BeginDisabled(kind_ == static_cast<int>(Application::PlayTopologyKind::Standalone));
            Ui::FieldLabel(Text("clients").c_str(), fonts);
            Ui::InputIntControl("##TopologyClients", &clients_, fonts);
            ImGui::EndDisabled();
        });
        ImGui::BeginDisabled(kind_ == static_cast<int>(Application::PlayTopologyKind::Standalone));
        FieldPair("##TopologyConnection", [&] {
            Ui::FieldLabel(Text("transport").c_str(), fonts);
            (void)Ui::InputTextControl("##TopologyTransport", transport_, 21, fonts);
        }, [&] {
            Ui::FieldLabel(Text("port").c_str(), fonts);
            Ui::InputIntControl("##TopologyPort", &port_, fonts);
        });
        Ui::FieldLabel(Text("simulation").c_str(), fonts);
        (void)Ui::InputTextControl("##TopologySimulation", simulation_, 21, fonts);
        Ui::Hint(Text("simulation_hint").c_str(), fonts);
        ImGui::EndDisabled();
    }

    /** @brief Commit one portable profile; retain draft and projection when persistence fails. */
    void PlayTopologyModal::SaveProject() {
        const auto profile = Draft();
        if (!profile) {
            errorKey_ = "invalid";
            return;
        }
        auto saved = store_->SaveProfile(projection_.revision, *profile);
        if (saved.HasError()) {
            errorKey_ = ErrorKey(saved.ErrorValue());
            return;
        }
        projection_ = std::move(saved).Value();
        const auto selected = std::ranges::find(projection_.profiles, profile->id, &Application::PlayTopologyProfile::id);
        Select(static_cast<int>(selected - projection_.profiles.begin()));
    }

    /** @brief Selects or starts a transient portable profile; the footer owns the save action. */
    void PlayTopologyModal::DrawProject() {
        const auto &fonts = context_.theme.fonts;
        const auto metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        float newWidth{};
        {
            Theme::ScopedTextStyle text(fonts.sans, metrics.fontSize, Theme::FontPx::Sans);
            newWidth = ImGui::CalcTextSize(Text("new").c_str()).x + metrics.paddingX * 2;
        }
        if (!projection_.profiles.empty()) {
            Ui::FieldLabel(Text("profile").c_str(), fonts);
            const Ui::ComboItemSource source{.label = [this](int index) {
                return projection_.profiles[static_cast<std::size_t>(index)].name.c_str();
            }};
            const bool sameRow = ImGui::GetContentRegionAvail().x >= newWidth + 180;
            ImGui::SetNextItemWidth(sameRow ? ImGui::GetContentRegionAvail().x - newWidth - ImGui::GetStyle().ItemSpacing.x
                                            : ImGui::GetContentRegionAvail().x);
            if (Ui::ComboControl("##TopologySelect", &selected_, static_cast<int>(projection_.profiles.size()), source, fonts))
                Select(selected_);
            if (sameRow)
                ImGui::SameLine();
        } else {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - newWidth));
        }
        if (Ui::Button({.label = Text("new").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = projection_.profiles.size() < 16,
                        .font = fonts.sans}))
            Select(-1);
        DrawFields();
    }

    /** @brief Keeps measured save/close actions bounded and right aligned, stacking only on narrow viewports. */
    bool PlayTopologyModal::DrawFooter() {
        const auto &fonts = context_.theme.fonts;
        const auto metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        Theme::ScopedTextStyle text(fonts.sans, metrics.fontSize, Theme::FontPx::Sans);
        const float closeWidth = ImGui::CalcTextSize(Text("close").c_str()).x + metrics.paddingX * 2;
        const float saveWidth = ImGui::CalcTextSize(Text("save_project").c_str()).x + metrics.paddingX * 2;
        const float total = closeWidth + saveWidth + ImGui::GetStyle().ItemSpacing.x;
        const bool stacked = total > ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - (stacked ? closeWidth : total)));
        const bool close = Ui::Button({.label = Text("close").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = fonts.sans});
        if (stacked)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - saveWidth));
        else
            ImGui::SameLine();
        if (Ui::Button({.label = Text("save_project").c_str(), .enabled = loaded_ && Draft().has_value(), .font = fonts.sans}))
            SaveProject();
        return close;
    }

    /** @brief Replace the portable profile projection only after complete successful reloading. */
    void PlayTopologyModal::Reload() {
        const auto loaded = store_->Reload();
        if (loaded.HasValue()) {
            loaded_ = true;
            projection_ = store_->Project();
            Select(projection_.profiles.empty() ? -1 : 0);
        } else
            errorKey_ = ErrorKey(loaded.ErrorValue());
    }

    ModalFrameResult PlayTopologyModal::Draw() {
        Ui::ScopedModalShell shell({.id = "PlayTopologyProfiles",
                                    .title = Text("title").c_str(),
                                    .requestedSize = {720, 500},
                                    .viewportPadding = 32,
                                    .minimumWidth = 280,
                                    .minimumHeight = 240,
                                    .headerHeight = 38,
                                    .headerHorizontalPadding = 13,
                                    .footerHeight = ImGui::GetMainViewport()->WorkSize.x < 500 ? 100.0F : 60.0F,
                                    .foregroundBorder = true,
                                    .closeButtonVariant = Ui::CloseButtonVariant::Compact,
                                    .titleFontSize = Theme::TextPx::Label()},
                                   context_.theme.fonts);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{20, 14});
        // Reserve the scroll lane before the first layout, including narrow stacked forms.
        ImGui::BeginChild("##TopologyBody", {0, shell.BodyHeight()}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PushTextWrapPos(0);
        ImGui::BeginDisabled(!loaded_);
        DrawProject();
        ImGui::EndDisabled();
        if (!errorKey_.empty()) {
            Ui::ErrorText(Text(errorKey_.c_str()).c_str(), context_.theme.fonts);
            if (Ui::Button({.label = Text("reload").c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context_.theme.fonts.sans}))
                Reload();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        shell.BeginFooter({20, 12});
        const bool close = DrawFooter();
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
